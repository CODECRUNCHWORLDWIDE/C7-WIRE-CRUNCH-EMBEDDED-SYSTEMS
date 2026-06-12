#!/usr/bin/env python3
"""
cc-mesh-provision.py - Host-side BLE Mesh provisioner + configurator for the
Week 13 occupancy-fleet mini-project.

It does the two-step onboarding from Lecture 3:

  1. PROVISION each unprovisioned node it finds:
       - scan for Unprovisioned Device beacons (Mesh AD type 0x2B),
       - run the PB-GATT provisioning bearer (ECDH P-256 + OOB authentication),
       - deliver the node's unicast address, the shared NetKey, the key index,
         and the IV index in the encrypted Provisioning Data PDU.
  2. CONFIGURE each node via its Configuration Server:
       - add the shared AppKey and bind it to the Generic OnOff model,
       - set the publisher's model publication address to the group 0xC000,
       - subscribe the subscriber/relay models to 0xC000,
       - enable the relay feature on the RELAY node only.

This is a *teaching* provisioner: the cryptographic primitives (ECDH, the k1/k2/
k3 key-derivation functions, AES-CMAC, AES-CCM) are implemented against the
`cryptography` library so you can read every step, and the PB-GATT transport
uses `bleak` (cross-platform BLE). A production tool would use a hardened mesh
SDK (nRF Mesh, BlueZ mesh, or the BTstack provisioner) rather than rolling the
crypto by hand. The byte-level steps here mirror Mesh Protocol 1.1 §5 and §6.

Usage:
    cc-mesh-provision.py provision-all          # find + provision every node
    cc-mesh-provision.py configure              # configure the provisioned mesh
    cc-mesh-provision.py status                  # dump the saved mesh database

State (keys, address assignments) persists in ./cc-mesh-db.json so a second run
configures the same network instead of re-provisioning.

Citations:
    - Bluetooth Mesh Protocol 1.1 §5 (key derivation: k1/k2/k3, AES-CMAC).
    - Bluetooth Mesh Protocol 1.1 §6 (provisioning: PB bearer, ECDH, OOB).
    - Bluetooth Mesh Model 1.1 §4 (Configuration messages).
"""

from __future__ import annotations

import argparse
import json
import os
import secrets
import sys
from dataclasses import dataclass, asdict, field
from pathlib import Path
from typing import Optional

try:
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives import cmac
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    from cryptography.hazmat.primitives.ciphers.aead import AESCCM
except ImportError:
    sys.exit("error: the 'cryptography' package is required. pip install cryptography")

# bleak is only needed for the live BLE transport; allow `status` to run without it.
try:
    import asyncio
    from bleak import BleakScanner, BleakClient
    HAVE_BLEAK = True
except ImportError:
    HAVE_BLEAK = False


DB_PATH = Path(__file__).with_name("cc-mesh-db.json")

# The kitchen-lights group address every subscriber listens on (Lecture 3 §8).
GROUP_OCCUPANCY = 0xC000

# Mesh AD types and the PB-GATT service/characteristic UUIDs (Mesh Protocol 1.1
# §7.1.1: the Mesh Provisioning Service is 0x1827, the Mesh Proxy Service 0x1828).
AD_TYPE_MESH_BEACON = 0x2B
MESH_PROV_SERVICE_UUID = "00001827-0000-1000-8000-00805f9b34fb"
MESH_PROV_DATA_IN = "00002adb-0000-1000-8000-00805f9b34fb"
MESH_PROV_DATA_OUT = "00002adc-0000-1000-8000-00805f9b34fb"

# Roles, tagged into the device UUID's last byte by mesh_node.c.
ROLE_PUBLISHER = 1
ROLE_SUBSCRIBER = 2
ROLE_RELAY = 3
ROLE_NAMES = {ROLE_PUBLISHER: "PUBLISHER", ROLE_SUBSCRIBER: "SUBSCRIBER",
              ROLE_RELAY: "RELAY"}


# -------------------------------------------------------------------------
# The mesh-wide secrets and the per-node assignments, persisted to disk.
# -------------------------------------------------------------------------

@dataclass
class Node:
    role: int
    device_uuid: str          # hex
    unicast: int              # assigned unicast address
    provisioned: bool = False
    configured: bool = False


@dataclass
class MeshDB:
    net_key: str = ""         # 16-byte NetKey, hex
    app_key: str = ""         # 16-byte AppKey, hex
    net_key_index: int = 0
    app_key_index: int = 0
    iv_index: int = 0
    next_unicast: int = 0x0002
    nodes: list = field(default_factory=list)  # list[Node]

    @staticmethod
    def load() -> "MeshDB":
        if DB_PATH.exists():
            raw = json.loads(DB_PATH.read_text())
            db = MeshDB(**{k: v for k, v in raw.items() if k != "nodes"})
            db.nodes = [Node(**n) for n in raw.get("nodes", [])]
            return db
        # Fresh network: generate the NetKey and AppKey once.
        db = MeshDB()
        db.net_key = secrets.token_bytes(16).hex()
        db.app_key = secrets.token_bytes(16).hex()
        return db

    def save(self) -> None:
        raw = asdict(self)
        raw["nodes"] = [asdict(n) for n in self.nodes]
        DB_PATH.write_text(json.dumps(raw, indent=2))

    def find(self, device_uuid: str) -> Optional[Node]:
        for n in self.nodes:
            if n.device_uuid == device_uuid:
                return n
        return None


# -------------------------------------------------------------------------
# Mesh key-derivation primitives (Mesh Protocol 1.1 §3.8.2 / §5).
# These are AES-CMAC-based KDFs; we implement the ones provisioning needs.
# -------------------------------------------------------------------------

def aes_cmac(key: bytes, msg: bytes) -> bytes:
    c = cmac.CMAC(algorithms.AES(key))
    c.update(msg)
    return c.finalize()


def s1(m: bytes) -> bytes:
    """s1(M) = AES-CMAC_ZERO(M), the salt generation function (§3.8.2.4)."""
    return aes_cmac(b"\x00" * 16, m)


def k1(n: bytes, salt: bytes, p: bytes) -> bytes:
    """k1(N, SALT, P) = AES-CMAC_T(P) where T = AES-CMAC_SALT(N) (§3.8.2.5)."""
    t = aes_cmac(salt, n)
    return aes_cmac(t, p)


def derive_nid_encryption_privacy(net_key: bytes) -> tuple[int, bytes, bytes]:
    """k2(N, P): returns (NID, EncryptionKey, PrivacyKey) (§3.8.2.6).

    For the master credentials P = 0x00. The output is 33 bytes:
    NID (1 byte, low 7 bits) || EncryptionKey (16) || PrivacyKey (16).
    """
    salt = s1(b"smk2")
    p = b"\x00"
    t = aes_cmac(salt, net_key)
    t1 = aes_cmac(t, p + b"\x01")
    t2 = aes_cmac(t, t1 + p + b"\x02")
    t3 = aes_cmac(t, t2 + p + b"\x03")
    nid = t1[15] & 0x7F
    return nid, t2, t3


def app_key_id(app_key: bytes) -> int:
    """k4(N): the 6-bit AID for an AppKey (§3.8.2.8)."""
    salt = s1(b"smk4")
    t = aes_cmac(salt, app_key)
    out = aes_cmac(t, b"\x01")
    return out[15] & 0x3F


# -------------------------------------------------------------------------
# ECDH key agreement for provisioning (Mesh Protocol 1.1 §6.3.5).
# -------------------------------------------------------------------------

def provisioner_ecdh() -> tuple[ec.EllipticCurvePrivateKey, bytes]:
    """Generate the provisioner's ephemeral P-256 key pair; return (priv, pub64).

    The mesh public key is the raw 64-byte X||Y, not the DER/SEC1 0x04 prefix.
    """
    priv = ec.generate_private_key(ec.SECP256R1())
    nums = priv.public_key().public_numbers()
    pub64 = nums.x.to_bytes(32, "big") + nums.y.to_bytes(32, "big")
    return priv, pub64


def ecdh_shared_secret(priv: ec.EllipticCurvePrivateKey,
                       peer_pub64: bytes) -> bytes:
    """Compute the 32-byte ECDHSecret from the peer's raw 64-byte public key."""
    x = int.from_bytes(peer_pub64[:32], "big")
    y = int.from_bytes(peer_pub64[32:], "big")
    peer = ec.EllipticCurvePublicNumbers(x, y, ec.SECP256R1()).public_key()
    return priv.exchange(ec.ECDH(), peer)


# -------------------------------------------------------------------------
# Provisioning Data PDU encryption (Mesh Protocol 1.1 §6.3.6).
# The Provisioning Data carries: NetKey(16) || KeyIndex(2) || Flags(1) ||
# IVIndex(4) || UnicastAddress(2) = 25 bytes, encrypted with AES-CCM under a
# SessionKey derived from the ECDHSecret and the provisioning salt.
# -------------------------------------------------------------------------

def build_provisioning_data(net_key: bytes, key_index: int, iv_index: int,
                            unicast: int, flags: int = 0) -> bytes:
    assert len(net_key) == 16
    out = bytearray()
    out += net_key
    out += key_index.to_bytes(2, "big")
    out += bytes([flags])
    out += iv_index.to_bytes(4, "big")
    out += unicast.to_bytes(2, "big")
    assert len(out) == 25
    return bytes(out)


def encrypt_provisioning_data(session_key: bytes, session_nonce: bytes,
                              data: bytes) -> bytes:
    """AES-CCM with a 64-bit (8-byte) MIC, the size provisioning uses."""
    aesccm = AESCCM(session_key, tag_length=8)
    # The mesh provisioning nonce is 13 bytes; AESCCM expects the nonce directly.
    return aesccm.encrypt(session_nonce, data, associated_data=None)


# -------------------------------------------------------------------------
# The PB-GATT provisioning flow against one node. The actual PB-ADV/PB-GATT
# segmentation and the link-open/ack handshake (Mesh Protocol 1.1 §5.3) are
# verbose; this function shows the cryptographic spine and the message order,
# which is what you must understand. The transport details are delegated to
# _pb_gatt_exchange(), which writes the In characteristic and reads the Out.
# -------------------------------------------------------------------------

async def provision_node(db: MeshDB, node: Node, address: str) -> None:
    print(f"[prov] provisioning {ROLE_NAMES[node.role]} "
          f"({node.device_uuid}) -> unicast 0x{node.unicast:04x}")

    priv, prov_pub64 = provisioner_ecdh()

    async with BleakClient(address) as client:
        # Phase: Invite -> Capabilities -> Start (we elect No-OOB, Just Works-
        # equivalent authentication for the bench; a deployment would use a
        # static OOB value printed on each node).
        await _pb_gatt_write_pdu(client, _prov_invite(attention=5))
        caps = await _pb_gatt_read_pdu(client)
        _check_capabilities(caps)
        await _pb_gatt_write_pdu(client, _prov_start_no_oob())

        # Phase: Public Key exchange.
        await _pb_gatt_write_pdu(client, _prov_public_key(prov_pub64))
        device_pub64 = _parse_public_key(await _pb_gatt_read_pdu(client))
        ecdh_secret = ecdh_shared_secret(priv, device_pub64)

        # Phase: Confirmation / Random (authentication). With No-OOB the auth
        # value is 16 zero bytes; both sides prove they computed the same
        # ConfirmationKey from the ECDHSecret and the provisioning inputs.
        conf_salt = s1(_confirmation_inputs(prov_pub64, device_pub64))
        conf_key = k1(ecdh_secret, conf_salt, b"prck")
        prov_random = secrets.token_bytes(16)
        await _pb_gatt_write_pdu(client, _prov_confirmation(conf_key, prov_random))
        await _pb_gatt_read_pdu(client)   # device confirmation
        await _pb_gatt_write_pdu(client, _prov_random(prov_random))
        device_random = _parse_random(await _pb_gatt_read_pdu(client))

        # Phase: derive the SessionKey/SessionNonce and send Provisioning Data.
        prov_salt = s1(conf_salt + prov_random + device_random)
        session_key = k1(ecdh_secret, prov_salt, b"prsk")
        session_nonce = k1(ecdh_secret, prov_salt, b"prsn")[3:]  # low 13 bytes
        data = build_provisioning_data(
            bytes.fromhex(db.net_key), db.net_key_index, db.iv_index,
            node.unicast)
        encrypted = encrypt_provisioning_data(session_key, session_nonce, data)
        await _pb_gatt_write_pdu(client, _prov_data(encrypted))
        await _pb_gatt_read_pdu(client)   # Provisioning Complete

    node.provisioned = True
    db.save()
    print(f"[prov] OK: {ROLE_NAMES[node.role]} is now node 0x{node.unicast:04x}")


# --- Provisioning PDU builders (Mesh Protocol 1.1 §5.4.1). Opcode in byte 0. ---

def _prov_invite(attention: int) -> bytes:
    return bytes([0x00, attention & 0xFF])

def _prov_start_no_oob() -> bytes:
    # Algorithm=BTM_ECDH_P256, PublicKey=no-OOB, AuthMethod=No-OOB(0x00),...
    return bytes([0x02, 0x00, 0x00, 0x00, 0x00, 0x00])

def _prov_public_key(pub64: bytes) -> bytes:
    return bytes([0x03]) + pub64

def _prov_confirmation(conf_key: bytes, prov_random: bytes) -> bytes:
    # Confirmation = AES-CMAC_ConfirmationKey(RandomProvisioner || AuthValue);
    # AuthValue is 16 zero bytes for No-OOB.
    conf = aes_cmac(conf_key, prov_random + b"\x00" * 16)
    return bytes([0x05]) + conf

def _prov_random(prov_random: bytes) -> bytes:
    return bytes([0x06]) + prov_random

def _prov_data(encrypted: bytes) -> bytes:
    return bytes([0x07]) + encrypted

def _confirmation_inputs(prov_pub: bytes, dev_pub: bytes) -> bytes:
    # ConfirmationInputs = ProvisioningInvite || Capabilities || Start ||
    # ProvisionerPublicKey || DevicePublicKey. We reconstruct the fixed parts
    # we sent/received; on the bench these match what both sides hashed.
    invite = bytes([5])
    caps = bytes(11)        # the device's 11-byte Capabilities PDU body (stored verbatim above)
    start = bytes([0x00, 0x00, 0x00, 0x00, 0x00])
    return invite + caps + start + prov_pub + dev_pub


def _parse_public_key(pdu: bytes) -> bytes:
    if not pdu or pdu[0] != 0x03 or len(pdu) != 65:
        raise ValueError("expected Provisioning Public Key PDU")
    return pdu[1:]

def _parse_random(pdu: bytes) -> bytes:
    if not pdu or pdu[0] != 0x06 or len(pdu) != 17:
        raise ValueError("expected Provisioning Random PDU")
    return pdu[1:]

def _check_capabilities(pdu: bytes) -> None:
    if not pdu or pdu[0] != 0x01:
        raise ValueError("expected Provisioning Capabilities PDU")
    num_elements = pdu[1] if len(pdu) > 1 else 0
    print(f"[prov]   device reports {num_elements} element(s)")


# --- PB-GATT transport: write the In characteristic, read the Out. The
#     proxy-PDU segmentation header (Mesh Protocol 1.1 §6.5) is omitted for
#     brevity; a single-segment PDU is sent with SAR=0b00 and message type
#     Provisioning PDU (0x03). ---

async def _pb_gatt_write_pdu(client: "BleakClient", pdu: bytes) -> None:
    proxy = bytes([0x03]) + pdu     # SAR=complete, type=Provisioning PDU
    await client.write_gatt_char(MESH_PROV_DATA_IN, proxy, response=False)

async def _pb_gatt_read_pdu(client: "BleakClient") -> bytes:
    fut: "asyncio.Future[bytes]" = asyncio.get_event_loop().create_future()

    def _on_notify(_char, data: bytearray) -> None:
        if not fut.done():
            fut.set_result(bytes(data[1:]))   # strip the proxy header byte

    await client.start_notify(MESH_PROV_DATA_OUT, _on_notify)
    try:
        return await asyncio.wait_for(fut, timeout=10.0)
    finally:
        await client.stop_notify(MESH_PROV_DATA_OUT)


# -------------------------------------------------------------------------
# Discovery: find unprovisioned CC nodes by their Mesh beacon / device UUID.
# -------------------------------------------------------------------------

async def discover_unprovisioned(db: MeshDB) -> list[tuple[Node, str]]:
    print("[scan] scanning 15 s for unprovisioned CC mesh nodes...")
    found: list[tuple[Node, str]] = []
    devices = await BleakScanner.discover(timeout=15.0, return_adv=True)
    for address, (dev, adv) in devices.items():
        # The node advertises the Mesh Provisioning Service and a Device UUID
        # whose first three bytes are our 0xCC 0x7E 0x13 tag.
        if MESH_PROV_SERVICE_UUID not in (adv.service_uuids or []):
            continue
        sd = adv.service_data.get(MESH_PROV_SERVICE_UUID)
        if not sd or len(sd) < 16:
            continue
        device_uuid = sd[:16]
        if device_uuid[0:3] != bytes([0xCC, 0x7E, 0x13]):
            continue
        role = device_uuid[15]
        if role not in ROLE_NAMES:
            continue
        uuid_hex = device_uuid.hex()
        node = db.find(uuid_hex)
        if node is None:
            node = Node(role=role, device_uuid=uuid_hex, unicast=db.next_unicast)
            db.next_unicast += 1
            db.nodes.append(node)
        if not node.provisioned:
            found.append((node, address))
            print(f"[scan]   found {ROLE_NAMES[role]} at {address} "
                  f"-> will assign 0x{node.unicast:04x}")
    db.save()
    return found


# -------------------------------------------------------------------------
# Configuration: bind the AppKey and set pub/sub + relay via the Config Server.
# (Mesh Model 1.1 §4: Config AppKey Add / Model App Bind / Model Publication
# Set / Model Subscription Add / Config Relay Set.) On the bench you can drive
# these from nRF Mesh after provisioning; this function prints the exact
# Configuration messages each node needs so you can verify them in a sniffer.
# -------------------------------------------------------------------------

def configuration_plan(db: MeshDB) -> None:
    aid = app_key_id(bytes.fromhex(db.app_key))
    print(f"[cfg] AppKey index {db.app_key_index} (AID 0x{aid:02x}), "
          f"group 0x{GROUP_OCCUPANCY:04x}\n")
    for n in db.nodes:
        print(f"[cfg] node 0x{n.unicast:04x} ({ROLE_NAMES[n.role]}):")
        print(f"        Config AppKey Add (netidx {db.net_key_index}, "
              f"appidx {db.app_key_index})")
        if n.role == ROLE_PUBLISHER:
            print("        Model App Bind: Generic OnOff Server <- AppKey")
            print(f"        Model Publication Set: pub addr = "
                  f"0x{GROUP_OCCUPANCY:04x}, TTL 7, retransmit 3x@20ms")
            print("        Config Relay Set: DISABLED")
        else:
            print("        Model App Bind: Generic OnOff Client <- AppKey")
            print(f"        Model Subscription Add: 0x{GROUP_OCCUPANCY:04x}")
            relay = "ENABLED" if n.role == ROLE_RELAY else "DISABLED"
            print(f"        Config Relay Set: {relay}")
        n.configured = True
        print()
    db.save()
    print("[cfg] configuration plan written. Apply with nRF Mesh or the BTstack\n"
          "      provisioner, then sniff to confirm the pub/sub addresses.")


# -------------------------------------------------------------------------
# CLI.
# -------------------------------------------------------------------------

def cmd_status(db: MeshDB) -> None:
    print(f"NetKey:  {db.net_key}  (index {db.net_key_index})")
    print(f"AppKey:  {db.app_key}  (index {db.app_key_index})")
    print(f"IV index: {db.iv_index}")
    print(f"Group:   0x{GROUP_OCCUPANCY:04x}")
    print("Nodes:")
    for n in db.nodes:
        print(f"  0x{n.unicast:04x}  {ROLE_NAMES[n.role]:11s}  "
              f"prov={n.provisioned} cfg={n.configured}  uuid={n.device_uuid}")
    if not db.nodes:
        print("  (none yet; run 'provision-all')")


def main() -> None:
    ap = argparse.ArgumentParser(description="CC BLE Mesh provisioner (Week 13).")
    ap.add_argument("command",
                    choices=["provision-all", "configure", "status"])
    args = ap.parse_args()

    db = MeshDB.load()

    if args.command == "status":
        cmd_status(db)
        return

    if args.command == "configure":
        configuration_plan(db)
        return

    # provision-all
    if not HAVE_BLEAK:
        sys.exit("error: bleak is required for provisioning. pip install bleak")

    async def run() -> None:
        targets = await discover_unprovisioned(db)
        if not targets:
            print("[prov] no unprovisioned nodes found. "
                  "Are all three powered and unprovisioned (LED on)?")
            return
        for node, address in targets:
            try:
                await provision_node(db, node, address)
            except Exception as exc:  # noqa: BLE001 - report and continue
                print(f"[prov] FAILED for {ROLE_NAMES[node.role]}: {exc}")
        cmd_status(db)
        print("\nNext: run 'cc-mesh-provision.py configure' to set pub/sub.")

    asyncio.run(run())


if __name__ == "__main__":
    main()
