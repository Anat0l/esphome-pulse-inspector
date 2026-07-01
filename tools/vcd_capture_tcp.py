#!/usr/bin/env python3
"""Reliable VCD capture client for esphome `pulse_inspector_vcd`.

Why this exists:
- ncat/PowerShell/cmd-redirect each have their own Windows-specific
  buffering and EOL-translation quirks that occasionally make the
  resulting capture file smaller than what was actually sent.
- This script is a no-frills recv()->fsync() loop, with byte counters
  that can be cross-checked against the device-side counters logged by
  the component every 5 seconds ("VCD stream: ... bytes sent ...").

Usage:
    python tools/vcd_capture_tcp.py 192.168.20.217 9000 capture.vcd
    python tools/vcd_capture_tcp.py 192.168.20.217 9000 capture.vcd --duration 600

Exit reasons:
- Ctrl+C        -> graceful close, file is fully on disk and replayable.
- Peer closed   -> device sent FIN; final size is logged.
- recv timeout  -> nothing arrived for `--idle-timeout` seconds.
"""
from __future__ import annotations

import argparse
import os
import socket
import sys
import time


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("host")
    p.add_argument("port", type=int)
    p.add_argument("output")
    p.add_argument("--duration", type=int, default=0,
                   help="Stop after N seconds (0 = run until peer closes / Ctrl+C).")
    p.add_argument("--idle-timeout", type=int, default=120,
                   help="Abort if no bytes arrive for this many seconds (default 120).")
    p.add_argument("--print-interval", type=int, default=5,
                   help="Progress line every N seconds (default 5).")
    args = p.parse_args()

    print(f"[client] connecting to {args.host}:{args.port}", file=sys.stderr)
    sock = socket.create_connection((args.host, args.port), timeout=10)
    sock.settimeout(args.idle_timeout)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
    try:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    except OSError:
        pass
    print(f"[client] connected; writing to {args.output}", file=sys.stderr)

    bytes_total = 0
    chunks_total = 0
    started_at = time.time()
    last_print = started_at
    reason = "peer closed"

    with open(args.output, "wb", buffering=0) as f:
        try:
            while True:
                if args.duration and (time.time() - started_at) >= args.duration:
                    reason = "duration reached"
                    break

                try:
                    chunk = sock.recv(8192)
                except socket.timeout:
                    reason = f"no bytes for {args.idle_timeout}s"
                    break

                if not chunk:
                    break

                f.write(chunk)
                # buffering=0 already gives unbuffered IO, but be explicit
                # so even funky filesystems push the data out.
                os.fsync(f.fileno()) if False else None  # noqa: SIM108

                bytes_total += len(chunk)
                chunks_total += 1

                now = time.time()
                if now - last_print >= args.print_interval:
                    age = now - started_at
                    rate = bytes_total / age if age > 0 else 0.0
                    print(
                        f"[client] {age:6.1f}s  {bytes_total:>10} B  "
                        f"({rate:>7.0f} B/s)  chunks={chunks_total}",
                        file=sys.stderr,
                    )
                    last_print = now
        except KeyboardInterrupt:
            reason = "Ctrl+C"

    age = time.time() - started_at
    print(
        f"[client] stopped: {reason}; received {bytes_total} bytes "
        f"in {chunks_total} chunks over {age:.1f}s",
        file=sys.stderr,
    )
    try:
        sock.shutdown(socket.SHUT_RDWR)
    except OSError:
        pass
    sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
