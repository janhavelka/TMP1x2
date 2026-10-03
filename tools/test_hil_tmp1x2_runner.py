#!/usr/bin/env python3
"""HIL host parser/cleanup regressions; optionally use the real native CLI model."""
from __future__ import annotations

import contextlib
import io
import json
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import types
import unittest
from unittest import mock

import hil_tmp1x2_runner as hil

CLI_FIXTURE = None
if "--cli-fixture" in sys.argv:
    index = sys.argv.index("--cli-fixture")
    CLI_FIXTURE = sys.argv[index + 1]
    del sys.argv[index:index + 2]

SETTINGS = ("Desired: model=TMP102 address=0x48 timeout=50 ms mode=continuous rate=4 Hz extended=no\n"
            "Thresholds: low=75.0000 C high=80.0000 C alert=comparator polarity=low faults=1 offline-threshold=3\n"
            "Address range: 0x48..0x4B; physical ALERT=available pin=-1\n> ")
HEALTH = ("Health: state=READY online=yes consec=0 ok=20 fail=0\n"
          "Trust: bound=yes initialized=yes dirty=no last-ok=100 ms last-error=0 ms OK detail=0\n"
          "Conversion: pending=no ready=no\n> ")
SAMPLE = "Temperature: 25.0000 C raw=0x1900 counts=400 extended=no timestamp=200 ms clock=known one-shot=unproven\n> "


def operation(token=5, status="OK", kind="CONFIGURE"):
    return (f"Operation started: token={token} kind={kind}; job and cancel remain available.\n"
            "[I] IN_PROGRESS detail=0\n> "
            f"Operation result: token={token} kind={kind} elapsed=23 ms write-attempted=yes\n"
            f"[{'I' if status == 'OK' else 'E'}] {status} detail=0\n> ")


class Clock:
    def __init__(self):
        self.now = 0.0

    def __call__(self):
        self.now += 0.01
        return self.now


class FakeSerial:
    def __init__(self, *chunks, write_result=None):
        self.chunks = list(chunks)
        self.written = b""
        self.write_result = write_result

    @property
    def in_waiting(self):
        return len(self.chunks[0]) if self.chunks else 0

    def write(self, data):
        self.written += data
        return self.write_result if self.write_result is not None else len(data)

    def read(self, size):
        return self.chunks.pop(0) if self.chunks else b""


class ProcessSerial:
    """Bounded byte transport around the actual CLI executable, not hardware."""
    def __init__(self, executable):
        self.process = subprocess.Popen([executable, "--hil-fixture"], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
        self.bytes = queue.Queue()
        self.thread = threading.Thread(target=self._reader, daemon=True)
        self.thread.start()

    def _reader(self):
        while True:
            value = self.process.stdout.read(1)
            if not value:
                return
            self.bytes.put(value)

    @property
    def in_waiting(self):
        return self.bytes.qsize()

    def read(self, size):
        try:
            values = [self.bytes.get(timeout=0.05)]
        except queue.Empty:
            return b""
        for _ in range(size - 1):
            try:
                values.append(self.bytes.get_nowait())
            except queue.Empty:
                break
        return b"".join(values)

    def write(self, data):
        result = self.process.stdin.write(data)
        self.process.stdin.flush()
        return result

    def close(self):
        self.process.stdin.close()
        self.process.wait(timeout=5)
        self.thread.join(timeout=5)
        self.process.stdout.close()
        self.process.stderr.close()


class ParserTests(unittest.TestCase):
    def test_unrelated_ok_cannot_certify_matching_operation_status(self):
        stale = "Operation result: token=4 kind=CONFIGURE elapsed=0 ms write-attempted=yes\n[I] OK detail=0\n"
        output = stale + operation().replace("[I] OK detail=0", "[I] IN_PROGRESS detail=0")
        self.assertFalse(hil.response_complete("extended 1", output))
        self.assertEqual("INCONCLUSIVE", hil.classify("extended 1", output)[0])

    def test_stop_rejects_unverified_full_test_restoration(self):
        output = ("Configuration test cancelled; scheduling restoration of the captured profile.\n"
                  "Diagnostic summary: cancelled pass=0 fail=0 skip=0\n"
                  "Baseline restoration: not verified; run begin to retry the captured profile\n> ")
        self.assertEqual("FAIL", hil.classify("stop", output)[0])
        output = "Diagnostic summary: cancelled pass=0 fail=1 skip=0\n> "
        self.assertEqual("FAIL", hil.classify("stop", output)[0])

    def test_expected_fault_must_be_complete_and_only_error(self):
        result = hil.Result("read", "read", "FAIL", "I2C_TIMEOUT", "[E] I2C_TIMEOUT detail=0\n> ")
        self.assertTrue(hil.expected_error(result, hil.TRANSPORT_ERRORS))
        result.output = "[E] I2C_TIMEOUT detail=0\n[E] CONFIG_MISMATCH detail=0\n> "
        self.assertFalse(hil.expected_error(result, hil.TRANSPORT_ERRORS))
        result.output = "[E] I2C_TIMEOUT detail=0"
        self.assertFalse(hil.expected_error(result, hil.TRANSPORT_ERRORS))

    def test_boot_output_limit_preserves_partial_live_evidence(self):
        stream = io.BytesIO()
        session = hil.SerialSession(FakeSerial(b"first\n", b"overflow"), 1, 1, Clock(), stream)
        with mock.patch.object(hil, "MAX_RESPONSE_BYTES", 8), self.assertRaises(OSError):
            session.capture_boot(1)
        self.assertIn(b"first\n", stream.getvalue())
        self.assertIn("first\n", session.transcript[0])

    def test_response_limit_preserves_partial_output_without_accepting_trailing_ok(self):
        stream = io.BytesIO()
        session = hil.SerialSession(FakeSerial(b"prefix", b"[I] OK detail=0\n> "), 1, 1, Clock(), stream)
        with mock.patch.object(hil, "MAX_RESPONSE_BYTES", 8):
            result = session.command("verify")
        self.assertEqual("INCONCLUSIVE", result.outcome)
        self.assertEqual("prefix", result.output)
        self.assertIn(b"[I] OK detail=0", stream.getvalue())

    def test_interrupted_command_retains_partial_output(self):
        serial = FakeSerial(b"[I] IN_PROGRESS detail=0\n")
        original = serial.read
        def interrupt(size):
            if serial.chunks:
                return original(size)
            raise KeyboardInterrupt()
        serial.read = interrupt
        result = hil.SerialSession(serial, 1, 1, Clock()).command("extended 1")
        self.assertEqual("INCONCLUSIVE", result.outcome)
        self.assertIn("IN_PROGRESS", result.output)
        self.assertEqual("operator interrupted", result.reason)

    def test_deadline_bounds_underlying_read_and_write_timeout(self):
        serial = FakeSerial(b"[I] OK detail=0\n> ")
        serial.timeout, serial.write_timeout = 5.0, 5.0
        session = hil.SerialSession(serial, 5, 120, Clock())
        result = session.command("verify", timeout_s=0.04)
        self.assertEqual("PASS", result.outcome)
        self.assertLessEqual(serial.timeout, 0.04)
        self.assertLessEqual(serial.write_timeout, 0.04)
        prior = serial.written
        self.assertEqual("INCONCLUSIVE", session.command("verify", timeout_s=0).outcome)
        self.assertEqual(prior, serial.written)

    def test_unfinished_stream_is_not_reparsed_for_every_chunk(self):
        serial = FakeSerial(*([b"noise\n"] * 20), b"[I] OK detail=0\n> ")
        with mock.patch.object(hil, "response_complete", wraps=hil.response_complete) as complete:
            result = hil.SerialSession(serial, 5, 5, Clock()).command("verify")
        self.assertEqual("PASS", result.outcome)
        self.assertEqual(1, complete.call_count)

    def test_memory_retention_keeps_latest_response_and_full_live_transcript(self):
        stream = io.BytesIO()
        serial = FakeSerial(*([b"[I] OK detail=0\n> "] * 5))
        session = hil.SerialSession(serial, 5, 5, Clock(), stream)
        runner = hil.Runner(session, hil.parse_args(["--dry-run"]))
        with mock.patch.object(hil, "MAX_RETAINED_CHARS", 50), contextlib.redirect_stdout(io.StringIO()):
            for _ in range(5):
                runner.command("verify")
        self.assertLessEqual(session.retained_chars, 50)
        self.assertLessEqual(runner.retained_chars, 50)
        self.assertEqual(5, stream.getvalue().count(b"[I] OK detail=0"))
        self.assertTrue(runner.results[-1].output)
        self.assertEqual("", runner.results[0].output)

    def test_result_limit_stops_suites_without_blocking_cleanup(self):
        session = hil.SerialSession(FakeSerial(), 1, 1)
        runner = hil.Runner(session, hil.parse_args(["--dry-run"]))
        with mock.patch.object(hil, "MAX_RESULTS", 1), contextlib.redirect_stdout(io.StringIO()):
            runner.record(hil.Result("first", "", "PASS", "ok"))
            with self.assertRaises(hil.CheckError):
                runner.record(hil.Result("second", "", "PASS", "ok"))
            runner.cleanup_active = True
            runner.record(hil.Result("cleanup", "", "PASS", "ok"))
        self.assertEqual("INCONCLUSIVE", runner.results[1].outcome)
        self.assertEqual("cleanup", runner.results[-1].check)

    def test_malformed_threshold_baseline_is_rejected(self):
        for value in ("200.0000", "9" * 400 + ".0000"):
            with self.assertRaises(hil.CheckError):
                hil.parse_settings(SETTINGS.replace("75.0000", value))

    def test_transcript_failure_marks_run_inconclusive_but_allows_cleanup(self):
        stream = mock.Mock()
        stream.write.side_effect = OSError("disk full")
        session = hil.SerialSession(FakeSerial(b"[I] OK detail=0\n> ", b"[I] OK detail=0\n> "), 1, 1, Clock(), stream)
        runner = hil.Runner(session, hil.parse_args(["--dry-run"]))
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(hil.CheckError):
                runner.command("verify")
            runner.cleanup_active = True
            self.assertEqual("PASS", runner.command("verify").outcome)
        self.assertEqual("INCONCLUSIVE", runner.results[0].outcome)

    def test_main_setup_failures_create_json_and_transcript(self):
        cases = {
            "missing pyserial": None,
            "constructor": types.SimpleNamespace(Serial=mock.Mock(side_effect=ValueError("unsupported baud"))),
            "open": types.SimpleNamespace(Serial=mock.Mock(return_value=mock.Mock(open=mock.Mock(side_effect=OSError("port unavailable"))))),
        }
        for name, serial in cases.items():
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "report.json"
                args = ["--port", "fake", "--model", "tmp102", "--address", "0x48", "--report", str(path)]
                with mock.patch.dict(sys.modules, {"serial": serial}), contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(2, hil.main(args))
                report = json.loads(path.read_text())
                self.assertEqual("INCONCLUSIVE", report["outcome"])
                self.assertFalse(report["hardware_run"])
                self.assertEqual("serial setup", report["results"][0]["check"])
                self.assertTrue(path.with_suffix(".txt").exists())
                self.assertFalse(path.with_suffix(".json.lock").exists())

    def test_serial_close_failure_is_recorded_instead_of_losing_report(self):
        connection = FakeSerial()
        connection.open = mock.Mock()
        connection.close = mock.Mock(side_effect=OSError("close failed"))
        serial = types.SimpleNamespace(Serial=mock.Mock(return_value=connection))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "report.json"
            with mock.patch.dict(sys.modules, {"serial": serial}), mock.patch.object(hil.SerialSession, "capture_boot"), mock.patch.object(hil.Runner, "run"), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(2, hil.main(["--port", "fake", "--model", "tmp102", "--address", "0x48", "--report", str(path)]))
            report = json.loads(path.read_text())
            self.assertEqual("serial close", report["results"][0]["check"])

    def test_transcript_close_failure_has_inconclusive_json_and_exit(self):
        class CloseFails:
            def __init__(self, stream):
                self.stream = stream
            def write(self, data):
                return self.stream.write(data)
            def flush(self):
                self.stream.flush()
            def close(self):
                self.stream.close()
                raise OSError("delayed filesystem failure")
        connection = FakeSerial()
        connection.open, connection.close = mock.Mock(), mock.Mock()
        serial = types.SimpleNamespace(Serial=mock.Mock(return_value=connection))
        original = hil.EvidenceFiles
        def reserve(path, args):
            files = original(path, args)
            files.transcript = CloseFails(files.transcript)
            return files
        def passed(runner):
            runner.record(hil.Result("test check", "", "PASS", "ok"))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "report.json"
            with mock.patch.dict(sys.modules, {"serial": serial}), mock.patch.object(hil, "EvidenceFiles", side_effect=reserve), mock.patch.object(hil.SerialSession, "capture_boot"), mock.patch.object(hil.Runner, "run", passed), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(2, hil.main(["--port", "fake", "--model", "tmp102", "--address", "0x48", "--report", str(path)]))
            report = json.loads(path.read_text())
            self.assertEqual("INCONCLUSIVE", report["outcome"])
            self.assertEqual("transcript close", report["results"][-1]["check"])

    def test_evidence_reservation_prevents_overwrite_and_concurrent_run(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "report.json"
            files = hil.EvidenceFiles(path, hil.parse_args(["--dry-run"]))
            original = path.read_bytes()
            self.assertIsNone(json.loads(original)["hardware_run"])
            try:
                with self.assertRaises(FileExistsError):
                    hil.EvidenceFiles(path, hil.parse_args(["--dry-run"]))
                self.assertEqual(original, path.read_bytes())
                self.assertTrue(path.with_suffix(".json.lock").exists())
            finally:
                files.close()
            with self.assertRaises(FileExistsError):
                hil.EvidenceFiles(path, hil.parse_args(["--dry-run"]))
            self.assertEqual(original, path.read_bytes())

    def test_bad_evidence_path_never_opens_serial(self):
        serial = types.SimpleNamespace(Serial=mock.Mock())
        with tempfile.TemporaryDirectory() as directory:
            parent = Path(directory) / "not-a-directory"
            parent.write_text("preserve")
            with mock.patch.dict(sys.modules, {"serial": serial}), contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(2, hil.main(["--port", "fake", "--model", "tmp102", "--address", "0x48", "--report", str(parent / "report.json")]))
            serial.Serial.assert_not_called()
            self.assertEqual("preserve", parent.read_text())

    def test_json_finalization_failure_preserves_initial_report_and_transcript(self):
        runner = hil.Runner(hil.SerialSession(FakeSerial(), 1, 1), hil.parse_args(["--dry-run"]))
        runner.results = [hil.Result("some check", "", "PASS", "ok")]
        runner.session.transcript = ["captured bytes"]
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            path = Path(directory) / "report.json"
            with mock.patch.object(hil.os, "replace", side_effect=OSError("disk full")):
                self.assertEqual(2, hil.write_report(path, runner))
            self.assertEqual("INCONCLUSIVE", json.loads(path.read_text())["outcome"])
            self.assertIn("captured bytes", path.with_suffix(".txt").read_text())
            self.assertEqual([], list(Path(directory).glob("*.tmp")))

    def test_start_prompt_and_wrong_token_cannot_finish_operation(self):
        output = operation()
        start = output.split("Operation result:")[0]
        self.assertFalse(hil.response_complete("extended 1", start))
        self.assertFalse(hil.response_complete("extended 1", output.replace("result: token=5", "result: token=4")))
        self.assertTrue(hil.response_complete("extended 1", output))
        self.assertEqual("PASS", hil.classify("extended 1", output)[0])

    def test_chunked_async_completion_keeps_waiting(self):
        output = operation()
        initial, final = output.split("Operation result:")
        serial = FakeSerial(initial.encode(), b"", b"Operation result:" + final.encode())
        session = hil.SerialSession(serial, 1, 5, Clock())
        self.assertEqual("PASS", session.command("extended 1").outcome)
        self.assertEqual(b"extended 1\n", serial.written)
        self.assertEqual([], serial.chunks)

    def test_split_ansi_and_crlf(self):
        output = "\x1b[32m[I]\x1b[0m OK detail=0\r\n> "
        serial = FakeSerial(output[:3].encode(), output[3:9].encode(), output[9:].encode())
        self.assertEqual("PASS", hil.SerialSession(serial, 1, 5, Clock()).command("verify").outcome)

    def test_late_duplicate_prompt_is_not_the_next_command_response(self):
        serial = FakeSerial(b"> ", b"Operation: active=no token=5 kind=NONE status=OK\n> ")
        self.assertEqual("PASS", hil.SerialSession(serial, 1, 5, Clock()).command("job").outcome)
        self.assertEqual([], serial.chunks)

    def test_missing_response_and_partial_writes_are_inconclusive(self):
        for serial in (FakeSerial(), FakeSerial(b"> "), FakeSerial(write_result=2)):
            with self.subTest(serial=serial):
                self.assertEqual("INCONCLUSIVE", hil.SerialSession(serial, 0.1, 1, Clock()).command("probe").outcome)

    def test_terminal_failures_and_cancelled_diagnostics(self):
        self.assertEqual("FAIL", hil.classify("extended 1", operation(status="CONFIG_MISMATCH"))[0])
        self.assertEqual("FAIL", hil.classify("probe", "[E] I2C_NACK_ADDR detail=5\n> ")[0])
        self.assertEqual("INCONCLUSIVE", hil.classify("selfcheck", "Diagnostic summary: cancelled pass=1 fail=0 skip=0\n> ")[0])

    def test_full_test_requires_all_cases_and_verified_restoration(self):
        good = "Diagnostic summary: complete pass=19 fail=0 skip=0\nBaseline restoration: verified\n> "
        self.assertEqual("PASS", hil.classify("selftest full", good)[0])
        self.assertEqual("INCONCLUSIVE", hil.classify("selftest full", good.replace("pass=19", "pass=18"))[0])
        self.assertEqual("FAIL", hil.classify("selftest full", good.replace("restoration: verified", "restoration: not verified"))[0])
        self.assertEqual("FAIL", hil.classify("selftest full", good.replace("pass=19 fail=0", "pass=18 fail=1"))[0])

    def test_selfcheck_only_allows_optional_gpio_skip(self):
        good = "Diagnostic summary: complete pass=8 fail=0 skip=1\n> "
        self.assertEqual("PASS", hil.classify("selfcheck", good)[0])
        self.assertEqual("INCONCLUSIVE", hil.classify("selfcheck", good.replace("pass=8", "pass=7"))[0])
        self.assertEqual("INCONCLUSIVE", hil.classify("selfcheck", good.replace("pass=8", "pass=7").replace("skip=1", "skip=2"))[0])

    def test_snapshot_requires_independent_trust_flags(self):
        good = "Snapshot: config-match=yes thresholds-match=yes temperature-trusted=yes\nDiagnostic summary: complete pass=1 fail=0 skip=0\n> "
        self.assertEqual("PASS", hil.classify("snapshot read", good)[0])
        self.assertEqual("FAIL", hil.classify("snapshot read", good.replace("temperature-trusted=yes", "temperature-trusted=no"))[0])

    def test_stress_requires_exact_count_and_all_checks(self):
        good = ("Run: stopped target=3 completed=3 ok=12 fail=0 elapsed=200 ms remaining=0\n"
                "Mixed checks: completed=12 samples=3 (four checks per complete cycle)\n"
                "Tracked health delta: ok=12 fail=0\nAdapter delta: attempts=15 ok=15 fail=0\n> ")
        self.assertEqual("PASS", hil.classify("stress_mix 3", good)[0])
        for changed in (good.replace("completed=3 ok", "completed=2 ok"),
                        good.replace("ok=12 fail=0 elapsed", "ok=11 fail=1 elapsed"),
                        good.replace("samples=3", "samples=2"), good.replace("ok=15 fail=0", "ok=14 fail=1")):
            self.assertNotEqual("PASS", hil.classify("stress_mix 3", changed)[0])
        self.assertEqual("INCONCLUSIVE", hil.classify("stress_mix 3", good.replace("\n> ", "\nCounter saturation reached; deltas are lower bounds.\n> "))[0])
        self.assertEqual("INCONCLUSIVE", hil.classify("stress_mix 3", good.replace("Adapter delta:", "Missing adapter:"))[0])

    def test_historical_health_errors_do_not_fail_fresh_checks(self):
        output = HEALTH.replace("fail=0", "fail=4").replace("ms OK detail", "ms I2C_TIMEOUT detail")
        self.assertEqual("PASS", hil.classify("health", output)[0])
        self.assertEqual(4, hil.parse_health(output)["fail"])

    def test_profiles_and_extended_only_restore_order(self):
        profile = hil.parse_settings(SETTINGS)
        self.assertEqual(0x48, profile["address"])
        self.assertEqual("4", profile["rate"])
        profile.update(extended=True, low=-200.0, high=140.0)
        commands = hil.restore_commands(profile)
        self.assertLess(commands.index("extended 1"), commands.index("threshold -200.0000 140.0000"))
        self.assertEqual("recover", commands[0])
        self.assertEqual("verify", commands[-1])
        for forbidden in ("init", "busreset", "reset all", "ara", "scan", "end"):
            self.assertNotIn(forbidden, commands)

    def test_samples_positive_negative_normal_extended_and_provenance(self):
        self.assertEqual("PASS", hil.check_sample(SAMPLE, -40, 50)[0])
        negative = SAMPLE.replace("25.0000", "-10.0000").replace("0x1900", "0xF600").replace("counts=400", "counts=-160")
        self.assertEqual("PASS", hil.check_sample(negative, -40, 50)[0])
        extended = negative.replace("0xF600", "0xFB01").replace("extended=no", "extended=yes").replace("unproven", "confirmed")
        self.assertEqual("PASS", hil.check_sample(extended, -40, 50, True, True)[0])
        for changed in (extended.replace("0xFB01", "0xFB03"), extended.replace("counts=-160", "counts=-159"),
                        extended.replace("-10.0000", "-11.0000"), extended.replace("confirmed", "unproven"),
                        extended.replace("clock=known", "clock=unknown")):
            self.assertEqual("FAIL", hil.check_sample(changed, -40, 50, True, True)[0])
        self.assertEqual("FAIL", hil.check_sample(SAMPLE, -40, 20)[0])

    def test_coherent_but_stale_format_decode_is_rejected_by_continuity(self):
        coherent_wrong = SAMPLE.replace("25.0000", "50.0000").replace("0x1900", "0x1901").replace("counts=400", "counts=800").replace("extended=no", "extended=yes").replace("unproven", "confirmed")
        self.assertEqual("PASS", hil.check_sample(coherent_wrong, -40, 125, True, True)[0])
        self.assertEqual("FAIL", hil.check_format_continuity([25.0, 50.0, 25.0], 1.0)[0])
        self.assertEqual("FAIL", hil.check_format_continuity([25.0, 12.5, 25.0], 1.0)[0])
        self.assertEqual("PASS", hil.check_format_continuity([25.0, 25.0625, 25.0], 1.0)[0])

    def test_comparator_checks_raw_config_all_polarities_queues_and_states(self):
        for polarity in ("low", "high"):
            for faults, code in ((1, 0), (2, 1), (4, 2), (6, 3)):
                for active in (False, True):
                    word = 0x60D0 | (code << 11) | (0x0400 if polarity == "high" else 0) | (0x0020 if active == (polarity == "high") else 0)
                    output = f"CONFIG=0x{word:04X} valid=yes OS=0 EM=1 AL={int(bool(word & 32))} ALERT-active={'yes' if active else 'no'} mode=CONTINUOUS rate=8 Hz\n> "
                    with self.subTest(polarity=polarity, faults=faults, active=active):
                        self.assertEqual("PASS", hil.check_comparator(output, polarity, faults, active)[0])
                        self.assertEqual("FAIL", hil.check_comparator(output, polarity, faults, not active)[0])

    def test_stop_waits_for_first_cancellation_restoration(self):
        output = "Configuration test cancelled; scheduling restoration of the captured profile.\n> "
        self.assertFalse(hil.response_complete("stop", output))
        output += "Diagnostic summary: cancelled pass=1 fail=0 skip=0\nBaseline restoration: verified\n> "
        self.assertTrue(hil.response_complete("stop", output))

    def test_cleanup_accepts_expected_cancelled_but_not_other_error(self):
        output = "[I] OK detail=0\nOperation result: token=5 kind=CONFIGURE elapsed=0 ms write-attempted=yes\n[E] CANCELLED detail=0\n> > "
        self.assertTrue(hil.response_complete("stop", output))
        self.assertEqual("PASS", hil.classify("stop", output)[0])
        self.assertEqual("FAIL", hil.classify("stop", output.replace("CANCELLED", "I2C_ERROR"))[0])

    def test_disconnect_does_not_promote_incomplete_transport_error(self):
        session = mock.Mock()
        session.command.return_value = hil.Result("read", "read", "INCONCLUSIVE", "missing terminal prompt", "[E] I2C_TIMEOUT detail=0")
        runner = hil.Runner(session, hil.parse_args(["--dry-run"]))
        runner.baseline = hil.parse_settings(SETTINGS)
        runner.healthy = mock.Mock(return_value=hil.parse_health(HEALTH))
        runner.command = mock.Mock()
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(hil.CheckError):
            runner.disconnect()
        self.assertEqual("INCONCLUSIVE", runner.results[-1].outcome)

    def test_argument_validation(self):
        for arguments in ([], ["--dry-run", "--stress-count", "0"], ["--dry-run", "--command-timeout", "nan"],
                          ["--dry-run", "--min-c", "nan"], ["--dry-run", "--min-c", "20", "--max-c", "10"],
                          ["--dry-run", "--model", "tmp112d", "--address", "0x48"]):
            with self.subTest(arguments=arguments), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                hil.parse_args(arguments)

    def test_dry_run_is_not_hardware_evidence(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(0, hil.main(["--dry-run", "--suite", "smoke", "--suite", "oneshot"]))
        self.assertIn("DRY RUN", output.getvalue())
        self.assertNotIn("PASS", output.getvalue())
        self.assertNotIn("busreset", output.getvalue())

    def test_restoration_runs_after_failed_mutating_suite(self):
        runner = hil.Runner(mock.Mock(), hil.parse_args(["--dry-run", "--suite", "oneshot"]))
        profile = hil.parse_settings(SETTINGS)
        def preflight():
            runner.baseline = profile
        def oneshot():
            runner.mutated = True
            raise hil.CheckError("injected write failure")
        runner.preflight, runner.oneshot = preflight, oneshot
        runner.restore = mock.Mock()
        with contextlib.redirect_stdout(io.StringIO()):
            runner.run()
        runner.restore.assert_called_once()
        self.assertTrue(any(result.outcome == "INCONCLUSIVE" for result in runner.results))

    def test_failed_restoration_retains_captured_profile_and_fails_report(self):
        runner = hil.Runner(mock.Mock(), hil.parse_args(["--dry-run"]))
        runner.baseline = hil.parse_settings(SETTINGS)
        runner.mutated = True
        runner.command = mock.Mock(side_effect=hil.CheckError("bus disconnected"))
        with contextlib.redirect_stdout(io.StringIO()):
            runner.restore()
        self.assertEqual("not verified", runner.restoration)
        self.assertEqual("FAIL", runner.results[-1].outcome)
        self.assertEqual(75.0, runner.baseline["low"])

    def test_report_has_skips_and_inconclusive_without_hardware_claim(self):
        runner = hil.Runner(hil.SerialSession(FakeSerial(), 1, 1), hil.parse_args(["--dry-run"]))
        runner.results = [hil.Result("GPIO", "", "SKIP", "unwired"), hil.Result("serial", "", "INCONCLUSIVE", "timeout")]
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            path = Path(directory) / "report.json"
            self.assertEqual(2, hil.write_report(path, runner))
            report = json.loads(path.read_text())
            self.assertFalse(report["hardware_run"])
            self.assertEqual("INCONCLUSIVE", report["outcome"])
            self.assertEqual(1, report["counts"]["SKIP"])
            self.assertTrue(path.with_suffix(".txt").exists())

    @unittest.skipUnless(CLI_FIXTURE, "pass --cli-fixture PATH to validate actual shared CLI output")
    def test_all_automated_non_stimulus_suites_against_real_cli_model(self):
        serial = ProcessSerial(CLI_FIXTURE)
        args = hil.parse_args(["--port", "native-model", "--model", "tmp102", "--address", "0x48",
                               "--suite", "smoke", "--suite", "config", "--suite", "oneshot", "--suite", "stress",
                               "--stress-count", "3", "--silence-seconds", "0.01"])
        session = hil.SerialSession(serial, 3, 8)
        runner = hil.Runner(session, args)
        try:
            session.capture_boot(0.25)
            with contextlib.redirect_stdout(io.StringIO()):
                runner.run()
            failures = [result for result in runner.results if result.outcome in {"FAIL", "INCONCLUSIVE"}]
            self.assertEqual([], failures, "\n\n".join(session.transcript))
            self.assertEqual("verified", runner.restoration)
            self.assertGreater(len(runner.results), 60)
        finally:
            serial.close()

    @unittest.skipUnless(CLI_FIXTURE, "pass --cli-fixture PATH to validate actual shared CLI output")
    def test_reordered_suites_restore_their_entry_profile(self):
        serial = ProcessSerial(CLI_FIXTURE)
        args = hil.parse_args(["--port", "native-model", "--model", "tmp102", "--address", "0x48",
                               "--suite", "oneshot", "--suite", "config", "--suite", "smoke"])
        session = hil.SerialSession(serial, 3, 8)
        runner = hil.Runner(session, args)
        try:
            session.capture_boot(0.2)
            with contextlib.redirect_stdout(io.StringIO()):
                runner.run()
            self.assertFalse([result for result in runner.results if result.outcome in {"FAIL", "INCONCLUSIVE"}],
                             "\n\n".join(session.transcript))
            self.assertEqual("verified", runner.restoration)
        finally:
            serial.close()

    @unittest.skipUnless(CLI_FIXTURE, "pass --cli-fixture PATH to validate actual shared CLI output")
    def test_real_cli_cleanup_after_partial_write_cancellation(self):
        serial = ProcessSerial(CLI_FIXTURE)
        session = hil.SerialSession(serial, 1, 2)
        runner = hil.Runner(session, hil.parse_args(["--dry-run"]))
        try:
            session.capture_boot(0.2)
            serial.write(b"$pause\n")
            session.capture_boot(0.05)
            serial.write(b"extended 1\n")
            session.capture_boot(0.05)
            serial.write(b"$tick 2\n")
            session.capture_boot(0.05)
            with contextlib.redirect_stdout(io.StringIO()):
                runner.stop_owned_work()
            self.assertFalse([result for result in runner.results if result.outcome != "PASS"],
                             "\n\n".join(session.transcript))
            self.assertIn("CANCELLED", session.transcript[-3])
        finally:
            serial.close()

    @unittest.skipUnless(CLI_FIXTURE, "pass --cli-fixture PATH to validate actual shared CLI output")
    def test_real_cli_cleanup_consumes_retained_shutdown_shot(self):
        serial = ProcessSerial(CLI_FIXTURE)
        session = hil.SerialSession(serial, 1, 2)
        runner = hil.Runner(session, hil.parse_args(["--dry-run"]))
        try:
            session.capture_boot(0.2)
            self.assertEqual("PASS", session.command("mode shutdown").outcome)
            for command in ("$pause", "stress 2", "$tick 1"):
                serial.write((command + "\n").encode())
                session.capture_boot(0.05)
            self.assertEqual("PASS", session.command("stop").outcome)
            health = hil.parse_health(session.command("health").output)
            self.assertTrue(health["pending"])
            serial.write(b"$tick 40\n")
            session.capture_boot(0.05)
            with contextlib.redirect_stdout(io.StringIO()):
                runner.stop_owned_work()
            self.assertIn("tryread", [result.command for result in runner.results])
            self.assertFalse(hil.parse_health(session.command("health").output)["pending"])
        finally:
            serial.close()


if __name__ == "__main__":
    unittest.main()
