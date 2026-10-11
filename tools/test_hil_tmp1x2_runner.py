#!/usr/bin/env python3
"""HIL host parser/cleanup regressions; optionally use the real native CLI model."""
from __future__ import annotations

import contextlib
import io
import json
from pathlib import Path
import queue
import re
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
FIRMWARE = ("TMP1x2 diagnostic CLI | Arduino-ESP32 3.3.0 | esp32s3 | built Oct 11 2026 12:00:00\n"
            "Library: 1.0.0 (abcdef0, clean, 2026-10-11T10:00:00Z)\n"
            "Firmware: git=abcdef0123456789abcdef0123456789abcdef01 tree=clean built=2026-10-11T10:00:00Z\n> ")
CONSOLE = "Console: queued=0 sent=400 dropped=0 truncated=0\n> "
BUS = "Bus settings: SDA=8 SCL=9 frequency=400000 Hz timeout=50 ms\n> "
TEMPERATURE_SUMMARY = "Temperature summary: min=24.9375 C max=25.0625 C mean=25.0000 C\n"
SNAPSHOT = "Snapshot: CONFIG=0x60A0 TEMP=0x1900 TLOW=0x4B00 THIGH=0x5000 " + hil.SNAPSHOT_TRUST + "\n"
SELF_CHECK_LABELS = (
    "transport binding", "initialized driver", "presence/configuration plausibility",
    "live configuration and trust", "desired configuration and threshold readback",
    "decoded thresholds and ordering", "temperature register format", "physical ALERT GPIO",
    "coherent register snapshot",
)
CONFIG_LABELS = (
    "rate 0.25 Hz", "rate 1 Hz", "rate 4 Hz", "rate 8 Hz", "continuous mode", "shutdown mode",
    "normal format", "extended format", "comparator mode", "interrupt mode",
    "active-low polarity", "active-high polarity", "fault queue 1", "fault queue 2",
    "fault queue 4", "fault queue 6", "threshold window 0..50 C", "threshold window -10..30 C",
    "restore captured profile",
)


def diagnostic(command, *, skip_gpio=False):
    labels = {"selfcheck": SELF_CHECK_LABELS, "selftest full": CONFIG_LABELS,
              "settings read": SELF_CHECK_LABELS[3:6], "snapshot read": SELF_CHECK_LABELS[8:]}[command]
    rows = [f"[SKIP] {label}: optional GPIO not configured\n" if skip_gpio and label == "physical ALERT GPIO"
            else f"[PASS] {label}\n" for label in labels]
    if command in {"selfcheck", "snapshot read"}:
        rows.insert(0, SNAPSHOT)
    skipped = int(skip_gpio and command == "selfcheck")
    rows.append(f"Diagnostic summary: complete pass={len(labels) - skipped} fail=0 skip={skipped}\n")
    if command == "selftest full":
        rows.append("Baseline restoration: verified\n")
    return "".join(rows) + "> "


def discovery():
    rows = []
    for address in (*range(0x40, 0x44), *range(0x48, 0x4C)):
        raw, candidate, code = ("0x60A0", "yes", "OK") if address == 0x48 else (
            ("0x0000", "no", "CONFIG_MISMATCH") if address == 0x49 else
            ("unavailable", "no", "I2C_NACK_ADDR"))
        rows.append(f"Discover address=0x{address:02X} config={raw} candidate={candidate} code={code} detail=0 identity=unverified\n")
    return ("[I] IN_PROGRESS detail=0\n> " + "".join(rows) +
            "Discovery complete: checked=8 ack=2 readable=2 candidates=1 incompatible=1 absent=6 errors=0 identity=unverified\n[I] OK detail=0\n> ")


def operation(token=5, status="OK", kind="CONFIGURE"):
    return (f"Operation started: token={token} kind={kind}; job and cancel remain available.\n"
            "[I] IN_PROGRESS detail=0\n> "
            f"Operation result: token={token} kind={kind} elapsed=23 ms write-attempted=yes\n"
            f"[{'I' if status == 'OK' else 'E'}] {status} detail=0: {status}\n> ")


class Clock:
    def __init__(self, step=0.01):
        self.now = 0.0
        self.step = step

    def __call__(self):
        self.now += self.step
        return self.now


class FakeSerial:
    """Stage replies until a command is written, with a separate marker reply."""
    def __init__(self, *chunks, write_result=None, startup=False, marker_reply=None, poison_reply=None):
        self.chunks = list(chunks)
        self.written = b""
        self.write_result = write_result
        self.active = startup
        self.marker_reply = marker_reply
        self.poison_reply = poison_reply
        self.marker_chunks = 0

    @property
    def in_waiting(self):
        return len(self.chunks[0]) if self.active and self.chunks else 0

    @property
    def commands(self):
        return [line.decode("ascii") for line in self.written.splitlines()
                if line and line != b"\x03" and not line.startswith(b"hilmark ")]

    def write(self, data):
        self.written += data
        self.active = True
        if data == b"\x03\n":
            self.chunks.insert(0, self.poison_reply if self.poison_reply is not None else
                               b"[E] INVALID_PARAM detail=0: Invalid control character; command discarded\n> ")
        elif data.startswith(b"hilmark "):
            token = data.split()[1]
            reply = (b"CLI marker: token=" + token + b"\n> " if self.marker_reply is None
                     else self.marker_reply(token))
            replies = reply if isinstance(reply, list) else [reply]
            self.chunks[:0] = replies
            self.marker_chunks = len(replies)
        return self.write_result if self.write_result is not None else len(data)

    def read(self, size):
        if not self.active or not self.chunks:
            return b""
        data = self.chunks.pop(0)
        if self.marker_chunks:
            self.marker_chunks -= 1
            if not self.marker_chunks:
                self.active = False
        return data


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


class SoakSession:
    """Deterministic command durations and real parser use without UART or sleep."""
    def __init__(self, failure_batch=None, interrupt_batch=None, console_loss_batch=None,
                 temperature_failure_batch=None, stale_sample_batch=None, stalled_health_batch=None):
        self.now = 0.0
        self.batches = 0
        self.logged_bytes = 0
        self.transcript_error = None
        self.failure_batch = failure_batch
        self.interrupt_batch = interrupt_batch
        self.console_loss_batch = console_loss_batch
        self.temperature_failure_batch = temperature_failure_batch
        self.stale_sample_batch = stale_sample_batch
        self.stalled_health_batch = stalled_health_batch
        self.restarted = False
        self.framing_lost = False
        self.health_successes = 20

    def log(self, data):
        self.logged_bytes += len(data)

    def command(self, command, **kwargs):
        self.now += 0.25
        if command == "settings":
            output = SETTINGS
        elif command.startswith("verbose"):
            output = "Verbose: " + ("off" if command.endswith(" 0") else "on") + "\n> "
        elif command.startswith("stress_mix "):
            self.batches += 1
            count = int(command.split()[1])
            if self.batches != self.stalled_health_batch:
                self.health_successes += 5 * count
            fail = int(self.batches == self.failure_batch)
            output = (f"Run: stopped target={count} completed={count} ok={4 * count - fail} fail={fail} elapsed=1000 ms remaining=0\n"
                      f"Mixed checks: completed={4 * count} samples={count} (four checks per complete cycle; shutdown sampling may trigger OS)\n"
                      + TEMPERATURE_SUMMARY +
                      f"Tracked health delta: ok={5 * count} fail=0\nAdapter delta: attempts={6 * count} ok={6 * count} fail=0\n> ")
            if self.batches == self.temperature_failure_batch:
                output = output.replace("max=25.0625", "max=125.0625")
        elif command == "sample":
            if self.batches == self.interrupt_batch:
                raise KeyboardInterrupt()
            sample_batch = self.batches - int(self.batches == self.stale_sample_batch)
            output = SAMPLE.replace("timestamp=200", f"timestamp={200 + sample_batch * 1000}")
        elif command == "health":
            output = HEALTH.replace("ok=20", f"ok={self.health_successes}")
        elif command == "console":
            output = CONSOLE.replace("dropped=0", "dropped=1") if self.batches == self.console_loss_batch else CONSOLE
        else:
            raise AssertionError("unmodeled soak command: " + command)
        outcome, reason = hil.classify(command, output)
        return hil.Result(command, command, outcome, reason, output, 0.25)


class ParserTests(unittest.TestCase):
    def test_required_records_reject_every_missing_duplicate_or_malformed_row(self):
        cases = ((hil.parse_health, HEALTH), (hil.parse_settings, SETTINGS),
                 (hil.parse_firmware, FIRMWARE), (hil.parse_console, CONSOLE),
                 (hil.parse_counters, "Bus: attempts=20 ok=20 fail=0\n> "))
        for parser, output in cases:
            rows = output.removesuffix("> ").splitlines(keepends=True)
            parser(output)
            for index, row in enumerate(rows):
                malformed = ("TMP1x2 diagnostic CLI\n" if row.startswith("TMP1x2 diagnostic CLI")
                             else row.split(":", 1)[0] + ":\n")
                mutations = (rows[:index] + rows[index + 1:],
                             rows[:index] + [row, row] + rows[index + 1:],
                             rows[:index] + [row, malformed] + rows[index + 1:],
                             rows[:index] + [malformed] + rows[index + 1:])
                for variant, altered in enumerate(mutations):
                    with self.subTest(parser=parser.__name__, row=index, variant=variant), self.assertRaises(hil.CheckError):
                        parser("".join(altered) + "> ")

    def test_later_conflicting_records_cannot_hide_behind_earlier_success(self):
        cases = (("health", HEALTH, HEALTH.replace("state=READY online=yes", "state=OFFLINE online=no")),
                 ("console", CONSOLE, CONSOLE.replace("dropped=0", "dropped=9")),
                 ("sample", SAMPLE, SAMPLE.replace("25.0000", "90.0000")),
                 ("version", FIRMWARE, FIRMWARE.replace("tree=clean", "tree=dirty")),
                 ("snapshot read", diagnostic("snapshot read"),
                  "Diagnostic summary: complete pass=0 fail=99 skip=0\n> "))
        for command, first, second in cases:
            with self.subTest(command=command):
                self.assertNotEqual("PASS", hil.classify(command, first.removesuffix("> ") + second)[0])

    def test_diagnostic_labels_and_summaries_must_be_complete_and_unique(self):
        for command in ("selfcheck", "selftest full", "settings read", "snapshot read"):
            good = diagnostic(command)
            self.assertEqual("PASS", hil.classify(command, good)[0])
            rows = good.removesuffix("> ").splitlines(keepends=True)
            for row in rows:
                if not row.startswith(("[PASS]", "Diagnostic summary:")):
                    continue
                for changed in (good.replace(row, "", 1), good.replace(row, row + row, 1),
                                good.replace(row, row.rstrip("\n") + " BROKEN\n", 1)):
                    with self.subTest(command=command, row=row, changed=changed):
                        self.assertNotEqual("PASS", hil.classify(command, changed)[0])
        skipped_core = diagnostic("selfcheck", skip_gpio=True).replace(
            "[PASS] transport binding", "[SKIP] transport binding: transport unbound").replace(
            "[SKIP] physical ALERT GPIO: optional GPIO not configured", "[PASS] physical ALERT GPIO")
        self.assertNotEqual("PASS", hil.classify("selfcheck", skipped_core)[0])

    def test_operation_kind_order_uniqueness_and_token_bounds(self):
        good = operation(kind="RECOVER")
        admission, terminal = good.split("Operation result:", 1)
        bad = (operation(kind="READ"), good + good,
               "Operation result:" + terminal + admission,
               good.replace("Operation started: token=5", "Operation started: token=5 junk"),
               operation(token=0, kind="RECOVER"), operation(token=4294967296, kind="RECOVER"),
               good.replace("[I] OK detail=0", "[I] OK detail=0\n[I] OK detail=0"))
        for output in bad:
            with self.subTest(output=output):
                self.assertNotEqual("PASS", hil.classify("recover", output)[0])
        self.assertEqual("PASS", hil.classify("recover", good)[0])

    def test_operation_tokens_advance_and_allow_uint32_wrap(self):
        for tokens, expected in (((5, 5), ("PASS", "INCONCLUSIVE")),
                                 ((5, 4), ("PASS", "INCONCLUSIVE")),
                                 ((5, 6), ("PASS", "PASS")),
                                 ((0xFFFFFFFF, 1), ("PASS", "PASS"))):
            with self.subTest(tokens=tokens):
                serial = FakeSerial(*(operation(token=token, kind="RECOVER").encode() for token in tokens))
                session = hil.SerialSession(serial, 1, 2, Clock())
                results = [session.command("recover"), session.command("recover")]
                self.assertEqual(expected, tuple(result.outcome for result in results))
                if expected[-1] != "PASS":
                    before = serial.written
                    self.assertEqual("INCONCLUSIVE", session.command("stop").outcome)
                    self.assertEqual(before, serial.written)

    def test_manual_job_admission_cannot_reuse_an_operation_token(self):
        admission = operation(token=6, kind="SHUTDOWN").split("Operation result:")[0].encode()
        serial = FakeSerial(admission, operation(token=6, kind="RECOVER").encode())
        session = hil.SerialSession(serial, 1, 2, Clock())
        self.assertEqual("PASS", session.command("job start shutdown 5000").outcome)
        self.assertEqual("INCONCLUSIVE", session.command("recover").outcome)
        self.assertTrue(session.framing_lost)

    def test_manual_terminal_must_match_the_admitted_token_and_kind(self):
        admission = operation(token=6, kind="SHUTDOWN").split("Operation result:")[0].encode()
        status = "[E] CANCELLED detail=0: Owner operation cancelled\n"
        for token, kind in ((5, "SHUTDOWN"), (6, "READ")):
            with self.subTest(token=token, kind=kind):
                terminal = f"Operation result: token={token} kind={kind} elapsed=200 ms write-attempted=no\n"
                serial = FakeSerial(admission, (status + terminal + status + "> > ").encode())
                session = hil.SerialSession(serial, 1, 2, Clock())
                self.assertEqual("PASS", session.command("job start shutdown 5000").outcome)
                result = session.command("job cancel")
                self.assertEqual("INCONCLUSIVE", result.outcome)
                self.assertFalse(hil.expected_error(result, {"CANCELLED"}))
                before = serial.written
                self.assertEqual("INCONCLUSIVE", session.command("recover").outcome)
                self.assertEqual(before, serial.written)

    def test_expected_error_rejects_mixed_or_duplicate_status_records(self):
        for payload in ("[E] INVALID_PARAM detail=0\n[I] OK detail=0\n> ",
                        "[E] INVALID_PARAM detail=0\n[E] INVALID_PARAM detail=0\n> ",
                        "[E] INVALID_PARAM detail=0\n[E] malformed\n> "):
            result = hil.Result("invalid command", "invalid command", "FAIL", "INVALID_PARAM", payload)
            with self.subTest(payload=payload):
                self.assertFalse(hil.expected_error(result, {"INVALID_PARAM"}))

    def test_generic_success_requires_exactly_one_complete_ok_record(self):
        good = "[I] OK detail=0: OK\n"
        for command in ("verify", "probe", "start", "rawwrite 2 0x4B00", "invalidate"):
            with self.subTest(command=command):
                self.assertEqual("PASS", hil.classify(command, good + "> ")[0])
                for output in (good + good, good + "[I] OK malformed\n",
                               good + "[I] OKAY detail=0\n", "[I] OK detail=0: OK BROKEN\n"):
                    with self.subTest(output=output):
                        self.assertNotEqual("PASS", hil.classify(command, output + "> ")[0])

    def test_fifo_marker_does_not_hide_two_success_responses(self):
        class FifoSerial(FakeSerial):
            def write(self, data):
                if not data.startswith(b"hilmark "):
                    return super().write(data)
                self.written += data
                self.active = True
                self.chunks.append(b"CLI marker: token=" + data.split()[1] + b"\n> ")
                # Existing wire bytes must arrive before the newly generated marker.
                self.marker_chunks = len(self.chunks)
                return len(data)
        reply = b"[I] OK detail=0: OK\n> "
        stream = io.BytesIO()
        serial = FifoSerial(reply, reply)
        session = hil.SerialSession(serial, 1, 2, Clock(), stream)
        result = session.command("verify")
        self.assertNotEqual("PASS", result.outcome)
        self.assertEqual(2, result.output.count("[I] OK"))
        self.assertNotIn("CLI marker:", result.output)
        wire = stream.getvalue()
        self.assertLess(wire.rfind(b"[I] OK detail=0: OK"), wire.index(b"CLI marker:"))
        self.assertEqual(["verify"], serial.commands)

    def test_manual_terminal_error_requires_its_two_status_records_and_one_result(self):
        for command, code, prefix in (("job cancel", "CANCELLED", ""),
                                      ("job step 0", "OPERATION_TIMEOUT", "Poll: budget=0 transfers=0 done=yes next=200 ms\n")):
            status = f"[E] {code} detail=0: Owner operation ended\n"
            terminal = "Operation result: token=6 kind=SHUTDOWN elapsed=200 ms write-attempted=no\n"
            output = prefix + status + terminal + status + "> > "
            result = hil.Result(command, command, "FAIL", code, output)
            with self.subTest(command=command):
                self.assertTrue(hil.expected_error(result, {code}))
                for changed in (output.replace(terminal, terminal + terminal),
                                output.replace(terminal, ""), output.replace(terminal, "Operation result: malformed\n"),
                                output.replace(terminal, terminal + status), output.replace(status, "[I] OK detail=0\n", 1)):
                    result.output = changed
                    self.assertFalse(hil.expected_error(result, {code}))

    def test_fresh_marker_is_required_and_kept_only_in_live_transcript(self):
        stream = io.BytesIO()
        serial = FakeSerial(b"[I] OK detail=0\n> ", b"[I] OK detail=0\n> ")
        session = hil.SerialSession(serial, 1, 2, Clock(), stream)
        first, second = session.command("verify"), session.command("verify")
        self.assertEqual(("PASS", "PASS"), (first.outcome, second.outcome))
        tokens = re.findall(rb"(?m)^hilmark ([0-9a-f]{32})$", serial.written)
        self.assertEqual(2, len(tokens))
        self.assertNotEqual(tokens[0], tokens[1])
        self.assertNotIn("CLI marker:", first.output + second.output)
        self.assertEqual(2, stream.getvalue().count(b"CLI marker:"))

    def test_wrong_missing_or_contaminated_marker_stops_further_commands(self):
        replies = (lambda token: b"", lambda token: b"CLI marker: token=" + b"0" * 32 + b"\n> ",
                   lambda token: b"CLI marker: token=" + token + b"\n> unwanted payload\n> ",
                   lambda token: b"CLI marker: token=" + token + b"\nCLI marker: token=" + token + b"\n> ")
        for reply in replies:
            with self.subTest(reply=reply):
                serial = FakeSerial(b"[I] OK detail=0\n> ", marker_reply=reply)
                session = hil.SerialSession(serial, 1, 2, Clock())
                self.assertEqual("INCONCLUSIVE", session.command("verify").outcome)
                self.assertTrue(session.framing_lost)
                before = serial.written
                self.assertEqual("INCONCLUSIVE", session.command("recover").outcome)
                self.assertEqual(before, serial.written)

    def test_marker_can_arrive_fragmented_after_response_prompt(self):
        serial = FakeSerial(b"[I] OK detail=0\n> ", marker_reply=lambda token:
                            [b"CLI mar", b"ker: token=" + token[:8], token[8:] + b"\r\n", b"> "])
        self.assertEqual("PASS", hil.SerialSession(serial, 1, 2, Clock()).command("verify").outcome)

    def test_pending_payload_is_preserved_and_rejected_before_new_write(self):
        stream = io.BytesIO()
        serial = FakeSerial(b"[I] OK detail=0\n> ", startup=True)
        session = hil.SerialSession(serial, 1, 2, Clock(), stream)
        result = session.command("verify")
        self.assertEqual("INCONCLUSIVE", result.outcome)
        self.assertEqual(b"", serial.written)
        self.assertIn(b"[I] OK detail=0", stream.getvalue())
        self.assertTrue(session.framing_lost)

    def test_initial_synchronization_discards_partial_input_before_marker(self):
        serial = FakeSerial()
        session = hil.SerialSession(serial, 1, 2, Clock())
        session.synchronize()
        self.assertTrue(serial.written.startswith(b"\x03\nhilmark "), serial.written)
        self.assertFalse(session.framing_lost)

    def test_initial_synchronization_rejects_unrelated_errors_and_restart(self):
        exact = b"[E] INVALID_PARAM detail=0: Invalid control character; command discarded\n> "
        for reply in (b"[E] INVALID_PARAM detail=0\n> ",
                      b"[E] INVALID_PARAM detail=0: invalid command/arguments; see help\n> ",
                      b"Diagnostic CLI; application owns the bus.\n" + exact,
                      b"ESP-ROM:esp32s3\n" + exact,
                      exact.replace(b"detail=0", b"detail=1")):
            with self.subTest(reply=reply):
                serial = FakeSerial(poison_reply=reply)
                session = hil.SerialSession(serial, 1, 2, Clock())
                result = session.synchronize()
                self.assertNotEqual("PASS", result.outcome)
                before = serial.written
                self.assertEqual("INCONCLUSIVE", session.command("recover").outcome)
                self.assertEqual(before, serial.written)

    def test_main_does_not_enter_suites_after_unproven_input_synchronization(self):
        connection = FakeSerial(poison_reply=b"[E] INVALID_PARAM detail=0: unrelated rejection\n> ")
        connection.open, connection.close = mock.Mock(), mock.Mock()
        serial = types.SimpleNamespace(Serial=mock.Mock(return_value=connection))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "report.json"
            with mock.patch.dict(sys.modules, {"serial": serial}), mock.patch.object(hil.SerialSession, "capture_boot"), \
                    mock.patch.object(hil.Runner, "run") as run, contextlib.redirect_stdout(io.StringIO()):
                self.assertNotEqual(0, hil.main(["--port", "fake", "--model", "tmp102", "--address", "0x48", "--report", str(path)]))
            run.assert_not_called()
            self.assertNotEqual("PASS", json.loads(path.read_text())["outcome"])
            self.assertNotIn("recover", connection.commands)

    def test_response_received_after_deadline_is_retained_without_marker_write(self):
        clock = Clock(step=0)
        stream = io.BytesIO()
        serial = FakeSerial(b"[I] OK detail=0\n> ")
        read = serial.read
        def late_read(size):
            clock.now += 2
            return read(size)
        serial.read = late_read
        session = hil.SerialSession(serial, 1, 2, clock, stream)
        result = session.command("verify")
        self.assertEqual("INCONCLUSIVE", result.outcome)
        self.assertIn("[I] OK detail=0", result.output)
        self.assertIn(b"[I] OK detail=0", stream.getvalue())
        self.assertEqual(b"verify\n", serial.written)
        self.assertTrue(session.framing_lost)

    def test_deadline_is_rechecked_after_host_logging_before_write(self):
        clock = Clock(step=0)
        serial = FakeSerial(b"[I] OK detail=0\n> ")
        session = hil.SerialSession(serial, 1, 2, clock)
        def delayed_log(data):
            clock.now += 2
        session.log = delayed_log
        result = session.command("verify")
        self.assertEqual("INCONCLUSIVE", result.outcome)
        self.assertEqual(b"", serial.written)

    def test_run_temperature_summary_requires_unique_ordered_finite_window(self):
        self.assertEqual("PASS", hil.check_run_temperature(TEMPERATURE_SUMMARY + "> ", -40, 125)[0])
        for output in ("", TEMPERATURE_SUMMARY * 2,
                       TEMPERATURE_SUMMARY + "Temperature summary: malformed\n",
                       TEMPERATURE_SUMMARY.replace("min=24.9375", "min=nan"),
                       TEMPERATURE_SUMMARY.replace("max=25.0625", "max=inf"),
                       TEMPERATURE_SUMMARY.replace("min=24.9375", "min=26.0000"),
                       TEMPERATURE_SUMMARY.replace("mean=25.0000", "mean=26.0000"),
                       TEMPERATURE_SUMMARY.replace("min=24.9375", "min=-40.0625"),
                       TEMPERATURE_SUMMARY.replace("max=25.0625", "max=125.0625")):
            with self.subTest(output=output):
                self.assertNotEqual("PASS", hil.check_run_temperature(output + "> ", -40, 125)[0])

    def test_register_replies_identify_exact_requested_addresses_once(self):
        self.assertEqual("PASS", hil.classify("rawread 2", "Raw 0x02 = 0x4B00\n> ")[0])
        for reply in ("Raw 0x03 = 0x4B00\n> ", "Raw 0x02 = 0x4B00\nRaw 0x02 = 0x5000\n> ",
                      "Raw 0x02 = 0x4B00\nRaw 0x02 = broken\n> "):
            self.assertNotEqual("PASS", hil.classify("rawread 2", reply)[0])
        rows = [f"0x{register:02X} = 0x{word:04X}\n" for register, word in
                enumerate((0x1900, 0x60A0, 0x4B00, 0x5000))]
        for command in ("dump", "rawdump"):
            self.assertEqual("PASS", hil.classify(command, "".join(rows) + "> ")[0])
            for index, row in enumerate(rows):
                for reply in ("".join(rows[:index] + rows[index + 1:]),
                              "".join(rows[:index] + [row, row] + rows[index + 1:]),
                              "".join(rows).replace(row, row.replace(" = ", " malformed "), 1),
                              "".join(rows).replace(row, row.replace(f"0x{index:02X}", "0x04", 1), 1)):
                    with self.subTest(command=command, index=index, reply=reply):
                        self.assertNotEqual("PASS", hil.classify(command, reply + "> ")[0])

    def test_discovery_requires_complete_per_address_evidence_and_terminal_status(self):
        output = discovery()
        for command in ("discover", "detect"):
            self.assertTrue(hil.response_complete(command, output))
            self.assertEqual("PASS", hil.classify(command, output)[0])
            self.assertFalse(hil.response_complete(command, "[I] IN_PROGRESS detail=0\n> "))
            self.assertEqual("INCONCLUSIVE", hil.classify(command, output.replace("[I] OK detail=0", ""))[0])
        self.assertEqual(dict(ack=2, candidates={0x48}, absent=6), hil.parse_discovery(output))
        for old, new in (("checked=8", "checked=7"), ("ack=2", "ack=3"),
                         ("readable=2", "readable=1"), ("absent=6", "absent=5"),
                         ("address=0x40", "address=0x48"), ("address=0x40", "address=0x44"),
                         ("config=0x60A0", "config=0x60A1"), ("config=0x0000", "config=0x60A0"),
                         ("Discovery complete", "Discovery cancelled"),
                         ("identity=unverified", "identity=TMP102")):
            with self.subTest(old=old, new=new):
                self.assertNotEqual("PASS", hil.classify("discover", output.replace(old, new))[0])
        self.assertEqual("FAIL", hil.classify("discover", output.replace("errors=0", "errors=1"))[0])
        self.assertEqual("FAIL", hil.classify("discover", output.replace("[I] OK", "[E] I2C_TIMEOUT"))[0])

    def test_discovery_session_waits_past_admission_prompt(self):
        admission, evidence = discovery().split("> ", 1)
        session = hil.SerialSession(FakeSerial((admission + "> ").encode(), evidence.encode()), 5, 5, Clock())
        result = session.command("discover")
        self.assertEqual("PASS", result.outcome)
        self.assertIn("Discovery complete", result.output)

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
        session = hil.SerialSession(FakeSerial(b"first\n", b"overflow", startup=True), 1, 1, Clock(), stream)
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
        self.assertTrue(session.framing_lost)

    def test_interrupted_command_retains_partial_output(self):
        serial = FakeSerial(b"[I] IN_PROGRESS detail=0\n")
        original = serial.read
        def interrupt(size):
            if serial.chunks:
                return original(size)
            raise KeyboardInterrupt()
        serial.read = interrupt
        session = hil.SerialSession(serial, 1, 1, Clock())
        result = session.command("extended 1")
        self.assertEqual("INCONCLUSIVE", result.outcome)
        self.assertIn("IN_PROGRESS", result.output)
        self.assertEqual("operator interrupted", result.reason)
        self.assertTrue(session.framing_lost)

    def test_deadline_bounds_underlying_read_and_write_timeout(self):
        serial = FakeSerial(b"[I] OK detail=0\n> ")
        serial.timeout, serial.write_timeout = 5.0, 5.0
        session = hil.SerialSession(serial, 5, 120, Clock(step=0.001))
        result = session.command("verify", timeout_s=0.04)
        self.assertEqual("PASS", result.outcome)
        self.assertLessEqual(serial.timeout, 0.04)
        self.assertLessEqual(serial.write_timeout, 0.04)
        prior = serial.written
        self.assertEqual("INCONCLUSIVE", session.command("verify", timeout_s=0).outcome)
        self.assertEqual(prior, serial.written)
        self.assertFalse(session.framing_lost)  # Nothing was sent at this expired admission.

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
            self.assertIn("serial close", [result["check"] for result in report["results"]])

    def test_serial_control_lines_are_selected_before_open_and_recorded(self):
        for dtr, rts in ((0, 0), (1, 0), (1, 1)):
            with self.subTest(dtr=dtr, rts=rts), tempfile.TemporaryDirectory() as directory:
                connection = FakeSerial()
                def opened():
                    self.assertIs(bool(dtr), connection.dtr)
                    self.assertIs(bool(rts), connection.rts)
                connection.open = mock.Mock(side_effect=opened)
                connection.close = mock.Mock()
                serial = types.SimpleNamespace(Serial=mock.Mock(return_value=connection))
                def passed(runner):
                    runner.record(hil.Result("control line setup", "", "PASS", "checked before opening"))
                    for suite in runner.suites:
                        suite["outcome"] = "PASS"
                path = Path(directory) / "report.json"
                args = ["--port", "fake", "--model", "tmp102", "--address", "0x48", "--report", str(path),
                        "--dtr", str(dtr), "--rts", str(rts)]
                with mock.patch.dict(sys.modules, {"serial": serial}), mock.patch.object(hil.SerialSession, "capture_boot"), mock.patch.object(hil.Runner, "run", passed), contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(0, hil.main(args))
                connection.open.assert_called_once()
                report = json.loads(path.read_text())
                self.assertEqual(dtr, report["metadata"]["dtr"])
                self.assertEqual(rts, report["metadata"]["rts"])

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
            for suite in runner.suites:
                suite["outcome"] = "PASS"
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
        self.assertEqual(["extended 1"], serial.commands)
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
                session = hil.SerialSession(serial, 0.1, 1, Clock())
                self.assertEqual("INCONCLUSIVE", session.command("probe").outcome)
                self.assertTrue(session.framing_lost)
                before = serial.written
                self.assertEqual("INCONCLUSIVE", session.command("stop").outcome)
                self.assertEqual(before, serial.written)

    def test_lost_serial_framing_defers_cleanup_without_further_writes(self):
        for serial in (FakeSerial(), FakeSerial(write_result=3), FakeSerial(b"[I] OK detail=0\n")):
            for mutated in (False, True):
                with self.subTest(serial=serial, mutated=mutated):
                    session = hil.SerialSession(serial, 0.1, 1, Clock())
                    runner = hil.Runner(session, hil.parse_args(["--dry-run"]))
                    runner.baseline = hil.parse_settings(SETTINGS)
                    runner.background_started, runner.mutated = True, mutated
                    self.assertEqual("INCONCLUSIVE", session.command("threshold 20 30").outcome)
                    before = serial.written
                    with contextlib.redirect_stdout(io.StringIO()):
                        runner.restore()
                    self.assertEqual(before, serial.written)
                    self.assertEqual("FAIL" if mutated else "INCONCLUSIVE", runner.results[-1].outcome)
                    self.assertEqual("not verified" if mutated else "not required", runner.restoration)
                    self.assertEqual(75.0, runner.baseline["low"])

    def test_complete_semantic_failure_keeps_serial_framing(self):
        serial = FakeSerial(b"[E] INVALID_PARAM detail=0\n> ", b"Watch stopped: ok=0 fail=0\n> ")
        session = hil.SerialSession(serial, 1, 2, Clock())
        self.assertEqual("FAIL", session.command("wreg 0 1").outcome)
        self.assertFalse(session.framing_lost)
        self.assertEqual("PASS", session.command("stop").outcome)
        self.assertEqual(["wreg 0 1", "stop"], serial.commands)

    def test_terminal_failures_and_cancelled_diagnostics(self):
        self.assertEqual("FAIL", hil.classify("extended 1", operation(status="CONFIG_MISMATCH"))[0])
        self.assertEqual("FAIL", hil.classify("probe", "[E] I2C_NACK_ADDR detail=5\n> ")[0])
        self.assertEqual("INCONCLUSIVE", hil.classify("selfcheck", "Diagnostic summary: cancelled pass=1 fail=0 skip=0\n> ")[0])

    def test_full_test_requires_all_cases_and_verified_restoration(self):
        good = diagnostic("selftest full")
        self.assertEqual("PASS", hil.classify("selftest full", good)[0])
        self.assertEqual("INCONCLUSIVE", hil.classify("selftest full", good.replace("pass=19", "pass=18"))[0])
        self.assertEqual("FAIL", hil.classify("selftest full", good.replace("restoration: verified", "restoration: not verified"))[0])
        self.assertEqual("FAIL", hil.classify("selftest full", good.replace("pass=19 fail=0", "pass=18 fail=1"))[0])

    def test_selfcheck_only_allows_optional_gpio_skip(self):
        good = diagnostic("selfcheck", skip_gpio=True)
        self.assertEqual("PASS", hil.classify("selfcheck", good)[0])
        self.assertEqual("INCONCLUSIVE", hil.classify("selfcheck", good.replace("pass=8", "pass=7"))[0])
        self.assertEqual("INCONCLUSIVE", hil.classify("selfcheck", good.replace("pass=8", "pass=7").replace("skip=1", "skip=2"))[0])
        for flag in ("config-match", "thresholds-match", "temperature-trusted"):
            self.assertEqual("FAIL", hil.classify("selfcheck", good.replace(flag + "=yes", flag + "=no"))[0])

    def test_snapshot_requires_independent_trust_flags(self):
        good = diagnostic("snapshot read")
        self.assertEqual("PASS", hil.classify("snapshot read", good)[0])
        self.assertEqual("FAIL", hil.classify("snapshot read", good.replace("temperature-trusted=yes", "temperature-trusted=no"))[0])

    def test_stress_requires_exact_count_and_all_checks(self):
        good = ("Run: stopped target=3 completed=3 ok=12 fail=0 elapsed=200 ms remaining=0\n"
                "Mixed checks: completed=12 samples=3 (four checks per complete cycle; shutdown sampling may trigger OS)\n"
                + TEMPERATURE_SUMMARY +
                "Tracked health delta: ok=15 fail=0\nAdapter delta: attempts=18 ok=18 fail=0\n> ")
        self.assertEqual("PASS", hil.classify("stress_mix 3", good)[0])
        for changed in (good.replace("completed=3 ok", "completed=2 ok"),
                        good.replace("ok=12 fail=0 elapsed", "ok=11 fail=1 elapsed"),
                        good.replace("samples=3", "samples=2"), good.replace("ok=18 fail=0", "ok=17 fail=1"),
                        good.replace("Tracked health delta: ok=15", "Tracked health delta: ok=0"),
                        good.replace("attempts=18", "attempts=19"),
                        good.replace("ok=15 fail=0\nAdapter delta: attempts=18 ok=18", "ok=12 fail=0\nAdapter delta: attempts=15 ok=15")):
            self.assertNotEqual("PASS", hil.classify("stress_mix 3", changed)[0])
        self.assertEqual("INCONCLUSIVE", hil.classify("stress_mix 3", good.replace("\n> ", "\nCounter saturation reached; deltas are lower bounds.\n> "))[0])
        self.assertEqual("INCONCLUSIVE", hil.classify("stress_mix 3", good.replace("Adapter delta:", "Missing adapter:"))[0])

    def test_firmware_identity_and_explicit_expectations(self):
        firmware = hil.parse_firmware(FIRMWARE)
        self.assertEqual("esp32s3", firmware["target"])
        self.assertEqual("Arduino-ESP32", firmware["framework"])
        self.assertEqual("clean", firmware["tree"])
        for extra, changed, fails in (
                (["--expected-firmware-commit", "abcdef0"], FIRMWARE, False),
                (["--expected-framework", "Arduino-ESP32", "--expected-target", "esp32s3", "--require-clean-firmware"], FIRMWARE, False),
                (["--expected-firmware-commit", "fffffff"], FIRMWARE, True),
                (["--expected-firmware-commit", "abcdef0"], FIRMWARE.replace(firmware["commit"], "unknown"), True),
                (["--require-clean-firmware"], FIRMWARE.replace("tree=clean", "tree=dirty"), True),
                (["--require-clean-firmware"], FIRMWARE.replace("tree=clean", "tree=unknown"), True),
                (["--expected-framework", "native-esp-idf"], FIRMWARE, True),
                (["--expected-target", "esp32s2"], FIRMWARE, True)):
            with self.subTest(extra=extra, changed=changed):
                runner = hil.Runner(mock.Mock(), hil.parse_args(["--dry-run", "--model", "tmp102", "--address", "0x48", *extra]))
                runner.session.transcript_error = None
                responses = {"version": changed, "job": "Operation: active=no token=0", "stats": "No watch/stress run statistics.",
                             "job auto": "Job auto: on", "settings": SETTINGS, "health": HEALTH, "verify": "[I] OK",
                             "console": CONSOLE, "bus": BUS}
                runner.command = mock.Mock(side_effect=lambda command: hil.Result(command, command, "PASS", "fixture", responses[command]))
                with contextlib.redirect_stdout(io.StringIO()):
                    if fails:
                        with self.assertRaises(hil.CheckError):
                            runner.preflight()
                    else:
                        runner.preflight()
                self.assertEqual("not verified" if fails else "matched requested fields", runner.firmware_verification)
        with self.assertRaises(hil.CheckError):
            hil.parse_firmware(FIRMWARE.replace("Firmware:", "missing:"))

    def test_reboot_cannot_pass_or_be_an_expected_error(self):
        restart = "Diagnostic CLI; application owns the bus. Type 'help' for commands.\n"
        result = hil.SerialSession(FakeSerial((restart + "[E] INVALID_PARAM detail=0\n> ").encode()), 1, 5, Clock()).command("wreg 0 1")
        self.assertEqual("INCONCLUSIVE", result.outcome)
        self.assertFalse(hil.expected_error(result, {"INVALID_PARAM"}))
        for marker in (restart, "ESP-ROM:esp32s3\n", "rst:0xc (SW_CPU_RESET)\n", "Rebooting...\n",
                       "Guru Meditation Error\n", "assert failed: cli.cpp:99\n", "Backtrace: 0x40001234\n",
                       "Brownout detector was triggered\n", "Task watchdog got triggered\n"):
            self.assertEqual("INCONCLUSIVE", hil.classify("probe", marker + "[I] OK detail=0\n> ")[0])

    def test_reboot_prevents_automatic_restore_to_unchecked_target(self):
        session = mock.Mock()
        session.restarted = True
        runner = hil.Runner(session, hil.parse_args(["--dry-run"]))
        runner.baseline, runner.mutated = hil.parse_settings(SETTINGS), True
        runner.command = mock.Mock()
        with contextlib.redirect_stdout(io.StringIO()):
            runner.restore()
        runner.command.assert_not_called()
        self.assertEqual("not verified", runner.restoration)
        self.assertEqual("FAIL", runner.results[-1].outcome)
        runner.mutated, runner.background_started = False, True
        runner.restoration = "not required"
        with contextlib.redirect_stdout(io.StringIO()):
            runner.restore()
        runner.command.assert_not_called()
        self.assertEqual("not required", runner.restoration)
        self.assertEqual("INCONCLUSIVE", runner.results[-1].outcome)

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
                        for clear in (0x10, 0x40, 0x80):
                            wrong = output.replace(f"CONFIG=0x{word:04X}", f"CONFIG=0x{word & ~clear:04X}")
                            self.assertEqual("FAIL", hil.check_comparator(wrong, polarity, faults, active)[0])

    def test_console_integrity_rejects_new_or_saturated_loss(self):
        historical = CONSOLE.replace("dropped=0", "dropped=12")
        baseline = hil.parse_console(historical)
        self.assertEqual("PASS", hil.check_console(historical, baseline)[0])
        self.assertEqual("FAIL", hil.check_console(historical.replace("dropped=12", "dropped=13"), baseline)[0])
        self.assertEqual("FAIL", hil.check_console(historical.replace("truncated=0", "truncated=1"), baseline)[0])
        saturated = CONSOLE.replace("dropped=0", "dropped=4294967295")
        self.assertEqual("INCONCLUSIVE", hil.check_console(saturated, hil.parse_console(saturated))[0])

    def test_long_soak_aggregates_without_per_pass_result_growth(self):
        session = SoakSession()
        args = hil.parse_args(["--dry-run", "--soak-seconds", "12000", "--soak-batch-count", "2"])
        runner = hil.Runner(session, args, clock=lambda: session.now)
        runner.console_baseline = hil.parse_console(CONSOLE)
        with contextlib.redirect_stdout(io.StringIO()):
            runner.soak()
        summary = runner.soak_summary
        self.assertEqual("PASS", summary["outcome"])
        self.assertGreaterEqual(summary["elapsed_seconds"], 12000)
        self.assertEqual(12000, summary["batches"])
        self.assertEqual(24000, summary["completed_cycles"])
        self.assertEqual(48000, summary["commands"])
        self.assertEqual(48000, summary["outcomes"]["PASS"])
        self.assertEqual(0.25, summary["latency_mean_seconds"])
        self.assertLess(len(runner.results), 10)
        self.assertGreater(session.logged_bytes, 100000)
        self.assertTrue(runner.original_verbose)

    def test_soak_stops_on_failure_or_output_loss_without_recovery(self):
        for session in (SoakSession(failure_batch=3), SoakSession(console_loss_batch=3)):
            runner = hil.Runner(session, hil.parse_args(["--dry-run", "--soak-seconds", "100"]), clock=lambda: session.now)
            runner.console_baseline = hil.parse_console(CONSOLE)
            with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(hil.CheckError):
                runner.soak()
            self.assertEqual("FAIL", runner.soak_summary["outcome"])
            self.assertEqual(2, runner.soak_summary["batches"])
            self.assertEqual(3, session.batches)
            self.assertEqual(1, runner.soak_summary["outcomes"]["FAIL"])
            self.assertLess(runner.soak_summary["elapsed_seconds"], 100)

    def test_soak_rejects_hidden_temperature_excursions_and_stale_batch_evidence(self):
        for name in ("temperature_failure_batch", "stale_sample_batch", "stalled_health_batch"):
            with self.subTest(fault=name):
                session = SoakSession(**{name: 3})
                runner = hil.Runner(session, hil.parse_args(["--dry-run", "--soak-seconds", "100"]), clock=lambda: session.now)
                runner.console_baseline = hil.parse_console(CONSOLE)
                with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(hil.CheckError):
                    runner.soak()
                self.assertNotEqual("PASS", runner.soak_summary["outcome"])
                self.assertEqual(2, runner.soak_summary["batches"])
                self.assertEqual(3, session.batches)
                self.assertLess(runner.soak_summary["elapsed_seconds"], 100)

    def test_interrupted_soak_does_not_pass_a_partial_duration_or_batch(self):
        session = SoakSession(interrupt_batch=3)
        runner = hil.Runner(session, hil.parse_args(["--dry-run", "--soak-seconds", "100"]), clock=lambda: session.now)
        runner.console_baseline = hil.parse_console(CONSOLE)
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(KeyboardInterrupt):
            runner.soak()
        self.assertEqual("INCONCLUSIVE", runner.soak_summary["outcome"])
        self.assertEqual(2, runner.soak_summary["batches"])
        self.assertLess(runner.soak_summary["elapsed_seconds"], 100)

    def test_last_soak_evidence_write_failure_cannot_leave_a_pass_summary(self):
        for marker in (b"soak validated: console:", b"=== Soak:", b"=== check: duration soak"):
            with self.subTest(marker=marker):
                session = SoakSession()
                original_log = session.log
                def failed_log(data):
                    original_log(data)
                    if marker in data:
                        session.transcript_error = "disk full at final batch evidence"
                session.log = failed_log
                args = hil.parse_args(["--dry-run", "--soak-seconds", "1", "--soak-progress-seconds", "0.5"])
                runner = hil.Runner(session, args, clock=lambda: session.now)
                runner.console_baseline = hil.parse_console(CONSOLE)
                with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(hil.CheckError):
                    runner.soak()
                self.assertEqual("INCONCLUSIVE", runner.soak_summary["outcome"])
                self.assertIn("disk full", runner.soak_summary["reason"])
                self.assertEqual("INCONCLUSIVE", runner.results[-1].outcome)

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
                          ["--dry-run", "--model", "tmp112d", "--address", "0x48"],
                          ["--dry-run", "--expected-firmware-commit", "unknown"],
                          ["--dry-run", "--expected-target", "unknown"],
                          ["--dry-run", "--soak-seconds", "nan"], ["--dry-run", "--soak-seconds", "604801"],
                          ["--dry-run", "--soak-batch-count", "1001"], ["--dry-run", "--soak-batch-count", "0"]):
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

    def test_baseline_checkpoint_is_readable_before_first_suite_mutation(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.ExitStack() as stack:
            path = Path(directory) / "report.json"
            args = hil.parse_args(["--dry-run", "--suite", "oneshot", "--report", str(path)])
            evidence = hil.EvidenceFiles(path, args)
            stack.callback(evidence.close)
            session = hil.SerialSession(FakeSerial(), 1, 1, transcript_file=evidence.transcript)
            runner = hil.Runner(session, args, evidence=evidence)
            def preflight():
                runner.baseline = hil.parse_settings(SETTINGS)
                runner.firmware = hil.parse_firmware(FIRMWARE)
            def before_mutation():
                checkpoint = json.loads(path.read_text())
                self.assertEqual("INCONCLUSIVE", checkpoint["outcome"])
                self.assertEqual(runner.baseline, checkpoint["baseline"])
                self.assertEqual(runner.firmware, checkpoint["firmware"])
                runner.mutated = True
            runner.preflight = preflight
            runner.oneshot = mock.Mock(side_effect=before_mutation)
            runner.healthy = mock.Mock()
            runner.verify_console = mock.Mock()
            runner.restore = mock.Mock()
            try:
                with contextlib.redirect_stdout(io.StringIO()):
                    runner.run()
                runner.oneshot.assert_called_once()
                runner.restore.assert_called_once()
            finally:
                evidence.close()

    def test_failed_checkpoint_prevents_all_suite_mutations(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.ExitStack() as stack:
            path = Path(directory) / "report.json"
            args = hil.parse_args(["--dry-run", "--suite", "oneshot", "--report", str(path)])
            evidence = hil.EvidenceFiles(path, args)
            stack.callback(evidence.close)
            runner = hil.Runner(hil.SerialSession(FakeSerial(), 1, 1), args, evidence=evidence)
            runner.preflight = mock.Mock(side_effect=lambda: setattr(runner, "baseline", hil.parse_settings(SETTINGS)))
            runner.oneshot = mock.Mock()
            try:
                with mock.patch.object(evidence, "write_json", side_effect=OSError("checkpoint disk full")), contextlib.redirect_stdout(io.StringIO()):
                    runner.run()
                runner.oneshot.assert_not_called()
                self.assertFalse(runner.mutated)
                self.assertTrue(any(result.outcome == "INCONCLUSIVE" for result in runner.results))
                self.assertEqual("INCONCLUSIVE", json.loads(path.read_text())["outcome"])
            finally:
                evidence.close()

    def test_pass_checks_cannot_certify_unfinished_suites_or_restoration(self):
        for state in ("suite not run", "suite inconclusive", "profile not restored", "presentation not restored"):
            with self.subTest(state=state), tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
                runner = hil.Runner(hil.SerialSession(FakeSerial(), 1, 1), hil.parse_args(["--dry-run"]))
                runner.results = [hil.Result("one completed check", "", "PASS", "ok")]
                for suite in runner.suites:
                    suite["outcome"] = "PASS"
                if state == "suite not run":
                    runner.suites[0]["outcome"] = "NOT_RUN"
                elif state == "suite inconclusive":
                    runner.suites[0]["outcome"] = "INCONCLUSIVE"
                elif state == "profile not restored":
                    runner.mutated = True
                    runner.restoration = "not verified"
                else:
                    runner.original_verbose = True
                    runner.presentation_restoration = "not verified"
                path = Path(directory) / "report.json"
                self.assertNotEqual(0, hil.write_report(path, runner))
                self.assertNotEqual("PASS", json.loads(path.read_text())["outcome"])

    def test_report_keeps_start_and_end_host_provenance_separate_from_firmware(self):
        start = dict(commit="a" * 40, dirty=False, runner_sha256="1" * 64)
        for changed in (False, True):
            end = dict(commit="b" * 40, dirty=True, runner_sha256="2" * 64) if changed else dict(start)
            with self.subTest(changed=changed), mock.patch.object(hil, "host_source", side_effect=[start, end]):
                runner = hil.Runner(hil.SerialSession(FakeSerial(), 1, 1), hil.parse_args(["--dry-run"]))
                runner.firmware = hil.parse_firmware(FIRMWARE)
                runner.results = [hil.Result("completed check", "", "PASS", "ok")]
                for suite in runner.suites:
                    suite["outcome"] = "PASS"
                report = hil.build_report(runner)
            self.assertEqual(start["commit"], report["git_commit"])
            self.assertEqual(start["dirty"], report["git_dirty"])
            self.assertEqual(start, report["host_source"]["start"])
            self.assertEqual(end, report["host_source"]["end"])
            self.assertEqual(changed, report["host_source"]["changed"])
            self.assertEqual(runner.firmware, report["firmware"])
            self.assertNotEqual(report["firmware"]["commit"], report["git_commit"])

    @unittest.skipUnless(CLI_FIXTURE, "pass --cli-fixture PATH to validate actual shared CLI output")
    def test_two_campaigns_share_running_cli_without_reset_and_restore_entry_state(self):
        serial = ProcessSerial(CLI_FIXTURE)
        try:
            initial = hil.SerialSession(serial, 3, 8)
            initial.capture_boot(0.2)
            self.assertEqual("PASS", initial.synchronize().outcome)
            self.assertEqual("PASS", initial.command("verbose 0").outcome)
            baseline = hil.parse_settings(initial.command("settings").output)
            commits, operation_tokens = [], []
            with tempfile.TemporaryDirectory() as directory:
                for campaign in range(2):
                    with self.subTest(campaign=campaign), contextlib.ExitStack() as stack:
                        path = Path(directory) / f"campaign-{campaign}.json"
                        args = hil.parse_args(["--port", "native-model", "--model", "tmp102", "--address", "0x48",
                                               "--suite", "owner", "--suite", "stress", "--suite", "smoke",
                                               "--stress-count", "2", "--silence-seconds", "0.01", "--report", str(path)])
                        evidence = hil.EvidenceFiles(path, args)
                        stack.callback(evidence.close)
                        session = hil.SerialSession(serial, 3, 8, transcript_file=evidence.transcript)
                        runner = hil.Runner(session, args, evidence=evidence)
                        try:
                            self.assertEqual("PASS", session.synchronize().outcome)
                            with contextlib.redirect_stdout(io.StringIO()):
                                runner.run()
                                exit_code = hil.write_report(path, runner, evidence)
                            report = json.loads(path.read_text())
                            failures = [(result["check"], result["reason"]) for result in report["results"]
                                        if result["outcome"] in {"FAIL", "INCONCLUSIVE"}]
                            self.assertEqual(0, exit_code, failures)
                            self.assertEqual("PASS", report["outcome"])
                            self.assertFalse(report["hardware_run"])
                            self.assertEqual(baseline, report["baseline"])
                            self.assertEqual("verified", report["restoration"])
                            self.assertEqual("verified", report["presentation_restoration"])
                            self.assertEqual(baseline, hil.parse_settings(session.command("settings").output))
                            self.assertIn("Verbose: off", session.command("verbose").output)
                            self.assertIn("Job auto: on", session.command("job auto").output)
                            commits.append(report["firmware"]["commit"])
                            operation_tokens.append([int(value) for value in re.findall(
                                r"Operation started: token=(\d+)", path.with_suffix(".txt").read_text())])
                        finally:
                            evidence.close()
            self.assertEqual(2, len(commits), "both campaigns must finish before comparing their identities")
            self.assertEqual(commits[0], commits[1])
            self.assertTrue(operation_tokens[0] and operation_tokens[1])
            self.assertGreater(min(operation_tokens[1]), max(operation_tokens[0]))
            self.assertIsNone(serial.process.poll())
        finally:
            serial.close()

    @unittest.skipUnless(CLI_FIXTURE, "pass --cli-fixture PATH to validate actual shared CLI output")
    def test_all_automated_non_stimulus_suites_against_real_cli_model(self):
        serial = ProcessSerial(CLI_FIXTURE)
        args = hil.parse_args(["--port", "native-model", "--model", "tmp102", "--address", "0x48",
                               "--suite", "smoke", "--suite", "config", "--suite", "oneshot", "--suite", "stress",
                               "--suite", "negative", "--suite", "owner", "--suite", "registers", "--suite", "detection",
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
            self.assertEqual("not requested", runner.firmware_verification)
        finally:
            serial.close()

    @unittest.skipUnless(CLI_FIXTURE, "pass --cli-fixture PATH to validate actual shared CLI output")
    def test_quiet_stress_and_soak_restore_original_presentation(self):
        for original_verbose in (True, False):
            with self.subTest(original_verbose=original_verbose):
                serial = ProcessSerial(CLI_FIXTURE)
                session = hil.SerialSession(serial, 3, 8)
                args = hil.parse_args(["--port", "native-model", "--model", "tmp102", "--address", "0x48",
                                       "--suite", "stress", "--stress-count", "3", "--silence-seconds", "0.01",
                                       "--soak-seconds", "0.03", "--soak-batch-count", "2"])
                runner = hil.Runner(session, args)
                try:
                    session.capture_boot(0.2)
                    self.assertEqual("PASS", session.command(f"verbose {int(original_verbose)}").outcome)
                    with contextlib.redirect_stdout(io.StringIO()):
                        runner.run()
                    failures = [result for result in runner.results if result.outcome in {"FAIL", "INCONCLUSIVE"}]
                    self.assertEqual([], failures, "\n\n".join(session.transcript))
                    self.assertEqual("PASS", runner.soak_summary["outcome"])
                    self.assertGreaterEqual(runner.soak_summary["elapsed_seconds"], 0.03)
                    self.assertGreater(runner.soak_summary["batches"], 0)
                    self.assertEqual("not required", runner.restoration)  # Continuous reads did not change config.
                    self.assertEqual("verified", runner.presentation_restoration)
                    self.assertEqual(original_verbose, runner.original_verbose)
                    self.assertIn("Verbose: " + ("on" if original_verbose else "off"), session.command("verbose").output)
                    stress_results = [result for result in runner.results if result.command.startswith(("stress ", "stress_mix "))]
                    self.assertTrue(stress_results)
                    self.assertFalse(any("Temperature:" in result.output for result in stress_results))
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
    def test_new_suites_restore_extended_only_entry_profile(self):
        serial = ProcessSerial(CLI_FIXTURE)
        session = hil.SerialSession(serial, 3, 8)
        args = hil.parse_args(["--port", "native-model", "--model", "tmp102", "--address", "0x48",
                               "--suite", "registers", "--suite", "owner", "--suite", "negative", "--suite", "oneshot"])
        runner = hil.Runner(session, args)
        try:
            session.capture_boot(0.2)
            for command in ("extended 1", "threshold 150 200"):
                self.assertEqual("PASS", session.command(command).outcome)
            with contextlib.redirect_stdout(io.StringIO()):
                runner.run()
            failures = [result for result in runner.results if result.outcome in {"FAIL", "INCONCLUSIVE"}]
            self.assertEqual([], failures, "\n\n".join(session.transcript))
            self.assertEqual("verified", runner.restoration)
            self.assertEqual(150.0, runner.baseline["low"])
            self.assertEqual(runner.baseline, hil.parse_settings(session.command("settings").output))
        finally:
            serial.close()

    @unittest.skipUnless(CLI_FIXTURE, "pass --cli-fixture PATH to validate actual shared CLI output")
    def test_failed_owner_suite_restores_automatic_polling_and_baseline(self):
        serial = ProcessSerial(CLI_FIXTURE)
        session = hil.SerialSession(serial, 3, 8)
        args = hil.parse_args(["--port", "native-model", "--model", "tmp102", "--address", "0x48", "--suite", "owner"])
        runner = hil.Runner(session, args)
        def interrupted_owner():
            runner.mutated = True
            runner.command("job auto 0")
            runner.command("job start shutdown 2147483647")
            raise hil.CheckError("injected suite interruption with manual job active")
        runner.owner = interrupted_owner
        try:
            session.capture_boot(0.2)
            with contextlib.redirect_stdout(io.StringIO()):
                runner.run()
            self.assertEqual("verified", runner.restoration, "\n\n".join(session.transcript))
            self.assertIn("Job auto: on", session.command("job auto").output)
            self.assertEqual(runner.baseline, hil.parse_settings(session.command("settings").output))
            self.assertTrue(any(result.outcome == "INCONCLUSIVE" for result in runner.results))
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
