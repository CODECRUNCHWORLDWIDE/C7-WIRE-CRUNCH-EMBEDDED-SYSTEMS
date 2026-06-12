#!/usr/bin/env python3
"""
cc-orientation.py - Host-side telemetry + live plot for the Week 15 mini-project.

Reads the CSV telemetry stream the fusion_node firmware prints over USB CDC
(t_ms,roll,pitch,yaw,motor_cmd,servo_deg), logs it to a file, and optionally
live-plots roll/pitch/yaw with matplotlib. Also computes the yaw drift over the
session, which is the headline number for the "plot drift over 10 minutes"
deliverable in the README.

Usage:
    cc-orientation.py [--port /dev/cu.usbmodem...] [--log run.csv] [--plot]
                      [--seconds N]

Citations:
    - Telemetry format defined in fusion_node.c.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path
from typing import Optional

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.exit("error: pyserial is required. pip install pyserial")


def find_pico_port() -> Optional[str]:
    """Return the first Pico CDC port (Raspberry Pi vendor id 0x2E8A)."""
    for port in serial.tools.list_ports.comports():
        if port.vid == 0x2E8A:
            return port.device
    return None


def parse_line(line: str):
    """Parse one CSV telemetry line into a tuple, or None for headers/junk."""
    line = line.strip()
    if not line or line.startswith("#"):
        return None
    parts = line.split(",")
    if len(parts) != 6:
        return None
    try:
        t_ms = int(parts[0])
        roll, pitch, yaw, motor, servo = (float(p) for p in parts[1:])
    except ValueError:
        return None
    return (t_ms, roll, pitch, yaw, motor, servo)


def run(port: str, baud: int, log_path: Optional[Path],
        do_plot: bool, seconds: Optional[float]) -> None:
    ser = serial.Serial(port, baud, timeout=2.0)
    ser.reset_input_buffer()
    print(f"cc-orientation: port={port} baud={baud}")

    log = log_path.open("w") if log_path else None
    if log:
        log.write("t_ms,roll_deg,pitch_deg,yaw_deg,motor_cmd,servo_deg\n")

    # Optional live plot.
    plot = None
    if do_plot:
        try:
            import matplotlib
            import matplotlib.pyplot as plt
            from collections import deque
            matplotlib.rcParams["toolbar"] = "None"
            plt.ion()
            fig, ax = plt.subplots()
            ax.set_xlabel("sample")
            ax.set_ylabel("degrees")
            ax.set_ylim(-180, 180)
            N = 500
            buf = {k: deque([0.0] * N, maxlen=N) for k in ("roll", "pitch", "yaw")}
            lines = {k: ax.plot(range(N), list(buf[k]), label=k)[0]
                     for k in buf}
            ax.legend(loc="upper right")
            plot = (plt, fig, ax, buf, lines, N)
        except ImportError:
            print("cc-orientation: matplotlib not installed; logging only")
            do_plot = False

    first_yaw = None
    last_yaw = 0.0
    count = 0
    t0 = time.time()

    try:
        while True:
            if seconds is not None and (time.time() - t0) >= seconds:
                break
            raw = ser.readline()
            if not raw:
                continue
            rec = parse_line(raw.decode("ascii", errors="replace"))
            if rec is None:
                continue
            t_ms, roll, pitch, yaw, motor, servo = rec
            count += 1
            if first_yaw is None:
                first_yaw = yaw
            last_yaw = yaw

            if log:
                log.write(f"{t_ms},{roll},{pitch},{yaw},{motor},{servo}\n")

            if count % 50 == 0:
                print(f"  t={t_ms/1000:7.1f}s  roll={roll:7.1f}  "
                      f"pitch={pitch:7.1f}  yaw={yaw:7.1f}  motor={motor:+.3f}")

            if do_plot and plot is not None:
                plt, fig, ax, buf, lines, N = plot
                for k, v in (("roll", roll), ("pitch", pitch), ("yaw", yaw)):
                    buf[k].append(v)
                    lines[k].set_ydata(list(buf[k]))
                if count % 5 == 0:
                    fig.canvas.draw_idle()
                    fig.canvas.flush_events()
    except KeyboardInterrupt:
        print("\ncc-orientation: stopped by user")
    finally:
        ser.close()
        if log:
            log.close()
            print(f"cc-orientation: logged {count} samples to {log_path}")

    # Drift report. With the board held still, yaw should not move; the delta
    # is the magnetometer-disciplined drift over the session.
    if first_yaw is not None:
        drift = last_yaw - first_yaw
        # wrap to [-180, 180]
        while drift > 180.0:
            drift -= 360.0
        while drift < -180.0:
            drift += 360.0
        elapsed_min = (time.time() - t0) / 60.0
        print(f"cc-orientation: yaw drift = {drift:+.2f} deg over "
              f"{elapsed_min:.1f} min "
              f"({drift / elapsed_min:+.2f} deg/min)" if elapsed_min > 0 else "")


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Telemetry + live plot for Code Crunch C7 Week 15.")
    ap.add_argument("--port", type=str, default=None,
                    help="serial port (auto-detected if omitted)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--log", type=Path, default=None, help="CSV log file")
    ap.add_argument("--plot", action="store_true", help="live matplotlib plot")
    ap.add_argument("--seconds", type=float, default=None,
                    help="stop after N seconds (default: run until Ctrl-C)")
    args = ap.parse_args()

    port = args.port or find_pico_port()
    if port is None:
        sys.exit("error: could not find a Pico CDC port. "
                 "Pass --port /dev/cu.usbmodem... explicitly.")

    run(port, args.baud, args.log, args.plot, args.seconds)


if __name__ == "__main__":
    main()
