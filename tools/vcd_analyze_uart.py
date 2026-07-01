#!/usr/bin/env python3
"""Offline UART analyzer for VCD files produced by pulse_inspector_vcd.

What it does:
    1. Reads a VCD file (any number of channels), expecting it to come from
       pulse_inspector_vcd (timescale 1us).
    2. For each requested channel, auto-detects the bit period from the
       distribution of edge intervals (smallest "unit" interval == 1 bit).
    3. Decodes the stream as a UART under several frame layouts and reports
       which one yielded the fewest framing errors.
    4. Optionally dumps the decoded bytes with timestamps and groups them
       into packets separated by "long" idle gaps.

Why this exists:
    Confirms the wire-level protocol parameters (baud, frame layout) from
    real captures before we commit to them in firmware. Also useful as a
    sanity-check in case decoded behaviour on-device looks weird.

Usage:
    python tools/vcd_analyze_uart.py capture.vcd
    python tools/vcd_analyze_uart.py file.vcd --channel ch2_gpio4
    python tools/vcd_analyze_uart.py file.vcd --channel ch2_gpio4 --dump 200
    python tools/vcd_analyze_uart.py file.vcd --baud 9600 --frame 8N1
"""
from __future__ import annotations

import argparse
import collections
import re
import sys
from dataclasses import dataclass
from typing import Dict, Iterable, List, Optional, Tuple


# ---------------------------------------------------------------------------
# VCD parsing
# ---------------------------------------------------------------------------

@dataclass
class VcdSignal:
    symbol: str
    name: str
    initial: Optional[int] = None  # 0/1 if declared at #0, None if 'x'


def parse_vcd(path: str) -> Tuple[Dict[str, VcdSignal], List[Tuple[int, str, int]]]:
    """Parse VCD into (signals_by_name, edges).

    edges is a list of (t_us, symbol, level) sorted by t.
    Only digital 0/1 transitions are returned; 'x' is treated as no edge.
    """
    signals_by_sym: Dict[str, VcdSignal] = {}
    name_to_sym: Dict[str, str] = {}
    edges: List[Tuple[int, str, int]] = []
    in_header = True
    cur_t = 0

    var_re = re.compile(r"\$var\s+\w+\s+\d+\s+(\S+)\s+(\S+)\s+\$end")

    with open(path, "rt", encoding="utf-8", errors="replace") as f:
        for raw in f:
            line = raw.strip()
            if not line:
                continue
            if in_header:
                m = var_re.match(line)
                if m:
                    sym, name = m.group(1), m.group(2)
                    signals_by_sym[sym] = VcdSignal(sym, name)
                    name_to_sym[name] = sym
                    continue
                if line.startswith("$enddefinitions"):
                    in_header = False
                    continue
                continue

            if line.startswith("#"):
                cur_t = int(line[1:])
                continue
            # Single-bit value change: e.g. `1!` or `0$`.
            if len(line) >= 2 and line[0] in "01xz":
                level_chr = line[0]
                sym = line[1:]
                if sym not in signals_by_sym:
                    continue
                if level_chr in ("x", "z"):
                    continue
                lvl = 1 if level_chr == "1" else 0
                if signals_by_sym[sym].initial is None and cur_t == 0:
                    signals_by_sym[sym].initial = lvl
                edges.append((cur_t, sym, lvl))

    signals_by_name = {sig.name: sig for sig in signals_by_sym.values()}
    return signals_by_name, edges


# ---------------------------------------------------------------------------
# Per-channel level reconstruction
# ---------------------------------------------------------------------------

def reconstruct_levels(
    edges: List[Tuple[int, str, int]],
    sym: str,
    initial: int,
) -> List[Tuple[int, int]]:
    """Reduce edges to (t, level) for a single channel, including initial."""
    out = [(0, initial)]
    last = initial
    for t, s, lvl in edges:
        if s != sym:
            continue
        if lvl == last:
            continue  # spurious same-value entry
        out.append((t, lvl))
        last = lvl
    return out


def level_at(timeline: List[Tuple[int, int]], t: int) -> int:
    """Binary-search the level at time t. Returns 0/1."""
    lo, hi = 0, len(timeline) - 1
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if timeline[mid][0] <= t:
            lo = mid
        else:
            hi = mid - 1
    return timeline[lo][1]


# ---------------------------------------------------------------------------
# Baud-rate auto-detection
# ---------------------------------------------------------------------------

COMMON_BAUDS = (1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200)


def detect_baud(timeline: List[Tuple[int, int]]) -> Optional[int]:
    """Estimate baud from the distribution of inter-edge intervals.

    Idea: the shortest, recurring interval between two edges within a UART
    byte equals exactly 1 bit period (when two adjacent bits flip). The
    smallest cluster in the histogram of intervals is a strong proxy for
    that bit period. Snap it to the closest standard baud.
    """
    intervals: List[int] = []
    for i in range(2, len(timeline)):  # skip the synthetic (0, initial) point
        dt = timeline[i][0] - timeline[i - 1][0]
        if dt > 0:
            intervals.append(dt)
    if len(intervals) < 100:
        return None

    # Build a coarse histogram (10 us buckets up to 5 ms — fine enough for
    # anything from 300 to ~1M baud).
    bucket = 10
    hist = collections.Counter((dt // bucket) * bucket for dt in intervals if dt < 5000)
    # The smallest bucket with at least 1% of edges in it is our best
    # candidate for the unit bit period. Without the threshold a few
    # spurious narrow glitches dominate.
    threshold = max(5, len(intervals) // 100)
    candidates = sorted(b for b, n in hist.items() if n >= threshold and b > 0)
    if not candidates:
        return None
    bit_us = candidates[0] + bucket / 2  # center of bucket
    baud_est = 1_000_000 / bit_us
    # Snap to the closest common baud within 10%.
    best = min(COMMON_BAUDS, key=lambda b: abs(b - baud_est))
    if abs(best - baud_est) / best > 0.10:
        return None
    return best


# ---------------------------------------------------------------------------
# UART decoder (sample-at-mid-bit on a level timeline)
# ---------------------------------------------------------------------------

@dataclass
class UartByte:
    t_start_us: int
    bits: int          # 8 or 9
    value: int
    extra: Optional[int] = None  # 9th bit / parity, if applicable
    framing_ok: bool = True
    parity_ok: Optional[bool] = None  # None when no parity check requested


def _byte_parity(value: int) -> int:
    """Return the parity bit (0 / 1) of an 8-bit value (XOR fold)."""
    v = value & 0xFF
    v ^= v >> 4
    v ^= v >> 2
    v ^= v >> 1
    return v & 1


def decode_uart(
    timeline: List[Tuple[int, int]],
    baud: int,
    data_bits: int = 8,
    extra_bit: bool = False,  # +1 mode/parity bit between data and stop
    stop_bits: int = 1,
    end_t: Optional[int] = None,
    parity: Optional[str] = None,  # None / "even" / "odd" -- enforces extra
) -> List[UartByte]:
    """Decode a UART stream by sampling the reconstructed level timeline.

    `extra_bit=True` means we sample one additional bit after the data bits
    and store it in `UartByte.extra`. This is how we model:
        - 9N1 (MDB-style): extra_bit=True, no parity check
        - 8N1: extra_bit=False
        - 8E1/8O1: extra_bit=True with a parity check (we don't enforce it)
    """
    if end_t is None:
        end_t = timeline[-1][0]

    bit_us = 1_000_000 / baud
    out: List[UartByte] = []

    # Find every falling edge (start bit).
    starts: List[int] = []
    for i in range(1, len(timeline)):
        prev_l = timeline[i - 1][1]
        cur_l = timeline[i][1]
        if prev_l == 1 and cur_l == 0:
            starts.append(timeline[i][0])

    i = 0
    while i < len(starts):
        t0 = starts[i]
        # Sample bit n at t0 + (n + 0.5) * bit_us. Bit 0 is start, bits
        # 1..data_bits are data (LSB first), then optional extra, then
        # stop bits.
        # Verify start bit is still 0 at mid-start.
        s_start = level_at(timeline, t0 + int(bit_us * 0.5))
        if s_start != 0:
            i += 1
            continue
        v = 0
        for b in range(data_bits):
            t = t0 + int(bit_us * (1.5 + b))
            if t > end_t:
                break
            bit = level_at(timeline, t)
            if bit:
                v |= 1 << b
        extra_val = None
        if extra_bit:
            t = t0 + int(bit_us * (1.5 + data_bits))
            extra_val = level_at(timeline, t) if t <= end_t else 0
        # Stop bit(s): must read 1.
        framing_ok = True
        for s in range(stop_bits):
            t = t0 + int(bit_us * (1.5 + data_bits + (1 if extra_bit else 0) + s))
            if t > end_t:
                framing_ok = False
                break
            if level_at(timeline, t) != 1:
                framing_ok = False
        parity_ok: Optional[bool] = None
        if parity in ("even", "odd") and extra_val is not None:
            expected = _byte_parity(v)
            if parity == "odd":
                expected ^= 1
            parity_ok = (extra_val == expected)
        out.append(UartByte(t0, data_bits, v, extra_val, framing_ok, parity_ok))

        # Skip to past this byte's stop bit(s) so we don't re-trigger on
        # mid-byte zero->one transitions counted as "starts".
        end_byte = t0 + int(bit_us * (1 + data_bits + (1 if extra_bit else 0) + stop_bits))
        while i < len(starts) and starts[i] < end_byte:
            i += 1
    return out


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------

def grade_decode(bytes_list: List[UartByte]) -> Tuple[int, int, int, float]:
    total = len(bytes_list)
    bad = sum(1 for b in bytes_list if not b.framing_ok)
    pbad = sum(1 for b in bytes_list if b.parity_ok is False)
    rate = (bad + pbad) / total if total else 1.0
    return total, bad, pbad, rate


def packetize(bytes_list: List[UartByte], idle_gap_us: int) -> List[List[UartByte]]:
    pkts: List[List[UartByte]] = []
    cur: List[UartByte] = []
    last_end = -1
    bit_us_guess = 105  # ≈ 9600 baud, only used for end-of-byte estimate
    for b in bytes_list:
        nominal_len = int(bit_us_guess * (1 + b.bits + 1))
        end = b.t_start_us + nominal_len
        if cur and (b.t_start_us - last_end) > idle_gap_us:
            pkts.append(cur)
            cur = []
        cur.append(b)
        last_end = end
    if cur:
        pkts.append(cur)
    return pkts


def fmt_byte(b: UartByte) -> str:
    base = f"{b.value:02X}"
    if b.extra is not None:
        base += f"({b.extra})"
    if not b.framing_ok:
        base += "!"
    if b.parity_ok is False:
        base += "p"
    return base


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

FRAME_PRESETS = {
    "8N1": dict(data_bits=8, extra_bit=False, stop_bits=1, parity=None),
    "8N2": dict(data_bits=8, extra_bit=False, stop_bits=2, parity=None),
    "9N1": dict(data_bits=8, extra_bit=True,  stop_bits=1, parity=None),
    "9N2": dict(data_bits=8, extra_bit=True,  stop_bits=2, parity=None),
    # Same wire layout as 9N1/9N2, but the 9th bit is interpreted as
    # an even-parity bit and verified against the data byte. Used by
    # MEI Protocol A and Necta Executive.
    "8E1": dict(data_bits=8, extra_bit=True,  stop_bits=1, parity="even"),
    "8E2": dict(data_bits=8, extra_bit=True,  stop_bits=2, parity="even"),
    "8O1": dict(data_bits=8, extra_bit=True,  stop_bits=1, parity="odd"),
}


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("vcd")
    p.add_argument("--channel", action="append", default=[],
                   help="Channel name(s) to analyze (default: all). Names are "
                        "as in the VCD $var line, e.g. ch2_gpio4.")
    p.add_argument("--baud", type=int, help="Force baudrate (else auto-detect).")
    p.add_argument("--frame", choices=list(FRAME_PRESETS.keys()),
                   help="Force frame layout (else try them all).")
    p.add_argument("--dump", type=int, default=0,
                   help="Print the first N decoded bytes per channel.")
    p.add_argument("--packets", type=int, default=0,
                   help="Group dump output into packets separated by an "
                        "idle gap of >= N microseconds (default 0 = off).")
    args = p.parse_args()

    print(f"[load] {args.vcd}", file=sys.stderr)
    sigs_by_name, edges = parse_vcd(args.vcd)

    print(f"[load] channels: {', '.join(sigs_by_name.keys())}", file=sys.stderr)
    print(f"[load] total edges: {len(edges)}", file=sys.stderr)

    targets = args.channel if args.channel else list(sigs_by_name.keys())
    for name in targets:
        if name not in sigs_by_name:
            print(f"[skip] channel {name} not in file", file=sys.stderr)
            continue
        sig = sigs_by_name[name]
        initial = sig.initial if sig.initial is not None else 1
        timeline = reconstruct_levels(edges, sig.symbol, initial)
        if len(timeline) < 5:
            print(f"\n=== {name} ({sig.symbol}) — no edges, skipping")
            continue

        baud = args.baud or detect_baud(timeline)
        print(f"\n=== {name} ({sig.symbol}): edges={len(timeline)-1}, "
              f"detected baud={baud}")
        if not baud:
            print("    -> could not detect baud; pass --baud explicitly")
            continue

        frames = [args.frame] if args.frame else list(FRAME_PRESETS.keys())
        scored = []
        for frame in frames:
            decoded = decode_uart(timeline, baud, **FRAME_PRESETS[frame])
            total, bad, pbad, rate = grade_decode(decoded)
            scored.append((frame, total, bad, pbad, rate, decoded))
            extra = ""
            if FRAME_PRESETS[frame]["parity"] is not None:
                extra = f", parity_errors={pbad} ({pbad / max(1, total) * 100:5.1f}%)"
            print(f"    {frame}: {total:>5} bytes, framing_errors={bad} "
                  f"({bad / max(1, total) * 100:5.1f}%){extra}")
        # Pick the layout with the lowest combined error rate.
        scored.sort(key=lambda r: (r[4], -r[1]))
        best = scored[0]
        print(f"    best layout: {best[0]}")

        if args.dump:
            decoded = best[5]
            print(f"    --- first {args.dump} decoded bytes ({best[0]}):")
            if args.packets:
                pkts = packetize(decoded[: args.dump], args.packets)
                for pkt in pkts:
                    t = pkt[0].t_start_us
                    hex_ = " ".join(fmt_byte(b) for b in pkt)
                    print(f"    [{t/1000:>10.3f} ms] [{len(pkt):>2}] {hex_}")
            else:
                for b in decoded[: args.dump]:
                    print(f"    [{b.t_start_us/1000:>10.3f} ms] {fmt_byte(b)}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
