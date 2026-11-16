#!/usr/bin/env python3
"""Compare observed IEC 61937 payloads without overriding transport evidence.

Input is the analyzer's version-1 JSONL, including raw subframes and content
events. This tool does not scan for new headers, decode audio, synchronize clocks,
or infer that an unmatched observation was dropped by a particular device.
"""
from __future__ import annotations

import argparse
from bisect import bisect_left, bisect_right
from collections import Counter
from dataclasses import dataclass, field
from decimal import Decimal, InvalidOperation
import hashlib
import json
from pathlib import Path
import sys


UINT64_MAX = (1 << 64) - 1
# Type: name, Pd units, maximum payload bits. Capacity excludes the four header
# words. E-AC-3 uses bytes; the other supported types use bits.
TYPES = {1: ("AC-3", "bits", 1536 * 32 - 64),
         11: ("DTS I", "bits", 512 * 32 - 64),
         12: ("DTS II", "bits", 1024 * 32 - 64),
         13: ("DTS III", "bits", 2048 * 32 - 64),
         21: ("E-AC-3", "bytes", 6144 * 32 - 64)}
DEFAULTS = {"max_input_bytes": 512 * 1024 * 1024, "max_line_bytes": 1024 * 1024,
            "max_records": 2000000, "max_subframes": 1000000,
            "max_bursts": 10000, "max_status_blocks": 20000,
            "max_payload_bytes": 64 * 1024 * 1024, "max_work": 20000000,
            "max_output_bytes": 16 * 1024 * 1024, "max_diff_bytes": 64}


class InputError(ValueError):
    pass


class Work:
    def __init__(self, maximum):
        self.maximum, self.used = maximum, 0

    def spend(self, count=1):
        self.used += count
        if self.used > self.maximum:
            raise InputError("Shared --max-work budget exceeded; select shorter inputs or raise the explicit budget")


def integer(value, name, maximum=UINT64_MAX):
    if type(value) is int:
        result = value
    elif isinstance(value, str) and value and len(value) <= 20 and value.isascii() and value.isdecimal():
        result = int(value)
    else:
        raise InputError(name + " must be an unsigned decimal integer")
    if not 0 <= result <= maximum:
        raise InputError(name + " outside supported unsigned range")
    return result


def boolean(value, name):
    if type(value) is not bool:
        raise InputError(name + " must be a JSON boolean")
    return value


def capture_rate(value):
    # Offline producers can declare fractional capture rates. Retain their
    # decimal representation without binary conversion or exponent expansion.
    if type(value) not in (int, str, Decimal):
        raise InputError("Invalid capture_rate_hz")
    try:
        number = Decimal(value)
    except InvalidOperation as exc:
        raise InputError("Invalid capture_rate_hz") from exc
    if not number.is_finite() or not 0 < number <= (1 << 32) - 1:
        raise InputError("Capture rate must be positive, finite and at most UINT32_MAX")
    return str(number)


def label(value, name, maximum=1024):
    if not isinstance(value, str) or len(value) > maximum:
        raise InputError(name + " must be a bounded string")
    return value


def interval(record):
    begin = integer(record["start_sample"], "start_sample")
    end = integer(record["end_sample_inclusive"], "end_sample_inclusive")
    if end < begin:
        raise InputError("Source interval ends before its start")
    return begin, end


def pairs(items):
    result = {}
    for key, value in items:
        if key in result:
            raise InputError("Duplicate JSON key: " + key)
        result[key] = value
    return result


def constant(value):
    raise InputError("Non-finite JSON constant: " + value)


@dataclass
class Frame:
    start: int
    end: int
    channel: int
    raw: int
    parity: bool
    invalid: bool
    preamble: str
    content: str
    epoch: int = 0


@dataclass
class Burst:
    index: int
    start: int
    end: int
    pc: int
    pd: int
    format: str
    complete: bool
    integrity: bool
    supported: bool = False
    declared_bits: int = 0
    observed_bits: int = 0
    payload: bytes = b""
    epoch: int = -1
    reasons: list = field(default_factory=list)
    status: list = field(default_factory=list)
    v_invalid_words: int = 0
    digest: str = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"

    @property
    def comparable(self):
        return self.supported and self.complete and self.integrity and not self.reasons

    def identity(self):
        # Pc metadata/status are deliberately separate from payload identity.
        return (self.pc & 127, self.pc >> 13, self.declared_bits,
                self.digest)

    def record(self):
        return {"index": self.index, "start_sample": str(self.start),
                "end_sample_inclusive": str(self.end), "Pc": self.pc, "Pd": self.pd,
                "type_code": self.pc & 127, "stream_number": self.pc >> 13,
                "type": TYPES.get(self.pc & 127, ("unsupported/null/pause",))[0],
                "producer_format": self.format, "producer_complete": self.complete,
                "producer_integrity": self.integrity, "supported_extent": self.supported,
                "declared_payload_bits": self.declared_bits,
                "observed_payload_bits": self.observed_bits,
                "stored_word_bytes": len(self.payload),
                "payload_sha256": self.digest if self.comparable else None,
                "observed_word_sha256": self.digest if self.observed_bits else None,
                "comparable": self.comparable, "exclusions": self.reasons,
                "source_epoch": self.epoch, "V_invalid_payload_words": self.v_invalid_words,
                "latest_prior_status": self.status,
                "transport_trust_promoted": False, "codec_body_validated": False}


@dataclass
class Source:
    path: str
    capture_rate: str
    origin: int
    frames: list
    events: dict
    statuses: list
    barriers: list
    summary_seen: bool
    producer_passed: object
    bursts: list = field(default_factory=list)
    discontinuities: list = field(default_factory=list)

    def record(self):
        return {"path": self.path, "capture_rate_hz": self.capture_rate,
                "origin_sample": str(self.origin), "subframes": len(self.frames),
                "summary_seen": self.summary_seen, "producer_policies_passed": self.producer_passed,
                "discontinuities": self.discontinuities,
                "observed_status_blocks": self.statuses,
                "bursts": [b.record() for b in self.bursts]}


def load(path, args, work):
    frames, events, statuses, barriers = [], {}, [], []
    metadata, summary, producer_passed = None, False, None
    size = records = 0
    with Path(path).open("rb") as stream:
        while True:
            line = stream.readline(args.max_line_bytes + 1)
            if not line:
                break
            size += len(line)
            records += 1
            work.spend()
            if len(line) > args.max_line_bytes or size > args.max_input_bytes or records > args.max_records:
                raise InputError("Input byte/line/record budget exceeded: " + str(path))
            if not line.strip():
                continue
            try:
                item = json.loads(line.decode("utf-8-sig"), object_pairs_hook=pairs,
                                  parse_constant=constant, parse_float=Decimal)
                if not isinstance(item, dict) or type(item.get("schema_version")) is not int or item["schema_version"] != 1:
                    raise InputError("Expected version-1 JSONL object")
                kind = label(item.get("event"), "event", 128)
                if summary:
                    raise InputError("Records after closing summary")
                if kind == "metadata":
                    if metadata is not None:
                        raise InputError("Expected exactly one initial metadata record")
                    metadata = (capture_rate(item["capture_rate_hz"]),
                                integer(item.get("origin_sample", "0"), "origin_sample"))
                    if not metadata[0]:
                        raise InputError("Capture rate must be positive")
                    continue
                if metadata is None:
                    raise InputError("Metadata must precede all events")
                if kind == "fatal_error":
                    raise InputError("Upstream fatal_error prevents comparison")
                if kind == "summary":
                    summary = True
                    if "passed" in item:
                        producer_passed = boolean(item["passed"], "summary.passed")
                elif kind == "subframe":
                    start, end = interval(item)
                    channel = integer(item["channel"], "channel", 1)
                    raw = integer(item["raw_slots_4_31"], "raw_slots_4_31", (1 << 28) - 1)
                    parity = boolean(item["parity_ok"], "parity_ok")
                    invalid = boolean(item["V_invalid"], "V_invalid")
                    for key in ("U", "C", "P"):
                        if key in item:
                            boolean(item[key], key)
                    preamble = label(item["preamble"], "preamble", 1)
                    if preamble not in (("B", "M") if channel == 0 else ("W",)):
                        raise InputError("Subframe preamble/channel disagreement")
                    if frames and start <= frames[-1].end:
                        raise InputError("Subframe source intervals overlap or run backwards")
                    if len(frames) >= args.max_subframes:
                        raise InputError("--max-subframes budget exceeded")
                    frames.append(Frame(start, end, channel, raw, parity, invalid,
                                        preamble, label(item["content"], "content", 128)))
                elif kind == "content":
                    if not boolean(item["burst"], "content.burst"):
                        continue
                    start, end = interval(item)
                    record = {"start": start, "end": end,
                              "pc": integer(item["Pc"], "Pc", 65535),
                              "pd": integer(item["Pd"], "Pd", 65535),
                              "complete": boolean(item["complete"], "content.complete"),
                              "integrity": boolean(item["integrity"], "content.integrity"),
                              "format": label(item["format"], "content.format", 128)}
                    if start not in events:
                        if len(events) >= args.max_bursts:
                            raise InputError("--max-bursts budget exceeded")
                        events[start] = []
                    if len(events[start]) >= 8:
                        raise InputError("Too many content confirmations for one header")
                    events[start].append(record)
                elif kind == "channel_status":
                    start, end = interval(item)
                    channel = integer(item["channel"], "status.channel", 1)
                    c = label(item["C_hex"], "C_hex", 48)
                    if len(c) != 48 or any(x not in "0123456789abcdefABCDEF" for x in c):
                        raise InputError("C_hex must contain exactly24hexadecimal bytes")
                    if len(statuses) >= args.max_status_blocks:
                        raise InputError("--max-status-blocks budget exceeded")
                    statuses.append({"start_sample": str(start), "end_sample_inclusive": str(end),
                                     "channel": channel, "C_hex": c.lower(),
                                     "integrity": boolean(item["integrity"], "status.integrity"),
                                     "supported": boolean(item["supported"], "status.supported"),
                                     "non_audio": boolean(item["non_audio"], "status.non_audio"),
                                     "status_crc": label(item.get("status_crc", "not_checked"), "status_crc", 64)})
                elif kind == "diagnostic":
                    start, end = interval(item)
                    code = label(item["code"], "diagnostic.code", 128)
                    if code in ("sync_loss", "channel_order", "block_length"):
                        if len(barriers) >= args.max_subframes:
                            raise InputError("Discontinuity budget exceeded")
                        barriers.append(start)
            except (KeyError, TypeError, ValueError, UnicodeError, RecursionError, InvalidOperation) as exc:
                raise InputError("{}:{}: {}".format(path, records, exc)) from exc
    if metadata is None:
        raise InputError("Missing metadata: " + str(path))
    source = Source(str(path), metadata[0], metadata[1], frames, events,
                    statuses, sorted(set(barriers)), summary, producer_passed)
    resolve(source, args, work)
    return source


def good_parity(frame):
    # Check the28raw bits independently of the producer's parity_ok flag.
    return frame.parity and bin(frame.raw).count("1") % 2 == 0


def resolve(source, args, work):
    frames = source.frames
    starts = [f.start for f in frames]
    epoch = 0
    for previous, current in zip(frames, frames[1:]):
        work.spend()
        diagnostic = (bisect_right(source.barriers, current.start) >
                      bisect_right(source.barriers, previous.start))
        if previous.end + 1 != current.start or previous.channel == current.channel or diagnostic:
            if len(source.discontinuities) >= args.max_bursts:
                raise InputError("Discontinuity report budget exceeds --max-bursts")
            epoch += 1
            source.discontinuities.append({"after_sample": str(previous.end),
                                           "before_sample": str(current.start),
                                           "reason": "source_gap/channel_order/synchronization_barrier"})
        current.epoch = epoch
    status_by_channel = [[], []]
    normalized_status = []
    for status in source.statuses:
        index = bisect_right(starts, int(status["end_sample_inclusive"])) - 1
        begin = bisect_left(starts, int(status["start_sample"]))
        status = dict(status)
        known = (0 <= begin <= index < len(frames) and
                 frames[begin].start == int(status["start_sample"]) and
                 frames[index].end == int(status["end_sample_inclusive"]) and
                 frames[begin].epoch == frames[index].epoch)
        status["source_epoch"] = frames[index].epoch if known else -1
        status["source_coverage_established"] = known
        normalized_status.append(status)
        status_by_channel[status["channel"]].append(status)
    source.statuses = normalized_status
    for statuses in status_by_channel:
        statuses.sort(key=lambda b: int(b["end_sample_inclusive"]))
    ends_by_channel = [[int(b["end_sample_inclusive"]) for b in blocks] for blocks in status_by_channel]
    payload_bytes = 0
    for index, (start, records) in enumerate(sorted(source.events.items())):
        work.spend()
        first, final = records[0], records[-1]
        burst = Burst(index, start, final["end"], final["pc"], final["pd"],
                      final["format"], final["complete"], final["integrity"])
        source.bursts.append(burst)
        # Duplicate identical evidence is harmless; changed Pc/Pd, contradictory
        # finalizations or backwards extents cannot replace earlier evidence.
        if any((r["pc"], r["pd"]) != (first["pc"], first["pd"]) for r in records):
            burst.reasons.append("contradictory_header_records")
        if final["integrity"] and any(not r["integrity"] for r in records):
            burst.reasons.append("earlier_integrity_failure")
        if any(b["end"] < a["end"] or a["complete"] and b != a for a, b in zip(records, records[1:])):
            burst.reasons.append("contradictory_completion_records")
        at = bisect_left(starts, start)
        header = frames[at:at + 4]
        header_ok = (len(header) == 4 and header[0].start == start and
                     [f.channel for f in header] == [0, 1, 0, 1] and
                     all(a.end + 1 == b.start and a.epoch == b.epoch for a, b in zip(header, header[1:])) and
                     [f.raw >> 8 & 65535 for f in header] == [0xf872, 0x4e1f, burst.pc, burst.pd])
        if not header_ok:
            burst.reasons.append("incomplete_or_inconsistent_header")
            continue
        burst.epoch = header[0].epoch
        if not all(good_parity(f) for f in header):
            burst.reasons.append("header_parity")
        if burst.pc & 0x80:
            burst.reasons.append("Pc_error_flag")
        spec = TYPES.get(burst.pc & 127)
        if spec is None:
            burst.reasons.append("unsupported_payload_extent")
            continue
        burst.supported = True
        burst.declared_bits = burst.pd * (8 if spec[1] == "bytes" else 1)
        if not 0 < burst.declared_bits <= spec[2]:
            burst.reasons.append("invalid_declared_payload_extent")
            continue
        header_end = header[-1].end
        if first["end"] < header_end or (not first["complete"] and first["end"] != header_end and len(records) > 1):
            burst.reasons.append("inconsistent_header_extent")
        remaining, previous, data = burst.declared_bits, header[-1], bytearray()
        for frame in frames[at + 4:at + 4 + (remaining + 15) // 16]:
            work.spend()
            if frame.end > burst.end:
                break
            if frame.start != previous.end + 1 or frame.channel == previous.channel or frame.epoch != burst.epoch:
                burst.reasons.append("payload_discontinuity")
                break
            valid = min(16, remaining)
            word = (frame.raw >> 8 & 65535) & (65535 << (16 - valid))
            data.extend((word >> 8, word & 255))
            burst.observed_bits += valid
            burst.v_invalid_words += frame.invalid
            if not good_parity(frame) and "payload_parity" not in burst.reasons:
                burst.reasons.append("payload_parity")
            remaining -= valid
            previous = frame
        burst.payload = bytes(data)
        payload_bytes += len(data)
        work.spend(len(data))
        burst.digest = hashlib.sha256(burst.payload).hexdigest()
        if payload_bytes > args.max_payload_bytes:
            raise InputError("--max-payload-bytes budget exceeded: " + source.path)
        if remaining or not burst.complete:
            burst.reasons.append("incomplete_payload")
        elif previous.end != burst.end:
            burst.reasons.append("inconsistent_completed_extent")
        if not burst.integrity:
            burst.reasons.append("producer_integrity_unconfirmed")
        for channel in (0, 1):
            i = bisect_left(ends_by_channel[channel], burst.start) - 1
            if i >= 0:
                status = status_by_channel[channel][i]
                if status["source_epoch"] == burst.epoch:
                    burst.status.append(status)
    # Header-shaped words inside a payload are legal. If the input separately
    # declares them as another candidate, neither overlapping interpretation can
    # supply an independent alignment anchor. Mark interval components in O(n).
    component, furthest = [], -1
    for burst in source.bursts:
        if component and burst.start > furthest:
            if len(component) > 1:
                for item in component:
                    item.reasons.append("overlapping_candidate_extents")
            component = []
        component.append(burst)
        furthest = max(furthest, burst.end) if len(component) > 1 else burst.end
    if len(component) > 1:
        for item in component:
            item.reasons.append("overlapping_candidate_extents")


def byte_difference(a, b, args, work):
    differences = []
    changed = changed_bits = 0
    for offset in range(max(len(a.payload), len(b.payload))):
        work.spend()
        x = a.payload[offset] if offset < len(a.payload) else None
        y = b.payload[offset] if offset < len(b.payload) else None
        if x != y:
            changed += 1
            common_bits = max(0, min(8, min(a.observed_bits, b.observed_bits) - offset * 8))
            mask = (255 << (8 - common_bits)) & 255
            if x is not None and y is not None:
                changed_bits += bin((x ^ y) & mask).count("1")
            if len(differences) < args.max_diff_bytes:
                differences.append({"byte_offset": offset, "a_hex": None if x is None else "%02x" % x,
                                    "b_hex": None if y is None else "%02x" % y})
    return {"different_stored_bytes": changed, "different_common_bits": changed_bits,
            "first_difference_byte": differences[0]["byte_offset"] if differences else None,
            "byte_differences": differences, "byte_details_truncated": changed > len(differences),
            "declared_bit_count_equal": a.declared_bits == b.declared_bits}


def status_difference(a, b):
    result = []
    for channel in (0, 1):
        x = next((s for s in a.status if s["channel"] == channel), None)
        y = next((s for s in b.status if s["channel"] == channel), None)
        if x is None or y is None:
            result.append({"channel": channel, "comparison": "unknown"})
        else:
            mask = bytes(x ^ y for x, y in zip(bytes.fromhex(x["C_hex"]), bytes.fromhex(y["C_hex"])))
            result.append({"channel": channel, "comparison": "different" if any(mask) else "equal",
                           "changed_mask_hex": mask.hex()})
    return result


def compare(a, b, args, work):
    aa, bb = a.bursts, b.bursts
    matches, unresolved, only_a, only_b = [], [], [], []

    def match(i, j, basis):
        x, y = aa[i], bb[j]
        assessed = x.comparable and y.comparable
        observed = x.supported and y.supported and x.observed_bits > 0 and y.observed_bits > 0
        difference = byte_difference(x, y, args, work) if observed else {
            "different_stored_bytes": None, "different_common_bits": None,
            "first_difference_byte": None, "byte_differences": [],
            "byte_details_truncated": False, "declared_bit_count_equal": None}
        equal = x.declared_bits == y.declared_bits and x.payload == y.payload if assessed else None
        matches.append({"a_index": i, "b_index": j, "basis": basis,
                        "payload_equal": equal, "header_equal": (x.pc, x.pd) == (y.pc, y.pd),
                        "both_comparable": assessed,
                        "byte_comparison_scope": "complete_payload" if assessed else "observed_prefix_only" if observed else "unassessed",
                        "observed_prefix_equal": x.observed_bits == y.observed_bits and x.payload == y.payload if observed else None,
                        "a_start_sample": str(x.start), "b_start_sample": str(y.start),
                        "status_comparison": status_difference(x, y), **difference})

    if args.offset_bursts is not None:
        for i in range(len(aa)):
            j = i + args.offset_bursts
            if 0 <= j < len(bb):
                match(i, j, "explicit_user_burst_offset")
            else:
                only_a.append(i)
        only_b = [j for j in range(len(bb)) if not 0 <= j - args.offset_bursts < len(aa)]
        mode = "explicit_user_burst_offset"
    else:
        counts_a = Counter(x.identity() for x in aa if x.comparable)
        counts_b = Counter(x.identity() for x in bb if x.comparable)
        positions_b = {x.identity(): x.index for x in bb if x.comparable and counts_b[x.identity()] == 1}
        anchors = []
        for x in aa:
            work.spend()
            key = x.identity()
            if x.comparable and counts_a[key] == 1 and key in positions_b:
                y = bb[positions_b[key]]
                work.spend(len(x.payload))
                if x.payload == y.payload:  # Hashes only index candidates.
                    anchors.append((x.index, y.index))
        ordered = all(x[1] < y[1] for x, y in zip(anchors, anchors[1:]))
        if not anchors or not ordered:
            unresolved.append({"reason": "no_unique_payload_anchors" if not anchors else "crossing_unique_anchors",
                               "a_indices": list(range(len(aa))), "b_indices": list(range(len(bb))),
                               "candidate_anchors": [[i, j] for i, j in anchors]})
        else:
            for i, j in anchors:
                match(i, j, "unique_ordered_exact_payload_anchor")
            boundaries = [(-1, -1)] + anchors + [(len(aa), len(bb))]
            for number, ((i, j), (k, l)) in enumerate(zip(boundaries, boundaries[1:])):
                left, right = list(range(i + 1, k)), list(range(j + 1, l))
                if not left and not right:
                    continue
                interior = number > 0 and number < len(boundaries) - 2
                continuous = interior and aa[i].epoch == aa[k].epoch and bb[j].epoch == bb[l].epoch
                usable = all(aa[n].comparable for n in left) and all(bb[n].comparable for n in right)
                if continuous and usable and len(left) == len(right) == 1 and (aa[left[0]].pc & 127, aa[left[0]].pc >> 13) == (bb[right[0]].pc & 127, bb[right[0]].pc >> 13):
                    match(left[0], right[0], "single_compatible_pair_between_unique_anchors")
                elif continuous and usable and (not left or not right):
                    only_a.extend(left)
                    only_b.extend(right)
                else:
                    unresolved.append({"reason": "capture_edge_unmatched" if not interior else "discontinuity_or_incomplete" if not continuous or not usable else "ambiguous_interior_mapping",
                                       "a_indices": left, "b_indices": right})
        mode = "unique_ordered_payload_anchors"
    matches.sort(key=lambda m: (m["a_index"], m["b_index"]))
    payload_exact = bool(matches) and all(m["payload_equal"] and m["header_equal"] and m["both_comparable"] for m in matches)
    all_covered = len(matches) == len(aa) == len(bb) and not unresolved and not only_a and not only_b
    continuous = not a.discontinuities and not b.discontinuities
    passed = payload_exact and all_covered and continuous and a.summary_seen and b.summary_seen
    status_differences = sum(any(s["comparison"] == "different" for s in m["status_comparison"]) for m in matches)
    report = {"schema_version": 1, "event": "compressed_comparison",
              "result": "exact_payload_comparison" if passed else "differences_or_uncertainty",
              "mode": mode, "explicit_offset_bursts": args.offset_bursts,
              "scope": "Received valid payload bits and raw Pc/Pd; no audio decoding or transport trust promotion",
              "clock_synchronization_performed": False,
              "status_comparison_basis": "Latest prior completed block within each source epoch; blocks/clocks are not assumed synchronized and differences do not establish causality",
              "unmatched_meaning": "Observed only on one side of a justified correspondence; not proof of transmission loss or device fault",
              "status_differences_fail_payload_comparison": False,
              "sources": {"a": a.record(), "b": b.record()}, "pairs": matches,
              "only_a_observations": only_a, "only_b_observations": only_b,
              "unresolved": unresolved,
              "totals": {"a_bursts": len(aa), "b_bursts": len(bb), "paired": len(matches),
                         "payload_equal_pairs": sum(m["payload_equal"] is True for m in matches),
                         "payload_different_pairs": sum(m["payload_equal"] is False for m in matches),
                         "payload_unassessed_pairs": sum(m["payload_equal"] is None for m in matches),
                         "pairs_with_status_differences": status_differences},
              "budgets": {name: getattr(args, name) for name in DEFAULTS},
              "work_used": work.used}
    return report, 0 if passed else 1


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("a", type=Path)
    p.add_argument("b", type=Path)
    p.add_argument("--output", type=Path, help="New JSON report file; existing files are never overwritten")
    p.add_argument("--offset-bursts", type=int, help="Explicit user correspondence B[i+N] to A[i], including partial/unsupported candidates; does not synchronize clocks")
    for name, default in DEFAULTS.items():
        p.add_argument("--" + name.replace("_", "-"), type=int, default=default)
    return p


def run(argv=None):
    args = parser().parse_args(argv)
    try:
        for name in DEFAULTS:
            if not 1 <= getattr(args, name) <= (1 << 40):
                raise InputError("Budget must be a positive bounded integer: " + name)
        if args.offset_bursts is not None and abs(args.offset_bursts) > args.max_bursts:
            raise InputError("--offset-bursts exceeds --max-bursts")
        work = Work(args.max_work)
        a, b = load(args.a, args, work), load(args.b, args, work)
        report, code = compare(a, b, args, work)
        text = json.dumps(report, indent=2, ensure_ascii=True, allow_nan=False) + "\n"
        if len(text.encode("utf-8")) > args.max_output_bytes:
            raise InputError("--max-output-bytes budget exceeded; no complete report written")
        if args.output is None:
            sys.stdout.write(text)
        else:
            with args.output.open("x", encoding="utf-8", newline="\n") as output:
                output.write(text)
        return code
    except (InputError, OSError, ValueError, RecursionError) as exc:
        print("compare_compressed: " + str(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(run())
