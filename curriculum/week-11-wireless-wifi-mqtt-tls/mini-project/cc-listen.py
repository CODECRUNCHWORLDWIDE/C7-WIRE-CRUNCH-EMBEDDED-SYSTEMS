#!/usr/bin/env python3
"""
cc-listen.py - Host-side subscriber and soak-analyzer for the Week 11
sensor node.

Subscribes to cc7/devices/<id>/telemetry on test.mosquitto.org over TLS
(port 8883), pretty-prints each reading with a host-side wall-clock and
monotonic timestamp, and tracks the sequence numbers it sees so it can
report the delivery ratio at the end of a run. This is the laptop side of
Challenge 1's 24-hour soak: pipe its output to listen.log, pipe the
device's UART to device.log, and reconcile the two by sequence number.

Two modes:
    listen   (default)  subscribe live and print every reading
    analyze             reconcile a device.log + listen.log pair offline

Usage:
    cc-listen.py --id a1b2c3
    cc-listen.py --id a1b2c3 --host test.mosquitto.org --port 8883
    cc-listen.py analyze --device device.log --listen listen.log

Requires: paho-mqtt >= 1.6  (pip install paho-mqtt)
For port 8883 the system CA store validates the broker cert; no pinning is
needed on the laptop (that constraint is the device's, not yours).
"""

from __future__ import annotations

import argparse
import json
import re
import ssl
import sys
import time
from pathlib import Path

# ---------------------------------------------------------------------------
# Live subscribe mode
# ---------------------------------------------------------------------------

def run_listen(args: argparse.Namespace) -> int:
    try:
        import paho.mqtt.client as mqtt
    except ImportError:
        sys.exit("error: paho-mqtt is required. pip install paho-mqtt")

    topic = f"cc7/devices/{args.id}/telemetry"
    seen: set[int] = set()
    t0 = time.monotonic()

    def on_connect(client, userdata, flags, reason_code, properties=None):
        # paho 2.x passes reason_code; 1.x passes rc int. Both stringify fine.
        print(f"# connected to {args.host}:{args.port} rc={reason_code}",
              flush=True)
        client.subscribe(topic, qos=0)
        print(f"# subscribed to {topic}", flush=True)

    def on_message(client, userdata, msg):
        rel = time.monotonic() - t0
        wall = time.strftime("%Y-%m-%dT%H:%M:%S")
        raw = msg.payload
        seq = "?"
        try:
            obj = json.loads(raw.decode("utf-8"))
            seq = obj.get("seq", "?")
            if isinstance(seq, int):
                seen.add(seq)
            extra = (f"temp_c={obj.get('temp_c')} "
                     f"vbat_v={obj.get('vbat_v')} "
                     f"uptime_s={obj.get('uptime_s')}")
        except (ValueError, UnicodeDecodeError):
            extra = f"raw={raw!r}"
        print(f"[{wall}] [+{rel:9.3f} s] RECV seq={seq} bytes={len(raw)} "
              f"topic={msg.topic} {extra}", flush=True)

    # paho 2.x changed the constructor signature; support both.
    try:
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                             client_id=f"cc-listen-{args.id}")
    except (AttributeError, TypeError):
        client = mqtt.Client(client_id=f"cc-listen-{args.id}")

    client.on_connect = on_connect
    client.on_message = on_message

    if args.port == 8883:
        client.tls_set(cert_reqs=ssl.CERT_REQUIRED,
                       tls_version=ssl.PROTOCOL_TLS_CLIENT)

    client.connect(args.host, args.port, keepalive=60)
    print(f"# listening; Ctrl-C to stop and print the summary", flush=True)
    try:
        client.loop_forever()
    except KeyboardInterrupt:
        pass
    finally:
        client.disconnect()

    if seen:
        lo, hi = min(seen), max(seen)
        span = hi - lo + 1
        print(f"\n# summary: {len(seen)} distinct seqs in [{lo}, {hi}] "
              f"(span {span}); gaps={span - len(seen)}", flush=True)
    return 0


# ---------------------------------------------------------------------------
# Offline reconcile mode
# ---------------------------------------------------------------------------

_PUB_RE = re.compile(r"PUB seq=(\d+)")
_RECV_RE = re.compile(r"RECV seq=(\d+)")


def _parse_seqs(path: Path, pattern: re.Pattern) -> set[int]:
    out: set[int] = set()
    with path.open("r", errors="replace") as fh:
        for line in fh:
            m = pattern.search(line)
            if m:
                out.add(int(m.group(1)))
    return out


def run_analyze(args: argparse.Namespace) -> int:
    device = Path(args.device)
    listen = Path(args.listen)
    if not device.exists():
        sys.exit(f"error: {device} not found")
    if not listen.exists():
        sys.exit(f"error: {listen} not found")

    sent = _parse_seqs(device, _PUB_RE)
    received = _parse_seqs(listen, _RECV_RE)

    if not sent:
        sys.exit("error: no 'PUB seq=' lines found in the device log")

    delivered = sent & received
    missing = sorted(sent - received)
    phantom = sorted(received - sent)
    ratio = len(delivered) / len(sent)

    print(f"sent       : {len(sent)}")
    print(f"received   : {len(received)}")
    print(f"delivered  : {len(delivered)}")
    print(f"missing    : {len(missing)}")
    print(f"phantom    : {len(phantom)}  (received with no matching send)")
    print(f"ratio      : {ratio * 100:.3f}%")
    print(f"verdict    : {'PASS' if ratio >= 0.99 else 'FAIL'} "
          f"(bar = 99.000%)")

    if missing:
        # Cluster contiguous runs of missing sequence numbers, which usually
        # correspond to a single disconnect/backoff window in device.log.
        runs = []
        start = prev = missing[0]
        for s in missing[1:]:
            if s == prev + 1:
                prev = s
            else:
                runs.append((start, prev))
                start = prev = s
        runs.append((start, prev))
        print(f"\nmissing runs ({len(runs)}):")
        for lo, hi in runs:
            n = hi - lo + 1
            print(f"  seq {lo}..{hi}  ({n} publish{'es' if n != 1 else ''})")

    if phantom:
        print(f"\nWARNING: {len(phantom)} phantom receives — likely two "
              f"devices on the same topic or a seq-counter reset.")

    return 0 if ratio >= 0.99 else 1


# ---------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(
        description="Subscriber + soak analyzer for the C7 Week 11 sensor node.")
    sub = ap.add_subparsers(dest="mode")

    # listen is the default if no subcommand is given.
    ap.add_argument("--id", help="device id suffix, e.g. a1b2c3")
    ap.add_argument("--host", default="test.mosquitto.org")
    ap.add_argument("--port", type=int, default=8883)

    p_an = sub.add_parser("analyze", help="reconcile device.log vs listen.log")
    p_an.add_argument("--device", required=True)
    p_an.add_argument("--listen", required=True)

    args = ap.parse_args()

    if args.mode == "analyze":
        return run_analyze(args)

    if not args.id:
        ap.error("--id is required in listen mode")
    return run_listen(args)


if __name__ == "__main__":
    raise SystemExit(main())
