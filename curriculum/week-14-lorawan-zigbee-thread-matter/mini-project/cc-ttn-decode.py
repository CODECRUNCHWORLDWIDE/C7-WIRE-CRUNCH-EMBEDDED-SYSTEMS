#!/usr/bin/env python3
"""
cc-ttn-decode.py - Offline LoRaWAN 1.0.x frame decoder for the Week 14
mini-project.

Given a captured PHYPayload (the hex you see in the node's CDC log or in a
gateway capture) plus your session keys, this tool:
  - parses the MHDR / FHDR / FPort / FRMPayload / MIC,
  - recomputes the MIC with NwkSKey and tells you whether it is VALID,
  - decrypts the FRMPayload with AppSKey and prints the plaintext.

It uses only the Python standard library (a small AES-128 + AES-CMAC are
included) so it runs anywhere with no pip install. The point is to debug a
rejected frame WITHOUT the network in the loop: if cc-ttn-decode says the MIC
is valid and the plaintext is right, your bug is on the radio/RF side, not in
the keys or the byte order; if it says the MIC is invalid, your bug is in the
keys, the DevAddr endianness, or the frame counter.

Usage:
    cc-ttn-decode.py --devaddr 26011F88 \\
        --nwkskey 2B7E151628AED2A6ABF7158809CF4F3C \\
        --appskey 00112233445566778899AABBCCDDEEFF \\
        --phy 40881F0126000000010F0AB81C2959C26

Citations:
    - LoRaWAN L2 1.0.4 §4.3.3 (FRMPayload), §4.4 (MIC).
    - RFC 4493 (AES-CMAC).  - FIPS 197 (AES).
"""

from __future__ import annotations

import argparse
import sys

# --------------------------------------------------------------------------
# AES-128 forward cipher (FIPS 197), pure Python, encrypt-only.
# --------------------------------------------------------------------------

_SBOX = bytes([
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
])

_RCON = [0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1B,0x36]


def _xtime(a: int) -> int:
    return ((a << 1) ^ (0x1B if (a & 0x80) else 0)) & 0xFF


def _key_expand(key: bytes) -> list[int]:
    rk = list(key)
    for i in range(4, 44):
        t = rk[(i - 1) * 4:(i - 1) * 4 + 4]
        if i % 4 == 0:
            t = [_SBOX[t[1]] ^ _RCON[i // 4 - 1], _SBOX[t[2]], _SBOX[t[3]], _SBOX[t[0]]]
        for j in range(4):
            rk.append(rk[(i - 4) * 4 + j] ^ t[j])
    return rk


def aes128_encrypt(key: bytes, block: bytes) -> bytes:
    rk = _key_expand(key)
    s = [block[i] ^ rk[i] for i in range(16)]
    for rnd in range(1, 11):
        s = [_SBOX[b] for b in s]
        # ShiftRows (column-major).
        s = [
            s[0],  s[5],  s[10], s[15],
            s[4],  s[9],  s[14], s[3],
            s[8],  s[13], s[2],  s[7],
            s[12], s[1],  s[6],  s[11],
        ]
        if rnd != 10:
            ns = s[:]
            for col in range(4):
                a = s[col * 4:col * 4 + 4]
                allx = a[0] ^ a[1] ^ a[2] ^ a[3]
                ns[col * 4 + 0] = a[0] ^ allx ^ _xtime(a[0] ^ a[1])
                ns[col * 4 + 1] = a[1] ^ allx ^ _xtime(a[1] ^ a[2])
                ns[col * 4 + 2] = a[2] ^ allx ^ _xtime(a[2] ^ a[3])
                ns[col * 4 + 3] = a[3] ^ allx ^ _xtime(a[3] ^ a[0])
            s = ns
        s = [s[i] ^ rk[rnd * 16 + i] for i in range(16)]
    return bytes(s)


# --------------------------------------------------------------------------
# AES-CMAC (RFC 4493).
# --------------------------------------------------------------------------

def _shl(b: bytes) -> bytes:
    out = bytearray(16)
    ov = 0
    for i in range(15, -1, -1):
        out[i] = ((b[i] << 1) | ov) & 0xFF
        ov = 1 if (b[i] & 0x80) else 0
    return bytes(out)


def aes_cmac(key: bytes, msg: bytes) -> bytes:
    l = aes128_encrypt(key, bytes(16))
    k1 = bytearray(_shl(l))
    if l[0] & 0x80:
        k1[15] ^= 0x87
    k2 = bytearray(_shl(bytes(k1)))
    if k1[0] & 0x80:
        k2[15] ^= 0x87

    n = (len(msg) + 15) // 16
    if n == 0:
        n = 1
        complete = False
    else:
        complete = (len(msg) % 16 == 0)

    if complete:
        last = bytes(a ^ b for a, b in zip(msg[(n - 1) * 16:], k1))
    else:
        rem = len(msg) % 16
        block = bytearray(16)
        block[:rem] = msg[(n - 1) * 16:(n - 1) * 16 + rem]
        block[rem] = 0x80
        last = bytes(a ^ b for a, b in zip(block, k2))

    x = bytes(16)
    for i in range(n - 1):
        y = bytes(a ^ b for a, b in zip(x, msg[i * 16:i * 16 + 16]))
        x = aes128_encrypt(key, y)
    y = bytes(a ^ b for a, b in zip(x, last))
    return aes128_encrypt(key, y)


# --------------------------------------------------------------------------
# LoRaWAN frame decode.
# --------------------------------------------------------------------------

def _b0(dir_byte: int, devaddr_le: bytes, fcnt: int, msg_len: int) -> bytes:
    b = bytearray(16)
    b[0] = 0x49
    b[5] = dir_byte
    b[6:10] = devaddr_le
    b[10:14] = fcnt.to_bytes(4, "little")
    b[15] = msg_len
    return bytes(b)


def _frm_crypt(key: bytes, dir_byte: int, devaddr_le: bytes, fcnt: int,
               payload: bytes) -> bytes:
    out = bytearray()
    blocks = (len(payload) + 15) // 16
    for i in range(1, blocks + 1):
        a = bytearray(16)
        a[0] = 0x01
        a[5] = dir_byte
        a[6:10] = devaddr_le
        a[10:14] = fcnt.to_bytes(4, "little")
        a[15] = i
        s = aes128_encrypt(key, bytes(a))
        chunk = payload[(i - 1) * 16:(i - 1) * 16 + 16]
        out += bytes(c ^ s[j] for j, c in enumerate(chunk))
    return bytes(out)


def decode(phy: bytes, devaddr_be: bytes, nwkskey: bytes, appskey: bytes,
           dir_byte: int = 0x00) -> None:
    if len(phy) < 13:
        sys.exit("error: PHYPayload too short to be a data frame")

    devaddr_le = devaddr_be[::-1]
    mhdr = phy[0]
    mtype = mhdr >> 5
    frame_devaddr_le = phy[1:5]
    fctrl = phy[5]
    fcnt = int.from_bytes(phy[6:8], "little")
    fopts_len = fctrl & 0x0F
    fhdr_len = 1 + 7 + fopts_len    # MHDR + DevAddr+FCtrl+FCnt + FOpts
    mic = phy[-4:]
    msg = phy[:-4]

    print(f"MHDR     0x{mhdr:02X}  (MType={mtype} "
          f"{'unconf-up' if mtype==2 else 'unconf-down' if mtype==3 else 'other'})")
    print(f"DevAddr  on-wire(LE)={frame_devaddr_le.hex().upper()}  "
          f"display(BE)={frame_devaddr_le[::-1].hex().upper()}")
    print(f"FCtrl    0x{fctrl:02X}  (FOptsLen={fopts_len})")
    print(f"FCnt     {fcnt}")

    if frame_devaddr_le != devaddr_le:
        print("WARNING: frame DevAddr does not match --devaddr. "
              "Check byte order (console is big-endian).")

    # MIC check.
    block = _b0(dir_byte, devaddr_le, fcnt, len(msg)) + msg
    calc = aes_cmac(nwkskey, block)[:4]
    mic_ok = (calc == mic)
    print(f"MIC      frame={mic.hex().upper()}  computed={calc.hex().upper()}  "
          f"-> {'VALID' if mic_ok else 'INVALID'}")

    if fhdr_len + 1 <= len(msg):
        fport = msg[fhdr_len]
        ct = msg[fhdr_len + 1:]
        key = nwkskey if fport == 0 else appskey
        pt = _frm_crypt(key, dir_byte, devaddr_le, fcnt, ct)
        print(f"FPort    {fport}")
        print(f"FRMPayload ciphertext: {ct.hex().upper()}")
        print(f"FRMPayload plaintext:  {pt.hex().upper()}")
        if fport == 1 and len(pt) >= 3:
            raw = int.from_bytes(pt[0:2], "big", signed=True)
            print(f"  decoded: temperature={raw/100.0:.2f} C  "
                  f"battery={pt[2]}%")
    else:
        print("FPort/FRMPayload: none (empty frame)")

    if not mic_ok:
        print("\nThe MIC is INVALID. Suspect, in order: DevAddr byte order, "
              "NwkSKey bytes, the frame counter, then the message length.")


def _hexarg(s: str) -> bytes:
    s = s.replace(" ", "").replace("0x", "")
    return bytes.fromhex(s)


def main() -> None:
    ap = argparse.ArgumentParser(description="Offline LoRaWAN frame decoder.")
    ap.add_argument("--devaddr", required=True,
                    help="DevAddr as shown in the TTN console (big-endian hex)")
    ap.add_argument("--nwkskey", required=True, help="NwkSKey (32 hex chars)")
    ap.add_argument("--appskey", required=True, help="AppSKey (32 hex chars)")
    ap.add_argument("--phy", required=True, help="PHYPayload (hex)")
    ap.add_argument("--downlink", action="store_true",
                    help="decode as a downlink (B0 direction = 1)")
    args = ap.parse_args()

    devaddr = _hexarg(args.devaddr)
    nwkskey = _hexarg(args.nwkskey)
    appskey = _hexarg(args.appskey)
    phy = _hexarg(args.phy)

    if len(devaddr) != 4:
        sys.exit("error: --devaddr must be 4 bytes")
    if len(nwkskey) != 16 or len(appskey) != 16:
        sys.exit("error: keys must be 16 bytes (32 hex chars)")

    decode(phy, devaddr, nwkskey, appskey, 0x01 if args.downlink else 0x00)


if __name__ == "__main__":
    main()
