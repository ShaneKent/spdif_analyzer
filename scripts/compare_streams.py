#!/usr/bin/env python3
"""Compare captured PCM containers without resampling or deleting timeline holes.

Positive lag means B[k + lag] == A[k]. Indices use each export's capture/trigger
origin and nearest initial frame phase; subsequent steps round adjacent channel
positions, avoiding accumulated absolute-grid clock drift. Different origins are
not automatically synchronized. Silence/repetition cannot prove a unique delay.
Exit: 0 unique exact overlap; 1 mismatch/holes/ambiguity/insufficient evidence;
2 malformed/incompatible inputs. All reports use JSON schema_version 1.
"""
from __future__ import annotations

import argparse
from math import gcd
import csv
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation, ROUND_HALF_UP, getcontext
import json
from pathlib import Path
import sys

getcontext().prec = 60


class InputError(ValueError):
    pass


def decimal(value):
    try:
        result = Decimal(str(value))
    except (InvalidOperation, ValueError) as exc:
        raise InputError(f"Invalid numeric value: {value}") from exc
    if not result.is_finite():
        raise InputError("Non-finite numeric value")
    return result


def rounded(value):
    return int(value.to_integral_value(rounding=ROUND_HALF_UP))


def boolean(value):
    if value is True or str(value).lower() in ("1", "true"):
        return True
    if value is False or str(value).lower() in ("0", "false"):
        return False
    raise InputError(f"Invalid boolean: {value}")


def integer(value):
    try:
        text = str(value)
        return int(text, 16) if text.lower().startswith("0x") else int(text)
    except (ValueError, TypeError) as exc:
        raise InputError(f"Invalid integer: {value}") from exc


def sample_index(value):
    result = integer(value)
    if not 0 <= result <= (1 << 64) - 1:
        raise InputError("JSONL sample index outside uint64 range")
    return result


@dataclass
class Row:
    time: Decimal
    channel: int
    raw: int
    pcm: bool
    valid: bool
    bits: int | None
    measured: Decimal | None
    sample: str | None


@dataclass
class Source:
    path: str
    rows: list
    rates: set
    origin: str
    capture: str | None
    override_bits: int | None = None


def selected(time, args):
    return ((args.start_seconds is None or time >= args.start_seconds) and
            (args.end_seconds is None or time < args.end_seconds))


def add_row(rows, time, channel, raw, pcm, valid, bits, measured, sample, args):
    if not selected(time, args):
        return
    if channel not in (0, 1) or not 0 <= raw < (1 << 28):
        raise InputError("Expected channel A/B and 28-bit raw_slots_4_31")
    if bits is not None and not 16 <= bits <= 24:
        raise InputError("Declared PCM layout outside supported 16..24 bits")
    if rows and time < rows[-1].time:
        raise InputError("Subframe timestamps must be monotonic")
    if len(rows) >= 2 * args.max_frames:
        raise InputError("Selected input exceeds --max-frames; select a shorter time interval")
    rows.append(Row(time, channel, raw, pcm, valid, bits, measured, sample))


def load(path, args):
    rows, rates = [], set()
    origin, capture = "trigger-relative CSV seconds", None
    override_bits = None
    with open(path, encoding="utf-8-sig", newline="") as stream:
        first = next((line.lstrip() for line in stream if line.strip()), "")
        stream.seek(0)
        if first.startswith("{"):
            metadata = None
            statuses = []
            prior_status = {}
            for line_no, line in enumerate(stream, 1):
                if not line.strip():
                    continue
                try:
                    item = json.loads(line)
                    if not isinstance(item, dict):
                        raise InputError("JSONL records must be objects")
                    if item.get("schema_version") != 1:
                        raise InputError("Unsupported JSONL schema version")
                    event = item.get("event")
                    if event == "metadata":
                        if metadata is not None or rows:
                            raise InputError("Expected one initial JSONL metadata record")
                        metadata = item
                        capture_rate = decimal(item["capture_rate_hz"])
                        if capture_rate <= 0:
                            raise InputError("capture_rate_hz must be positive")
                        capture = str(capture_rate)
                        origin = str(sample_index(item.get("origin_sample", "0")))
                        override_bits = integer(item.get("pcm_bits_override", 0)) or None
                        if override_bits is not None and not 16 <= override_bits <= 24:
                            raise InputError("Metadata PCM override must be 16..24 bits")
                        if decimal(item.get("nominal_rate_hz", 0)) > 0:
                            rates.add(decimal(item["nominal_rate_hz"]))
                    elif event in ("subframe", "channel_status"):
                        if metadata is None:
                            raise InputError("JSONL requires initial metadata with capture rate/origin")
                        start = sample_index(item["start_sample"])
                        end_sample = sample_index(item["end_sample_inclusive"])
                        if end_sample < start:
                            raise InputError("JSONL interval end precedes its start")
                        time = Decimal(start - int(origin)) / capture_rate
                        if event == "channel_status":
                            if (boolean(item.get("integrity", False)) and boolean(item.get("supported", False))
                                    and not boolean(item.get("non_audio", True))):
                                end = Decimal(end_sample - int(origin)) / capture_rate
                                rate = decimal(item.get("declared_sample_rate_hz", 0))
                                if rate > 0:
                                    channel = integer(item["channel"])
                                    if channel not in (0, 1):
                                        raise InputError("Channel status requires channel A/B")
                                    declaration = (time, end, rate, channel)
                                    if args.start_seconds is not None and end < args.start_seconds:
                                        if channel not in prior_status or end >= prior_status[channel][1]:
                                            prior_status[channel] = declaration
                                    elif args.end_seconds is None or time < args.end_seconds:
                                        if len(statuses) >= 2 * args.max_frames:
                                            raise InputError("Selected status records exceed --max-frames budget")
                                        statuses.append(declaration)
                            continue
                        layout = item.get("layout", {})
                        if not isinstance(layout, dict):
                            raise InputError("Subframe layout must be an object")
                        bits = integer(layout["bits"]) if boolean(layout.get("known", False)) else None
                        add_row(rows, time, integer(item["channel"]), integer(item["raw_slots_4_31"]),
                                str(item.get("content", "")).upper() == "PCM",
                                boolean(item["parity_ok"]) and not boolean(item["V_invalid"]),
                                bits, decimal(item["sample_rate_hz"]) if "sample_rate_hz" in item else None,
                                str(start), args)
                except (KeyError, TypeError, ValueError) as exc:
                    raise InputError(f"{path}:{line_no}: {exc}") from exc
            if metadata is None:
                raise InputError("Missing JSONL metadata")
            if rows:
                lo, hi = rows[0].time, rows[-1].time
                for ch in (0, 1):
                    # Status records cover their original block; the latest prior
                    # declaration remains relevant after the final confirmed block.
                    channel_records = [s for s in statuses + list(prior_status.values()) if s[3] == ch]
                    prior = [s for s in channel_records if s[1] < lo]
                    if prior:
                        rates.add(max(prior, key=lambda s: s[1])[2])
                    rates.update(s[2] for s in channel_records if s[0] <= hi and s[1] >= lo)
        else:
            reader = csv.DictReader(stream)
            required = {"start_seconds", "channel", "raw_slots_4_31", "content", "parity_ok", "V_invalid"}
            if not required.issubset(reader.fieldnames or []):
                raise InputError("CSV requires exported subframe columns: " + ", ".join(sorted(required)))
            for line_no, item in enumerate(reader, 2):
                try:
                    time = decimal(item["start_seconds"])
                    channel = {"A": 0, "B": 1, "0": 0, "1": 1}.get(item["channel"].strip())
                    bits = next((integer(item[k]) for k in ("layout_bits", "pcm_bits", "bits", "declared_word_length")
                                 if item.get(k) not in (None, "", "0")), None)
                    if item.get("layout_known") not in (None, "") and not boolean(item["layout_known"]):
                        bits = None
                    pcm = item["content"].upper() == "PCM"
                    if selected(time, args) and pcm:
                        for key in ("declared_sample_rate_hz", "nominal_rate_hz"):
                            if item.get(key) not in (None, "", "0"):
                                rates.add(decimal(item[key]))
                    measured = item.get("measured_sample_rate_hz")
                    add_row(rows, time, channel, integer(item["raw_slots_4_31"]), pcm,
                            boolean(item["parity_ok"]) and not boolean(item["V_invalid"]), bits,
                            decimal(measured) if measured else None, None, args)
                except (KeyError, TypeError, ValueError) as exc:
                    raise InputError(f"{path}:{line_no}: {exc}") from exc
    if not rows or not any(row.pcm for row in rows):
        raise InputError(f"{path}: selected interval has no confirmed PCM")
    return Source(str(path), rows, rates, origin, capture, override_bits)


def config(source, args):
    rates = source.rates | ({args.rate} if args.rate is not None else set())
    widths = []
    for channel in (0, 1):
        bits = {row.bits for row in source.rows if row.channel == channel and row.pcm and row.bits is not None}
        if source.override_bits is not None:
            bits.add(source.override_bits)
        if args.bits is not None:
            bits.add(args.bits)
        if len(bits) != 1:
            raise InputError(f"{source.path}: missing or changing channel {channel} PCM layout; supply --bits for missing metadata, or select a compatible --start-seconds/--end-seconds interval")
        width = next(iter(bits))
        if not 16 <= width <= 24:
            raise InputError("PCM width must be 16..24")
        widths.append(width)
    if len(rates) != 1:
        raise InputError(f"{source.path}: missing or changing nominal rate; supply --rate for missing metadata, or select a compatible --start-seconds/--end-seconds interval")
    rate = next(iter(rates))
    if rate <= 0:
        raise InputError("Rate must be positive")
    for row in source.rows:
        if row.pcm and row.measured is not None and abs(row.measured / rate - 1) > Decimal("0.02"):
            raise InputError(f"{source.path}: observed PCM transport rate conflicts with nominal rate; select a compatible interval")
    return rate, tuple(widths)


def timeline(source, rate, widths, args):
    anchor = source.rows[0]
    base = rounded(anchor.time * rate - Decimal(anchor.channel) / 2)
    previous = {}
    frames = {}
    for row in source.rows:
        if row.channel in previous:
            old, index = previous[row.channel]
            step = (row.time - old) * rate
            delta = rounded(step)
            if delta < 1 or abs(step - delta) > Decimal("0.2") or (delta == 1 and abs(step - 1) > Decimal("0.01")):
                raise InputError(f"{source.path}: non-integer/duplicate frame timing; select an uninterrupted interval")
            index += delta
        else:
            index = base + rounded((row.time - anchor.time) * rate - Decimal(row.channel - anchor.channel) / 2)
        previous[row.channel] = row.time, index
        slots = frames.setdefault(index, [None, None])
        if slots[row.channel] is not None:
            raise InputError("Duplicate frame/channel position")
        slots[row.channel] = row
    result = {}
    for index, pair in frames.items():
        good = all(row is not None and row.pcm and row.valid and
                   (row.bits == widths[channel] or (row.bits is None and (args.bits is not None or source.override_bits is not None))) for channel, row in enumerate(pair))
        if good and abs((pair[1].time - pair[0].time) * rate - Decimal("0.5")) > Decimal("0.2"):
            good = False
        values = None
        if good:
            values = []
            for channel, row in enumerate(pair):
                width = widths[channel]
                value = (row.raw & 0xffffff) >> (24 - width)
                values.append(value - (1 << width) if value & (1 << (width - 1)) else value)
            if args.swap_b and source is args.source_b:
                values.reverse()
            values = tuple(values)
        result[index] = (values, pair)
    return result


def minimal_period(values):
    if not values:
        return 0
    prefix = [0] * len(values)
    for i in range(1, len(values)):
        j = prefix[i - 1]
        while j and values[i] != values[j]:
            j = prefix[j - 1]
        if values[i] == values[j]:
            j += 1
        prefix[i] = j
    return len(values) - prefix[-1]


def indexed_period(frames, low, high, budget, min_constraints=3):
    """Find exact repetitive evidence without joining values across time holes.

    Candidate periods come from actual runs/repeated-value positions. Every
    candidate is checked against ORIGINAL frame-index residues; compressed known
    values are only a candidate generator. Validation is charged to a shared
    budget so sparse/repetitive adversarial data cannot cause unbounded work.
    """
    known = [(index, frames[index][0]) for index in sorted(frames)
             if low <= index <= high and frames[index][0] is not None]
    if len(known) < 3:
        return None
    values = [value for _, value in known]
    span = known[-1][0] - known[0][0] + 1
    period = minimal_period(values)
    if span == len(known):
        constraints = len(known) - period
        return period if (constraints >= min_constraints or (period * 2 <= len(known) and constraints >= 3)) else None
    maximum = span - 1
    known_map = dict(known)
    if not maximum:
        return None
    # Longest actual contiguous run often reveals the period despite a missing
    # sample elsewhere. It is a proposal, not permission to compress holes.
    run_start = best_start = 0
    best_length = 1
    for i in range(1, len(known) + 1):
        if i == len(known) or known[i][0] != known[i - 1][0] + 1:
            if i - run_start > best_length:
                best_start, best_length = run_start, i - run_start
            run_start = i
    proposals = []
    run = values[best_start:best_start + best_length]
    run_period = minimal_period(run)
    if run_period and run_period < len(run):
        proposals.append(run_period)
    if period < len(known):
        proposals.append(known[period][0] - known[0][0])
    positions_by_value = {}
    for index, value in known:
        positions_by_value.setdefault(value, []).append(index)
    repeated = sorted((positions for positions in positions_by_value.values() if len(positions) >= 2),
                      key=len, reverse=True)
    seen = set()
    # Consider every repeated value. The most common value may appear only in
    # the first cycle while less common values reveal the true cross-cycle lag.
    # Fallback pair generation and candidate validation share a hard work budget.
    def candidates():
        yield from proposals
        for positions in repeated:
            common = 0
            for position in positions[1:]:
                if budget[0] < 1:
                    raise InputError("Periodicity candidate generation exceeds --max-work; narrow the interval/lag or increase the work budget")
                budget[0] -= 1
                common = gcd(common, position - positions[0])
            yield common
            # Later pairs may reveal a period absent from every first-anchor
            # difference. Charge generation as well as validation, including
            # duplicates, so ALL-pair enumeration has a hard work bound.
            for first in range(len(positions) - 1):
                for second in range(first + 1, len(positions)):
                    if budget[0] < 1:
                        raise InputError("Periodicity candidate generation exceeds --max-work; narrow the interval/lag or increase the work budget")
                    budget[0] -= 1
                    difference = positions[second] - positions[first]
                    if difference > maximum:
                        break
                    yield difference
    for candidate_period in candidates():
        if not 1 <= candidate_period <= maximum or candidate_period in seen:
            continue
        seen.add(candidate_period)
        if budget[0] < 2 * len(known):
            raise InputError("Periodicity audit exceeds --max-work; narrow the interval/lag or increase the work budget")
        budget[0] -= 2 * len(known)
        residues = {}
        consistent = True
        for index, value in known:
            residue = index % candidate_period
            if residue in residues and residues[residue] != value:
                consistent = False
                break
            residues[residue] = value
        if consistent:
            constraints = sum(known_map.get(index + candidate_period) == value for index, value in known)
            if constraints >= min_constraints or (candidate_period * 2 <= span and constraints >= 3):
                return candidate_period
    return None


def candidate(a, b, lag, bounds):
    amin, amax, bmin, bmax = bounds
    low = max(amin, bmin - lag)
    high = min(amax, bmax - lag)
    compared = mismatches = 0
    if high >= low:
        for index, (values, _) in a.items():
            other = b.get(index + lag)
            if low <= index <= high and values is not None and other is not None and other[0] is not None:
                compared += 1
                mismatches += values != other[0]
    return {"lag_frames": lag, "compared_frames": compared, "mismatch_frames": mismatches,
            "hole_frames": max(0, high - low + 1 - compared)}, low, high


def compare(a, b, args):
    if min(sum(v[0] is not None for v in a.values()), sum(v[0] is not None for v in b.values())) < args.min_overlap:
        return {"status": "insufficient_evidence", "reason": "Too few valid paired PCM frames for minimum overlap"}, 1
    probes = len(a) * (2 * args.max_lag + 1)
    if probes > args.max_work:
        raise InputError(f"Search requires {probes} pair probes, above --max-work; narrow interval/lag or explicitly increase budget")
    best, ties, total_ties = None, [], 0
    bounds = min(a), max(a), min(b), max(b)
    for lag in range(-args.max_lag, args.max_lag + 1):
        score, low, high = candidate(a, b, lag, bounds)
        count, bad = score["compared_frames"], score["mismatch_frames"]
        if count < args.min_overlap or Decimal(count - bad) / count < args.min_match:
            continue
        cross = 0 if best is None else bad * best["compared_frames"] - best["mismatch_frames"] * count
        if best is None or cross < 0:
            best, ties, total_ties = score, [score], 1
        elif cross == 0:
            total_ties += 1
            if len(ties) < 128:
                ties.append(score)
    if best is None:
        return {"status": "insufficient_evidence", "reason": "No lag meets minimum overlap and exact-match fraction"}, 1
    if total_ties > 1:
        return {"status": "ambiguous", "reason": "Multiple offsets have equally good mismatch fractions", "candidate_count": total_ties, "candidates_truncated": total_ties > len(ties), "candidates": ties}, 1
    score, low, high = candidate(a, b, best["lag_frames"], bounds)
    lag = score["lag_frames"]
    mismatch_locations, valid_positions, values, other_values = [], [], [], []
    for index in sorted(a):
        first, pair_a = a[index]
        other = b.get(index + lag)
        if index < low or index > high or first is None or other is None or other[0] is None:
            continue
        second, pair_b = other
        valid_positions.append(index)
        values.append(first)
        other_values.append(second)
        if first != second and len(mismatch_locations) < args.max_mismatches:
            mismatch_locations.append({"a_frame": str(index), "b_frame": str(index + lag),
                                       "a_values": first, "b_values": second,
                                       "a_start_seconds": str(pair_a[0].time), "b_start_seconds": str(pair_b[0].time),
                                       "a_start_sample": pair_a[0].sample, "b_start_sample": pair_b[0].sample})
    constant = all(value == values[0] for value in values) or all(value == other_values[0] for value in other_values)
    periodicity_budget = [args.max_work - probes]
    # Audit the full selected sources as well as the winning overlap: an edge
    # lag can otherwise crop a two-cycle source down to one unique-looking cycle.
    intervals = [(a, min(a), max(a)), (b, min(b), max(b))]
    if (low, high) != (min(a), max(a)):
        intervals.append((a, low, high))
    if (low + lag, high + lag) != (min(b), max(b)):
        intervals.append((b, low + lag, high + lag))
    repeating = None
    if not constant:
        for source_frames, first, last in intervals:
            repeating = indexed_period(source_frames, first, last, periodicity_budget, args.min_overlap)
            if repeating:
                break
    if constant or repeating:
        return {"status": "ambiguous", "reason": "Constant or repetitive content cannot establish a unique delay", "repetition_period_frames": 1 if constant else repeating, "repetition_period_note": "One consistent observed period witness, not necessarily the fundamental period", "candidates": [score]}, 1
    holes, cursor = [], low
    for position in valid_positions:
        if position > cursor and len(holes) < args.max_mismatches:
            holes.append({"a_first_frame": str(cursor), "a_last_frame": str(position - 1),
                          "b_first_frame": str(cursor + lag), "b_last_frame": str(position - 1 + lag)})
        cursor = position + 1
    if cursor <= high and len(holes) < args.max_mismatches:
        holes.append({"a_first_frame": str(cursor), "a_last_frame": str(high),
                      "b_first_frame": str(cursor + lag), "b_last_frame": str(high + lag)})
    exact = score["mismatch_frames"] == 0 and score["hole_frames"] == 0
    return {"status": "aligned" if exact else "mismatch", **score,
            "offset_seconds": str(Decimal(lag) / args.rate_resolved),
            "offset_seconds_note": "Integer-frame component relative to export origins; excludes within-frame phase and independent clock calibration",
            "overlap_first_a_frame": str(low), "overlap_last_a_frame": str(high),
            "mismatches": mismatch_locations, "holes": holes,
            "locations_truncated": score["mismatch_frames"] > len(mismatch_locations)}, 0 if exact else 1


def parser():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("a", type=Path);p.add_argument("b", type=Path)
    p.add_argument("--max-lag", type=int, default=128)
    p.add_argument("--min-overlap", type=int, default=32)
    p.add_argument("--min-match", type=decimal, default=Decimal("0.9"), help="Minimum equal-frame fraction to establish alignment evidence; mismatches still fail")
    p.add_argument("--rate", type=decimal, help="Nominal PCM rate when metadata is missing; must agree with known declarations")
    p.add_argument("--bits", type=int, choices=range(16, 25), help="Explicit PCM word-width override for missing layout metadata; cannot promote non-PCM")
    p.add_argument("--start-seconds", type=decimal);p.add_argument("--end-seconds", type=decimal)
    p.add_argument("--swap-b", action="store_true", help="Explicitly exchange input B channel A/B sample values")
    p.add_argument("--max-mismatches", type=int, default=100, help="Cap mismatch locations and hole ranges in output (counts remain exact)")
    p.add_argument("--max-work", type=int, default=20_000_000)
    p.add_argument("--max-frames", type=int, default=1_000_000)
    return p


def main(argv=None):
    p = parser();args = p.parse_args(argv)
    if not (0 <= args.max_lag <= 1_000_000 and args.min_overlap >= 3 and
            0 <= args.max_mismatches <= 10000 and 1 <= args.max_work <= 1_000_000_000 and
            1 <= args.max_frames <= 10_000_000 and 0 <= args.min_match <= 1):
        p.error("Invalid search/report bounds (see --help)")
    if args.start_seconds is not None and args.end_seconds is not None and args.end_seconds <= args.start_seconds:
        p.error("--end-seconds must follow --start-seconds")
    base = {"schema_version": 1, "lag_convention": "B[k + lag_frames] == A[k]",
            "scope": "Relative captured PCM alignment; no resampling or calibrated clock-jitter measurement",
            "channel_order": "A/B in both inputs; --swap-b explicitly reverses B values",
            "search_max_lag_frames": args.max_lag,
            "timeline_policy": "Indices use each export origin and first observed phase, including invalid/unknown samples; adjacent frame steps retain holes. Different capture origins are not automatically synchronized."}
    try:
        a = load(args.a, args);b = load(args.b, args);args.source_b = b
        rate_a, bits_a = config(a, args);rate_b, bits_b = config(b, args)
        if (rate_a, bits_a) != (rate_b, tuple(reversed(bits_b)) if args.swap_b else bits_b):
            raise InputError("Inputs have incompatible nominal rates or declared PCM widths; no resampling is performed")
        args.rate_resolved = rate_a
        frames_a, frames_b = timeline(a, rate_a, bits_a, args), timeline(b, rate_b, bits_b, args)
        report, status = compare(frames_a, frames_b, args)
        report.update({"rate_hz": str(rate_a), "bits": bits_a[0] if bits_a[0] == bits_a[1] else bits_a, "channel_bits": bits_a, "swap_b": args.swap_b,
                       "sources": [{"path": source.path, "origin": source.origin,
                                    "capture_rate_hz": source.capture, "layout_override_bits": args.bits or source.override_bits,
                                    "first_observed_seconds": str(source.rows[0].time),
                                    "first_observed_sample": source.rows[0].sample,
                                    "first_frame_index": str(min(frames)),
                                    "last_frame_index": str(max(frames)), "observed_frames": len(frames),
                                    "missing_frame_positions": max(frames) - min(frames) + 1 - len(frames),
                                    "invalid_or_incomplete_frames": sum(value[0] is None for value in frames.values())}
                                   for source, frames in ((a, frames_a), (b, frames_b))]})
    except (OSError, ValueError, csv.Error) as exc:
        report, status = {"status": "incompatible", "reason": str(exc)}, 2
    print(json.dumps({**base, **report}, separators=(",", ":")))
    return status


if __name__ == "__main__":
    sys.exit(main())
