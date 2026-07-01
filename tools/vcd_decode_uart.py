"""Decode UART traffic from a pulse_inspector_vcd capture.

Usage:
    python tools/vcd_decode_uart.py <capture.vcd> [--baud N] [--bits 8|9] \
                                    [--parity N|E|O] [--invert CH]

Parses the VCD file, reconstructs the level timeline for every $var,
then decodes each channel as asynchronous serial (start bit = falling
edge, data LSB-first, stop bit high). Bytes are printed with their
timestamp, channel name and ASCII preview so you can correlate master
and slave traffic side by side.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from typing import Dict, List, Tuple


# -----------------------------------------------------------------------------
# VCD parser (minimal, just enough for files produced by pulse_inspector_vcd)
# -----------------------------------------------------------------------------

@dataclass
class Channel:
    name: str
    # List of (t_us, level) edges, sorted ascending by t_us.
    # level is 0, 1 or -1 for x/unknown.
    edges: List[Tuple[int, int]] = field(default_factory=list)


def parse_vcd(path: str) -> Dict[str, Channel]:
    """Return {vcd_symbol: Channel} built from the capture file."""
    channels: Dict[str, Channel] = {}
    current_t = 0
    header_done = False

    var_re = re.compile(r"\$var\s+wire\s+\d+\s+(\S)\s+(\S+)\s+\$end")

    with open(path, "r", encoding="utf-8", errors="replace") as fp:
        for raw in fp:
            line = raw.strip()
            if not line:
                continue
            if not header_done:
                m = var_re.match(line)
                if m:
                    sym, name = m.group(1), m.group(2)
                    channels[sym] = Channel(name=name)
                elif line == "$enddefinitions $end":
                    header_done = True
                continue

            if line.startswith("#"):
                try:
                    current_t = int(line[1:])
                except ValueError:
                    pass
                continue

            # Value-change line: <level><sym>, level is 0/1/x/z.
            if len(line) >= 2 and line[1] in channels:
                lvl_ch = line[0]
                sym = line[1]
                if lvl_ch == "0":
                    lvl = 0
                elif lvl_ch == "1":
                    lvl = 1
                else:
                    lvl = -1
                channels[sym].edges.append((current_t, lvl))

    return channels


# -----------------------------------------------------------------------------
# UART decoder
# -----------------------------------------------------------------------------

@dataclass
class UartFrame:
    t_us: int      # microsecond timestamp of the start bit
    value: int     # decoded byte (or 9-bit word if bits==9)
    ok: bool       # framing / parity ok
    note: str = "" # "framing error", "parity error", etc.


def level_at(edges: List[Tuple[int, int]], t: int) -> int:
    """Return the line level at time t, given a sorted list of edges.

    We assume edges[0][0] == 0 and its level is the initial state. If the
    earliest edge is after t we return -1 (unknown).
    """
    if not edges or edges[0][0] > t:
        return -1
    lo, hi = 0, len(edges) - 1
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if edges[mid][0] <= t:
            lo = mid
        else:
            hi = mid - 1
    return edges[lo][1]


def decode_uart(
    ch: Channel,
    baud: int,
    data_bits: int,
    parity: str,
    invert: bool,
) -> List[UartFrame]:
    """Scan ``ch`` for UART frames and decode them."""
    if not ch.edges:
        return []

    bit_us = 1_000_000.0 / baud
    frames: List[UartFrame] = []

    # Fold inversion into a simple wrapper.
    def lvl(t: int) -> int:
        v = level_at(ch.edges, t)
        if v < 0:
            return v
        return 1 - v if invert else v

    # Walk every falling edge — a 1 -> 0 transition is a candidate start bit.
    # We skip over anything inside an already-consumed frame.
    edges = ch.edges
    if invert:
        edges = [(t, 1 - l if l >= 0 else l) for t, l in ch.edges]

    n_extra = 0
    if parity in ("E", "O"):
        n_extra += 1
    total_bits = 1 + data_bits + n_extra + 1  # start + data + parity + stop

    last_frame_end_us = -1
    for idx, (t, l) in enumerate(edges):
        if l != 0:
            continue
        # falling edge only
        prev_lvl = edges[idx - 1][1] if idx > 0 else 1
        if prev_lvl != 1:
            continue
        # Allow half a bit period of clock skew so back-to-back frames at
        # slightly divergent baud rates don't get swallowed.
        if t + bit_us / 2 < last_frame_end_us:
            continue

        # Decode this frame.
        start_t = t
        bit_vals: List[int] = []
        for i in range(data_bits + n_extra):
            sample_t = int(round(start_t + (1.5 + i) * bit_us))
            bit_vals.append(lvl(sample_t))
        stop_sample = int(round(start_t + (1.5 + data_bits + n_extra) * bit_us))
        stop_lvl = lvl(stop_sample)

        # Assemble data byte (LSB-first).
        val = 0
        for i in range(data_bits):
            if bit_vals[i] == 1:
                val |= 1 << i

        ok = True
        note = ""
        if stop_lvl != 1:
            ok = False
            note = "framing error (stop != 1)"
        if parity in ("E", "O") and ok:
            parity_bit = bit_vals[data_bits]
            popcount = bin(val).count("1") + parity_bit
            if parity == "E" and (popcount % 2) != 0:
                ok = False
                note = "parity error"
            if parity == "O" and (popcount % 2) != 1:
                ok = False
                note = "parity error"

        frames.append(UartFrame(t_us=start_t, value=val, ok=ok, note=note))
        last_frame_end_us = start_t + int(round(total_bits * bit_us))

    return frames


# -----------------------------------------------------------------------------
# Pretty printer
# -----------------------------------------------------------------------------

def ascii_preview(b: int) -> str:
    if b == 9:
        return "TAB"
    if b == 10:
        return "LF"
    if b == 13:
        return "CR"
    if 32 <= b < 127:
        return chr(b)
    return "."


def print_frames(
    frames_by_ch: Dict[str, List[UartFrame]],
    group_gap_us: int,
    show_role: bool,
) -> None:
    # Merge all frames, tagged with channel name, sorted by time.
    merged: List[Tuple[int, str, UartFrame]] = []
    for name, frames in frames_by_ch.items():
        for f in frames:
            merged.append((f.t_us, name, f))
    merged.sort(key=lambda x: x[0])

    if not merged:
        print("(no UART frames decoded)")
        return

    prev_t = merged[0][0]
    if show_role:
        print(f"{'t, ms':>10}  {'dt, us':>8}  {'ch':<14}  {'role':<6}  "
              f"{'byte':>4}  {'9b':>2}  {'bin':>10}  ascii  notes")
    else:
        print(f"{'t, ms':>10}  {'dt, us':>8}  {'ch':<14}  {'hex':>6}  {'9b':>2}  "
              f"{'bin':>10}  ascii  notes")
    print("-" * 80)
    for t, name, f in merged:
        dt = t - prev_t
        if dt > group_gap_us:
            print()  # visual separator for poll cycles
        marker = "   " if f.ok else "ERR"
        data8 = f.value & 0xFF
        bit9 = (f.value >> 8) & 1
        if show_role:
            role = "MSTR" if bit9 else "slv"
            print(f"{t/1000:10.1f}  {dt:>8}  {name:<14}  {role:<6}  "
                  f"0x{data8:>02X}  {bit9:>2}  {data8:>08b}  "
                  f"{ascii_preview(data8):<5} {marker} {f.note}")
        else:
            print(f"{t/1000:10.1f}  {dt:>8}  {name:<14}  0x{f.value:>04X}  "
                  f"{bit9:>2}  {data8:>08b}  {ascii_preview(data8):<5} "
                  f"{marker} {f.note}")
        prev_t = t


# -----------------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("path")
    ap.add_argument("--baud", type=int, default=9600)
    ap.add_argument("--bits", type=int, default=8, choices=(5, 6, 7, 8, 9))
    ap.add_argument("--parity", default="N", choices=("N", "E", "O"))
    ap.add_argument("--invert", action="append", default=[],
                    help="Channel name whose logic is inverted (repeatable). "
                         "Use substring match, e.g. --invert ch1")
    ap.add_argument("--invert-all", action="store_true",
                    help="Invert every channel.")
    ap.add_argument("--group-gap-us", type=int, default=50_000,
                    help="Blank line separating frames that are more than "
                         "this many microseconds apart.")
    ap.add_argument("--ignore", action="append", default=[],
                    help="Skip channels whose name contains this substring "
                         "(repeatable).")
    ap.add_argument("--role", action="store_true",
                    help="When --bits 9 and --parity N, treat bit 9 as "
                         "address/data marker (1 = master/addr, 0 = slave/data) "
                         "and pretty-print accordingly.")
    args = ap.parse_args()

    channels = parse_vcd(args.path)
    if not channels:
        print("No channels in VCD file.", file=sys.stderr)
        return 1

    print(f"Baud: {args.baud}  bits: {args.bits}  parity: {args.parity}  "
          f"invert: {args.invert or '(none)'}")
    print(f"Channels: " + ", ".join(f"{s}={c.name}" for s, c in channels.items()))
    print()

    frames_by_ch: Dict[str, List[UartFrame]] = {}
    for sym, ch in channels.items():
        if any(tag in ch.name for tag in args.ignore):
            continue
        invert = args.invert_all or any(tag in ch.name for tag in args.invert)
        frames_by_ch[ch.name] = decode_uart(
            ch, baud=args.baud, data_bits=args.bits, parity=args.parity,
            invert=invert,
        )

    print_frames(frames_by_ch, args.group_gap_us, args.role)
    return 0


if __name__ == "__main__":
    sys.exit(main())
