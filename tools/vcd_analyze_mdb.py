"""Quick stats about the captured UART-like traffic.

Uses the decoder in vcd_decode_uart to get 9N1 frames with ch1 inverted
and summarises:
  - unique master bytes (9b=1) and slave bytes (9b=0) on each channel
  - inter-frame intervals per channel
  - cycle stats (pair counts, pair timing)
"""
from __future__ import annotations

import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from vcd_decode_uart import parse_vcd, decode_uart  # type: ignore


def main(path: str) -> int:
    channels = parse_vcd(path)
    if not channels:
        print("empty VCD", file=sys.stderr)
        return 1

    frames_by_name = {}
    for sym, ch in channels.items():
        invert = "ch1" in ch.name  # same default as used before
        frames_by_name[ch.name] = decode_uart(
            ch, baud=9600, data_bits=9, parity="N", invert=invert
        )

    for name, frames in frames_by_name.items():
        good = [f for f in frames if f.ok]
        print(f"== {name}: {len(frames)} frames, {len(good)} ok ==")

        master = Counter(f.value for f in good if (f.value >> 8) & 1)
        slave = Counter(f.value for f in good if not ((f.value >> 8) & 1))
        print(f"  bytes with 9b=1 (addr/cmd): {len(master)} unique")
        for v, n in master.most_common(10):
            print(f"     0x{v:03X}  (byte=0x{v & 0xff:02X}) x{n}")
        print(f"  bytes with 9b=0 (data):     {len(slave)} unique")
        for v, n in slave.most_common(10):
            print(f"     0x{v:03X}  (byte=0x{v & 0xff:02X}) x{n}")

        # Inter-frame intervals (us) inside a channel
        if len(good) >= 2:
            deltas = [good[i + 1].t_us - good[i].t_us for i in range(len(good) - 1)]
            hist = Counter()
            for d in deltas:
                # bucket to nearest 100 us up to 2 ms, otherwise to ms
                if d < 2000:
                    hist[f"{round(d / 100) * 100} us"] += 1
                else:
                    hist[f"{round(d / 1000)} ms"] += 1
            print("  top inter-frame gaps:")
            for bucket, n in hist.most_common(10):
                print(f"     {bucket:>12}  x{n}")

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "capture.vcd"))
