"""Hand-checked decoded-word vectors; no production decoder/encoder dependency."""
import contextlib
import copy
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location("compressed", ROOT / "scripts/compare_compressed.py")
M = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = M
SPEC.loader.exec_module(M)


def event(kind, **fields):
    return dict(schema_version=1, event=kind, **fields)


class Vector:
    """96MHz/48k gives exactly1000capture samples per subframe.

    Literal Pa/Pb and upper16-bit payload words are independent protocol vectors,
    not output from the C++ fixture encoder. Header/final ContentInfo callbacks
    deliberately precede the Pd/final payload subframe, like the native core.
    """
    def __init__(self, status=6, origin=0):
        self.next = origin + 1000
        self.number = 0
        self.status = status
        self.rows = [event("metadata", capture_rate_hz=96000000, origin_sample=str(origin))]
        self.c = bytes([status, 0x12, 0, 2] + [0] * 20)
        begin = self.next
        for i in range(384):
            self.word(0, c=bool(self.c[(i // 2) // 8] >> ((i // 2) % 8) & 1))
        for channel in (0, 1):
            self.rows.append(event("channel_status", start_sample=str(begin),
                                   end_sample_inclusive=str(self.next - 1), channel=channel,
                                   C_hex=self.c.hex(), integrity=True, supported=True,
                                   non_audio=bool(status & 2), status_crc="not_applicable"))

    def word(self, value, c=False, invalid=False):
        raw = value << 8 | int(c) << 26 | int(invalid) << 24
        # Even parity over all28slots, independently hand-counted by Python.
        raw |= (bin(raw).count("1") % 2) << 27
        channel = self.number % 2
        row = event("subframe", start_sample=str(self.next),
                    end_sample_inclusive=str(self.next + 999), channel=channel,
                    raw_slots_4_31=raw, parity_ok=True, V_invalid=invalid,
                    preamble="W" if channel else "B" if self.number % 384 == 0 else "M",
                    content="Non-PCM (unidentified)" if self.status & 2 else "PCM")
        self.rows.append(row)
        self.next += 1000
        self.number += 1
        return row

    def burst(self, words, pc=1, bits=None, complete=True, partial_header=None, invalid=False):
        if self.number % 2:
            self.word(0)
        start = self.next
        bits = len(words) * 16 if bits is None else bits
        pd = bits // 8 if pc & 127 == 21 else bits
        words4 = [0xf872, 0x4e1f, pc, pd]
        fmt = "PCM status / IEC 61937 header conflict" if not self.status & 2 else "Dolby Digital / AC-3" if pc & 127 == 1 else "DTS core type I"

        def content(end, done, integrity):
            return event("content", start_sample=str(start), end_sample_inclusive=str(end),
                         Pc=pc, Pd=pd, format=fmt, complete=done, integrity=integrity, burst=True)

        if partial_header is not None:
            for word in words4[:partial_header]:
                self.word(word)
            record = content(self.next - 1, False, False)
            record["Pc"] = pc if partial_header >= 3 else 0
            record["Pd"] = 0
            self.rows.append(record)
            return start
        for i, word in enumerate(words4):
            if i == 3:
                self.rows.append(content(self.next + 999, False, True))
            self.word(word)
        for i, word in enumerate(words):
            if i + 1 == len(words):
                self.rows.append(content(self.next + 999, complete, complete))
            self.word(word, invalid=invalid)
        return start

    def finished(self, summary=True):
        return self.rows + ([event("summary", passed=True)] if summary else [])


class CompareTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.a = Path(self.tmp.name) / "a.jsonl"
        self.b = Path(self.tmp.name) / "b.jsonl"

    def write(self, a, b):
        for path, rows in ((self.a, a), (self.b, b)):
            path.write_text("".join(json.dumps(x) + "\n" for x in rows), encoding="utf-8")

    def compare(self, a, b, *flags):
        self.write(a, b)
        args = M.parser().parse_args([str(self.a), str(self.b)] + list(flags))
        work = M.Work(args.max_work)
        return M.compare(M.load(self.a, args, work), M.load(self.b, args, work), args, work)

    def sequence(self, words, status=6, origin=0):
        vector = Vector(status, origin)
        for payload in words:
            vector.burst(payload)
        return vector.finished()

    def test_exact_and_native_callback_order(self):
        rows = self.sequence([[0x1234, 0xabcd], [0x3456, 0x789a]])
        result, code = self.compare(rows, rows)
        self.assertEqual(code, 0)
        self.assertEqual(result["totals"]["payload_equal_pairs"], 2)
        first = result["sources"]["a"]["bursts"][0]
        self.assertEqual((first["Pc"], first["Pd"], first["declared_payload_bits"]), (1, 32, 32))
        self.assertEqual(first["start_sample"], "385000")
        self.assertEqual(first["end_sample_inclusive"], "390999")

    def test_status_conflict_does_not_rewrite_trust(self):
        a = self.sequence([[0x1234], [0x5678]], status=6)
        b = self.sequence([[0x1234], [0x5678]], status=4)
        result, code = self.compare(a, b)
        self.assertEqual(code, 0)
        self.assertEqual(result["totals"]["pairs_with_status_differences"], 2)
        pair = result["pairs"][0]
        self.assertEqual(pair["status_comparison"][0]["changed_mask_hex"], "02" + "00" * 23)
        self.assertIn("conflict", result["sources"]["b"]["bursts"][0]["producer_format"])
        self.assertFalse(result["sources"]["b"]["bursts"][0]["transport_trust_promoted"])
        self.assertFalse(result["sources"]["b"]["bursts"][0]["codec_body_validated"])

    def test_changed_word_between_independent_anchors(self):
        a = self.sequence([[0x1111], [0x1234, 0xabcd], [0x3333]])
        b = self.sequence([[0x1111], [0x1234, 0xabce], [0x3333]])
        result, code = self.compare(a, b)
        self.assertEqual(code, 1)
        changed = result["pairs"][1]
        self.assertFalse(changed["payload_equal"])
        self.assertEqual(changed["first_difference_byte"], 3)
        self.assertEqual(changed["different_common_bits"], 2)  # CD xor CE =03.
        self.assertEqual(changed["byte_differences"], [{"byte_offset": 3, "a_hex": "cd", "b_hex": "ce"}])

    def test_unmatched_interior_observation_both_directions(self):
        a = self.sequence([[0x1111], [0x2222], [0x3333]])
        b = self.sequence([[0x1111], [0x3333]])
        for left, right, key in ((a, b, "only_a_observations"), (b, a, "only_b_observations")):
            result, code = self.compare(left, right)
            self.assertEqual(code, 1)
            self.assertEqual(result[key], [1])
            self.assertIn("not proof", result["unmatched_meaning"])
            self.assertEqual([(p["a_index"], p["b_index"]) for p in result["pairs"]], [(0, 0), (2, 1)] if left is a else [(0, 0), (1, 2)])

    def test_repeated_and_periodic_payloads_ambiguous(self):
        for sequence in ([[0x1111]] * 4, [[0x1111], [0x2222]] * 3):
            rows = self.sequence(sequence)
            result, code = self.compare(rows, rows)
            self.assertEqual(code, 1)
            self.assertFalse(result["pairs"])
            self.assertEqual(result["unresolved"][0]["reason"], "no_unique_payload_anchors")

    def test_repeated_interior_not_silently_zipped(self):
        rows = self.sequence([[1], [2], [2], [3]])
        result, code = self.compare(rows, rows)
        self.assertEqual(code, 1)
        self.assertEqual(len(result["pairs"]), 2)
        self.assertEqual(result["unresolved"][0]["a_indices"], [1, 2])

    def test_crossing_anchors_no_invented_order(self):
        result, code = self.compare(self.sequence([[1], [2], [3]]), self.sequence([[1], [3], [2]]))
        self.assertEqual(code, 1)
        self.assertEqual(result["unresolved"][0]["reason"], "crossing_unique_anchors")
        self.assertFalse(result["pairs"])

    def test_capture_edges_not_forced_changed_pairs(self):
        result, code = self.compare(self.sequence([[7], [1], [2], [8]]), self.sequence([[9], [1], [2], [10]]))
        self.assertEqual(code, 1)
        self.assertEqual(len(result["pairs"]), 2)
        self.assertTrue(all(x["reason"] == "capture_edge_unmatched" for x in result["unresolved"]))

    def test_internal_header_shaped_payload_never_scanned(self):
        rows = self.sequence([[0xf872, 0x4e1f, 1, 16, 0xabcd]])
        result, code = self.compare(rows, rows)
        self.assertEqual(code, 0)
        self.assertEqual(result["totals"]["a_bursts"], 1)

    def test_nested_candidate_metadata_cannot_double_count_payload(self):
        vector = Vector()
        start = vector.burst([0xf872, 0x4e1f, 1, 32, 0x4567, 0x5678, 0x6001, 0x6002, 0x6003, 0x6004])
        inner = event("content", start_sample=str(start + 4000),
                      end_sample_inclusive=str(start + 7999), Pc=1, Pd=32,
                      format="Dolby Digital / AC-3", complete=False, integrity=True, burst=True)
        vector.rows.extend((inner, dict(inner, end_sample_inclusive=str(start + 9999), complete=True)))
        result, code = self.compare(vector.finished(), vector.finished(), "--offset-bursts", "0")
        self.assertEqual(code, 1)
        self.assertEqual(result["totals"]["payload_unassessed_pairs"], 2)
        for burst in result["sources"]["a"]["bursts"]:
            self.assertIn("overlapping_candidate_extents", burst["exclusions"])

    def test_supported_types_and_explicit_length_units(self):
        for pc in (1, 11, 12, 13, 21):
            vector = Vector()
            vector.burst([0x0123, 0x4567], pc=pc)
            result, code = self.compare(vector.finished(), vector.finished())
            self.assertEqual(code, 0)
            burst = result["sources"]["a"]["bursts"][0]
            self.assertEqual(burst["declared_payload_bits"], 32)
            self.assertEqual(burst["Pd"], 4 if pc == 21 else 32)

    def test_partial_final_word_masks_only_unused_low_bits(self):
        a, b = Vector(), Vector()
        a.burst([0x1234, 0x8001], bits=17)
        b.burst([0x1234, 0xffff], bits=17)
        result, code = self.compare(a.finished(), b.finished())
        self.assertEqual(code, 0)
        self.assertEqual(result["sources"]["a"]["bursts"][0]["declared_payload_bits"], 17)
        b = Vector()
        b.burst([0x1234, 0xffff], bits=18)
        result, code = self.compare(a.finished(), b.finished(), "--offset-bursts", "0")
        self.assertEqual(code, 1)
        self.assertFalse(result["pairs"][0]["payload_equal"])
        self.assertEqual(result["pairs"][0]["different_common_bits"], 0)
        # Same masked bytes cannot erase a difference in declared bit counts.
        a, b = Vector(), Vector()
        a.burst([0x1234, 0x0000], bits=17)
        b.burst([0x1234, 0x0000], bits=18)
        result, code = self.compare(a.finished(), b.finished(), "--offset-bursts", "0")
        self.assertEqual(code, 1)
        self.assertFalse(result["pairs"][0]["payload_equal"])
        self.assertEqual(result["pairs"][0]["different_stored_bytes"], 0)

    def test_v_invalid_compressed_words_retained(self):
        vector = Vector()
        vector.burst([0x1234, 0x5678], invalid=True)
        result, code = self.compare(vector.finished(), vector.finished())
        self.assertEqual(code, 0)
        self.assertEqual(result["sources"]["a"]["bursts"][0]["V_invalid_payload_words"], 2)

    def test_same_bytes_different_pc_metadata_reported(self):
        a, b = Vector(), Vector()
        a.burst([0x1234], pc=1)
        b.burst([0x1234], pc=0x101)  # AC3 bsmod metadata, not bit7error.
        result, code = self.compare(a.finished(), b.finished())
        self.assertEqual(code, 1)
        self.assertTrue(result["pairs"][0]["payload_equal"])
        self.assertFalse(result["pairs"][0]["header_equal"])

    def test_partial_and_unsupported_never_equal_empty_payloads(self):
        for options in ({"pc": 0x33}, {"complete": False, "bits": 64}, {"partial_header": 3}, {"pc": 0}, {"pc": 3}):
            vector = Vector()
            vector.burst([0x1234], **options)
            result, code = self.compare(vector.finished(), vector.finished(), "--offset-bursts", "0")
            self.assertEqual(code, 1)
            self.assertIsNone(result["pairs"][0]["payload_equal"])
            self.assertEqual(result["totals"]["payload_unassessed_pairs"], 1)
            self.assertEqual(result["totals"]["payload_equal_pairs"], 0)

    def test_parity_error_and_pc_error_never_anchor(self):
        for pc, corrupt in ((1, True), (0x81, False)):
            vector = Vector()
            start = vector.burst([0x1234], pc=pc)
            if corrupt:
                row = next(x for x in vector.rows if x["event"] == "subframe" and int(x["start_sample"]) == start + 4000)
                row["raw_slots_4_31"] ^= 1 << 27
                row["parity_ok"] = False
            result, code = self.compare(vector.finished(), vector.finished())
            self.assertEqual(code, 1)
            self.assertFalse(result["pairs"])

    def test_explicit_mapping_keeps_partial_candidate_slot(self):
        vector = Vector()
        vector.burst([1])
        vector.burst([2], bits=64, complete=False)
        vector.burst([3])
        result, code = self.compare(vector.finished(), vector.finished(), "--offset-bursts", "0")
        self.assertEqual(code, 1)
        self.assertEqual([(x["a_index"], x["b_index"]) for x in result["pairs"]], [(0, 0), (1, 1), (2, 2)])
        self.assertIsNone(result["pairs"][1]["payload_equal"])

    def test_gap_blocks_missing_inference_and_preserves_coordinates(self):
        a = self.sequence([[1], [2], [3]])
        b = self.sequence([[1], [3]])
        # Insert200ms before B's second burst, including its content metadata.
        second = sorted({int(x["start_sample"]) for x in b if x["event"] == "content"})[1]
        for x in b:
            if "start_sample" in x and int(x["start_sample"]) >= second:
                x["start_sample"] = str(int(x["start_sample"]) + 19200000)
                x["end_sample_inclusive"] = str(int(x["end_sample_inclusive"]) + 19200000)
        result, code = self.compare(a, b)
        self.assertEqual(code, 1)
        self.assertFalse(result["only_a_observations"])
        self.assertEqual(result["unresolved"][0]["reason"], "discontinuity_or_incomplete")
        self.assertTrue(result["sources"]["b"]["discontinuities"])

    def test_payload_gap_does_not_join_words_on_either_side(self):
        rows = self.sequence([[0x1234, 0x5678, 0x9abc]])
        payload_last = max(int(x["start_sample"]) for x in rows if x["event"] == "subframe")
        for x in rows:
            if x.get("event") == "subframe" and int(x["start_sample"]) == payload_last:
                x["start_sample"] = str(int(x["start_sample"]) + 19200000)
                x["end_sample_inclusive"] = str(int(x["end_sample_inclusive"]) + 19200000)
            elif x.get("event") == "content" and x["complete"]:
                x["end_sample_inclusive"] = str(int(x["end_sample_inclusive"]) + 19200000)
        result, code = self.compare(rows, rows, "--offset-bursts", "0")
        self.assertEqual(code, 1)
        burst = result["sources"]["a"]["bursts"][0]
        self.assertEqual(burst["observed_payload_bits"], 32)
        self.assertIn("payload_discontinuity", burst["exclusions"])
        self.assertIsNone(result["pairs"][0]["payload_equal"])

    def test_status_block_crossing_gap_is_not_attached(self):
        a = self.sequence([[0x1234]])
        # Delete20ms from inside the supposedly completed status block.
        a = [x for x in a if x["event"] != "subframe" or not 101000 <= int(x["start_sample"]) < 121000]
        result, code = self.compare(a, a, "--offset-bursts", "0")
        self.assertEqual(code, 1)
        burst = result["sources"]["a"]["bursts"][0]
        self.assertFalse(burst["latest_prior_status"])
        self.assertTrue(all(not x["source_coverage_established"] for x in result["sources"]["a"]["observed_status_blocks"]))

    def test_large_exact_indices_no_clock_sync(self):
        base = (1 << 60) + 17
        a = self.sequence([[0x1234]], origin=base)
        b = self.sequence([[0x1234]], origin=base + 12345)
        result, code = self.compare(a, b)
        self.assertEqual(code, 0)
        self.assertEqual(result["pairs"][0]["a_start_sample"], str(base + 385000))
        self.assertEqual(result["pairs"][0]["b_start_sample"], str(base + 397345))
        self.assertFalse(result["clock_synchronization_performed"])

    def test_contradictory_finalization_does_not_launder_evidence(self):
        rows = self.sequence([[0x1234]])
        completion = next(x for x in rows if x["event"] == "content" and x["complete"])
        duplicate = dict(completion, complete=False, integrity=False)
        rows.insert(-1, duplicate)
        rows.insert(-1, copy.deepcopy(completion))
        result, code = self.compare(rows, rows, "--offset-bursts", "0")
        self.assertEqual(code, 1)
        self.assertIsNone(result["pairs"][0]["payload_equal"])

    def test_legal_duplicate_and_label_events_do_not_duplicate_burst(self):
        rows = self.sequence([[0x1234]])
        completion = next(x for x in rows if x["event"] == "content" and x["complete"])
        rows.insert(-1, copy.deepcopy(completion))
        rows.insert(-1, dict(completion, burst=False))
        result, code = self.compare(rows, rows)
        self.assertEqual(code, 0)
        self.assertEqual(result["totals"]["a_bursts"], 1)

    def test_no_summary_is_incomplete_not_fabricated_eof(self):
        rows = self.sequence([[0x1234]])[:-1]
        result, code = self.compare(rows, rows)
        self.assertEqual(code, 1)
        self.assertFalse(result["sources"]["a"]["summary_seen"])

    def test_producer_policy_failure_and_decimal_capture_rate_preserved(self):
        rows = self.sequence([[0x1234]])
        rows[0]["capture_rate_hz"] = 96000000.0
        rows[-1]["passed"] = False  # E.g. configured expected-format policy on D7.
        result, code = self.compare(rows, rows)
        self.assertEqual(code, 0)
        self.assertFalse(result["sources"]["a"]["producer_policies_passed"])
        rows[0]["capture_rate_hz"] = 96000000.5
        result, code = self.compare(rows, rows)
        self.assertEqual(code, 0)
        self.assertEqual(result["sources"]["a"]["capture_rate_hz"], "96000000.5")

    def test_diff_details_cap_preserves_exact_totals(self):
        a = self.sequence([[1], [0x0000, 0x0000], [3]])
        b = self.sequence([[1], [0xffff, 0xffff], [3]])
        result, code = self.compare(a, b, "--max-diff-bytes", "1")
        self.assertEqual(code, 1)
        difference = result["pairs"][1]
        self.assertEqual(difference["different_stored_bytes"], 4)
        self.assertEqual(difference["different_common_bits"], 32)
        self.assertEqual(len(difference["byte_differences"]), 1)
        self.assertTrue(difference["byte_details_truncated"])

    def test_input_budgets_and_strict_source_types(self):
        valid = self.sequence([[0x1234]])
        for flag, count in (("--max-input-bytes", "10"), ("--max-line-bytes", "10"), ("--max-records", "2"), ("--max-subframes", "2"), ("--max-payload-bytes", "1"), ("--max-work", "10"), ("--max-status-blocks", "1")):
            with self.subTest(flag=flag), self.assertRaises(M.InputError):
                self.compare(valid, valid, flag, count)
        for key, bad in (("start_sample", "18446744073709551616"), ("start_sample", "-1"), ("start_sample", True), ("parity_ok", "true"), ("V_invalid", 0), ("channel", True)):
            damaged = copy.deepcopy(valid)
            row = next(x for x in damaged if x["event"] == "subframe")
            row[key] = bad
            with self.subTest(key=key, bad=bad), self.assertRaises(M.InputError):
                self.compare(damaged, valid)

    def test_malformed_json_and_duplicate_keys(self):
        valid = self.sequence([[0x1234]])
        self.write(valid, valid)
        args = M.parser().parse_args([str(self.a), str(self.b)])
        for text in ('{"schema_version":1,"event":"metadata","capture_rate_hz":NaN}\n',
                     '{"schema_version":1,"event":"metadata","capture_rate_hz":Infinity}\n',
                     '{"schema_version":1,"schema_version":1,"event":"metadata","capture_rate_hz":96000000}\n',
                     '{"schema_version":1,\n'):
            self.a.write_text(text, encoding="utf-8")
            with self.assertRaises(M.InputError):
                M.load(self.a, args, M.Work(args.max_work))

    def test_cli_output_budget_no_file_and_no_overwrite(self):
        rows = self.sequence([[0x1234]])
        self.write(rows, rows)
        output = Path(self.tmp.name) / "report.json"
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(M.run([str(self.a), str(self.b), "--output", str(output), "--max-output-bytes", "10"]), 2)
        self.assertFalse(output.exists())
        self.assertEqual(M.run([str(self.a), str(self.b), "--output", str(output)]), 0)
        original = output.read_bytes()
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(M.run([str(self.a), str(self.b), "--output", str(output)]), 2)
        self.assertEqual(output.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
