#!/usr/bin/env python3
"""Bounded serial HIL suites for the shared Arduino/native ESP-IDF TMP1x2 CLI.

Only pyserial is optional; dry-run, parsing and unit tests use the standard library.
Exit 0: selected automated checks passed; 1: failed check; 2: inconclusive/setup error.
This runner never flashes firmware, changes model/address, scans, or resets the bus.
"""
from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import datetime as dt
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
from typing import Callable

ROOT = Path(__file__).resolve().parents[1]
MAX_RESPONSE_BYTES = 8 * 1024 * 1024
MAX_RETAINED_CHARS = 16 * 1024 * 1024
MAX_RESULTS = 10000
ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
PROMPT = re.compile(r"(?:^|\n)(?:> )+$")
ERROR = re.compile(r"\[E\]\s+(\w+)")
DIAGNOSTIC = re.compile(r"Diagnostic summary: (complete|cancelled|active) pass=(\d+) fail=(\d+) skip=(\d+)")
RUN = re.compile(r"Run: (active|stopped) target=(\d+) completed=(\d+) ok=(\d+) fail=(\d+) elapsed=(\d+) ms remaining=(\d+)")
SAMPLE = re.compile(r"Temperature: ([+-]?\d+\.\d+) C raw=0x([0-9A-Fa-f]{4}) counts=(-?\d+) extended=(yes|no) timestamp=(\d+) ms clock=(known|unknown) one-shot=(confirmed|unproven)")
TRANSPORT_ERRORS = {"I2C_ERROR", "I2C_NACK_ADDR", "I2C_NACK_DATA", "I2C_TIMEOUT", "I2C_BUS", "DEVICE_NOT_FOUND", "TIMEOUT"}
MUTATING = {"config", "oneshot", "alert", "disconnect"}
SYNC_PATTERNS = {
    "version": r"TMP1x2 diagnostic CLI.*\nLibrary: \S+",
    "job": r"Operation: active=(?:yes|no) token=\d+|Diagnostic: [^\n]+ active=(?:yes|no)",
    "stats": r"No watch/stress run statistics\.|Run: (?:active|stopped)",
    "settings": r"Desired: model=\S+ address=0x[0-9A-Fa-f]+",
    "health": r"Health: state=\w+ online=(?:yes|no)",
    "probe": r"\[I\] OK\b",
    "verify": r"\[I\] OK\b",
    "read": SAMPLE.pattern,
    "sample": SAMPLE.pattern,
    "tryread": SAMPLE.pattern,
    "xfer_stats": r"Bus: attempts=\d+ ok=\d+ fail=\d+",
    "config": r"CONFIG=0x[0-9A-Fa-f]{4} valid=yes OS=[01] EM=[01] AL=[01] ALERT-active=(?:yes|no)",
    "alertpin": r"Physical ALERT: (?:active|inactive) pin=\d+",
    "stop": r"(?:stopped:|Operation result:|Diagnostic summary:)",
}


def clean(text: str) -> str:
    return ANSI.sub("", text).replace("\r", "")


@dataclass
class Result:
    check: str
    command: str
    outcome: str
    reason: str
    output: str = ""
    duration_s: float = 0.0


class CheckError(RuntimeError):
    pass


class EvidenceFiles:
    """Reserve output paths before opening hardware and preserve live transcripts."""
    def __init__(self, path: Path, args):
        self.path = path
        self.transcript = None
        self.lock = path.with_suffix(".json.lock")
        self.owns_lock = False
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            with self.lock.open("x", encoding="utf-8"):
                self.owns_lock = True
            if path.exists() or path.with_suffix(".txt").exists():
                raise FileExistsError("refusing to overwrite existing HIL evidence")
            # An interrupted run leaves a valid INCONCLUSIVE document, never PASS.
            with path.open("x", encoding="utf-8") as output:
                json.dump(dict(schema_version=1, outcome="INCONCLUSIVE", hardware_run=None,
                               reason="run has not completed", metadata=vars(args)), output, indent=2)
                output.write("\n")
            self.transcript = path.with_suffix(".txt").open("xb")
        except (OSError, ValueError):
            self.close()
            raise

    def write_json(self, report: dict):
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=self.path.parent,
                                             prefix=self.path.name + ".", suffix=".tmp", delete=False) as output:
                temporary = Path(output.name)
                json.dump(report, output, indent=2, allow_nan=False)
                output.write("\n")
            os.replace(temporary, self.path)
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)

    def close_transcript(self):
        stream = self.transcript
        self.transcript = None
        if stream is not None:
            stream.close()

    def close(self):
        try:
            self.close_transcript()
        finally:
            if self.owns_lock:
                self.lock.unlink(missing_ok=True)
                self.owns_lock = False


def is_operation(command: str) -> bool:
    return command in {"measure", "recover", "shutdown"} or command.split()[0] in {
        "mode", "rate", "extended", "threshold", "alert", "polarity", "faults"}


def operation_status(output: str):
    started = re.search(r"Operation started: token=(\d+) kind=(\w+)", output)
    if not started:
        return None
    return re.search(r"Operation result: token=" + started[1] + r" kind=" + started[2] +
                     r"\b[^\n]*\n\[([IE])\] (\w+)\b", output)


def response_complete(command: str, output: str) -> bool:
    """A start prompt is not completion of a cooperative operation or diagnostic."""
    output = clean(output)
    if not PROMPT.search(output):
        return False
    if is_operation(command):
        started = re.search(r"Operation started: token=(\d+) kind=(\w+)", output)
        if not started:
            return bool(ERROR.search(output))
        terminal = operation_status(output)
        return terminal is not None and terminal[2] != "IN_PROGRESS"
    if command in {"selfcheck", "selftest full", "settings read", "snapshot read"}:
        return bool(DIAGNOSTIC.search(output) or ERROR.search(output))
    if command.startswith(("stress ", "stress_mix ")):
        return bool(re.search(r"Run: stopped ", output) or ERROR.search(output))
    if command == "stop":
        # First cancellation of a full test schedules restoration. Wait for it;
        # sending a second stop would cancel the restoration itself.
        if "scheduling restoration" in output:
            return bool(DIAGNOSTIC.search(output))
    # Synchronous cancellation prints two prompts (finishOperation + feed).
    # A prompt can arrive in a separate USB packet after the previous response;
    # ignore it until this command's payload is present.
    return bool(ERROR.search(output) or re.search(SYNC_PATTERNS.get(command, r"(?!)"), output))


def classify(command: str, output: str, completed: bool = True) -> tuple[str, str]:
    output = clean(output)
    if not completed:
        return "INCONCLUSIVE", "deadline expired before a complete response"
    errors = ERROR.findall(output)
    expected_cancel = command == "stop" and errors and all(error == "CANCELLED" for error in errors) and "Operation result:" in output
    if errors and not expected_cancel:
        return "FAIL", errors[0]
    if "[FAIL]" in output:
        return "FAIL", "firmware diagnostic reported a failed check"
    if is_operation(command):
        terminal = operation_status(output)
        if not response_complete(command, output) or terminal is None or terminal.groups() != ("I", "OK"):
            return "INCONCLUSIVE", "missing matching successful terminal operation result"
    elif command in {"selfcheck", "selftest full", "settings read", "snapshot read"}:
        summary = DIAGNOSTIC.search(output)
        if not summary or summary[1] != "complete":
            return "INCONCLUSIVE", "diagnostic did not complete"
        passed, failed, skipped = map(int, summary.groups()[1:])
        if failed:
            return "FAIL", f"diagnostic failed {failed} checks"
        expected = {"selfcheck": 9, "selftest full": 19, "settings read": 3, "snapshot read": 1}[command]
        if passed + skipped != expected or skipped > (1 if command == "selfcheck" else 0):
            return "INCONCLUSIVE", "missing checks or unexpected skipped checks"
        if command == "selftest full" and "Baseline restoration: verified\n" not in output:
            return "FAIL", "captured baseline restoration was not verified"
        if command == "snapshot read" and "config-match=yes thresholds-match=yes temperature-trusted=yes" not in output:
            return "FAIL", "snapshot configuration, thresholds or temperature are untrusted"
    elif command.startswith(("stress ", "stress_mix ")):
        run = RUN.search(output)
        if not run:
            return "INCONCLUSIVE", "missing finite-run summary"
        target, completed_count, ok, failed, _, remaining = map(int, run.groups()[1:])
        expected = int(command.split()[1])
        if failed:
            return "FAIL", f"run failed {failed} checks"
        if run[1] != "stopped" or target != expected or completed_count != expected or remaining != 0:
            return "INCONCLUSIVE", "run stopped before the requested count"
        if ok != expected * (4 if command.startswith("stress_mix") else 1):
            return "FAIL", "successful check count does not match completed cycles"
        if command.startswith("stress_mix") and f"Mixed checks: completed={4 * expected} samples={expected} " not in output:
            return "FAIL", "mixed run is missing expected samples"
        if "Counter saturation reached" in output:
            return "INCONCLUSIVE", "saturated transport counters cannot establish exact run accounting"
        if not re.search(r"Tracked health delta: ok=\d+ fail=\d+", output) or not re.search(r"Adapter delta: attempts=\d+ ok=\d+ fail=\d+", output):
            return "INCONCLUSIVE", "missing tracked health or adapter transfer deltas"
        if re.search(r"(?:Tracked health|Adapter) delta:[^\n]*fail=[1-9]", output):
            return "FAIL", "run recorded transport failures"
    else:
        if command not in SYNC_PATTERNS or not re.search(SYNC_PATTERNS[command], output):
            return "INCONCLUSIVE", "missing command-specific response"
        if command == "stop":
            summary = DIAGNOSTIC.search(output)
            if summary and int(summary[3]):
                return "FAIL", "cancelled diagnostic recorded failed checks"
            if ("scheduling restoration" in output or "Baseline restoration:" in output) and "Baseline restoration: verified\n" not in output:
                return "FAIL", "cancelled full test did not verify baseline restoration"
    return "PASS", "complete response with required checks"


def expected_error(result: Result, allowed: set[str]) -> bool:
    """An expected physical error must not hide another error or broken framing."""
    output = clean(result.output)
    errors = ERROR.findall(output)
    return (result.outcome == "FAIL" and bool(errors) and set(errors) <= allowed and
            "[FAIL]" not in output and response_complete(result.command, output))


def parse_settings(output: str) -> dict:
    output = clean(output)
    desired = re.search(r"Desired: model=(\S+) address=(0x[0-9A-Fa-f]+) timeout=(\d+) ms mode=(continuous|shutdown) rate=(0\.25|1|4|8)(?: Hz)? extended=(yes|no)", output)
    thresholds = re.search(r"Thresholds: low=(-?\d+\.\d+) C high=(-?\d+\.\d+) C alert=(comparator|interrupt) polarity=(low|high) faults=([1246]) offline-threshold=(\d+)", output)
    pin = re.search(r"physical ALERT=(available|not present) pin=(-?\d+)", output)
    if not desired or not thresholds or not pin:
        raise CheckError("cannot capture a complete desired profile from settings")
    profile = dict(model=desired[1], address=int(desired[2], 16), timeout_ms=int(desired[3]),
                mode=desired[4], rate=desired[5], extended=desired[6] == "yes",
                low=float(thresholds[1]), high=float(thresholds[2]), alert=thresholds[3],
                polarity=thresholds[4], faults=int(thresholds[5]), offline_threshold=int(thresholds[6]),
                alert_pin=int(pin[2]), alert_output=pin[1] == "available")
    minimum, maximum = (-256.0, 255.9375) if profile["extended"] else (-128.0, 127.9375)
    if not minimum <= profile["low"] <= profile["high"] <= maximum:
        raise CheckError("captured thresholds are not finite, ordered and representable")
    return profile


def restore_commands(profile: dict) -> list[str]:
    # Extended first allows restoration of thresholds outside the normal format.
    # Recover repairs dirty state and pending format evidence before typed edits.
    return ["recover", "extended 1", f"threshold {profile['low']:.4f} {profile['high']:.4f}",
            f"rate {profile['rate']}", f"alert {profile['alert']}", f"polarity {profile['polarity']}",
            f"faults {profile['faults']}", f"extended {int(profile['extended'])}", f"mode {profile['mode']}", "verify"]


def parse_health(output: str) -> dict:
    output = clean(output)
    health = re.search(r"Health: state=(\w+) online=(yes|no) consec=(\d+) ok=(\d+) fail=(\d+)", output)
    trust = re.search(r"Trust: bound=(yes|no) initialized=(yes|no) dirty=(yes|no)", output)
    conversion = re.search(r"Conversion: pending=(yes|no) ready=(yes|no)", output)
    if not health or not trust or not conversion:
        raise CheckError("missing complete health/trust/conversion report")
    return dict(state=health[1], online=health[2] == "yes", consecutive=int(health[3]),
                ok=int(health[4]), fail=int(health[5]), bound=trust[1] == "yes",
                initialized=trust[2] == "yes", dirty=trust[3] == "yes", pending=conversion[1] == "yes")


def check_sample(output: str, minimum: float, maximum: float, extended=None, fresh=False) -> tuple[str, str]:
    match = SAMPLE.search(clean(output))
    if not match:
        return "INCONCLUSIVE", "missing full temperature sample"
    celsius, raw, counts = float(match[1]), int(match[2], 16), int(match[3])
    is_extended = match[4] == "yes"
    width, shift = (13, 3) if is_extended else (12, 4)
    expected = raw >> shift
    if expected & (1 << (width - 1)):
        expected -= 1 << width
    if bool(raw & 1) != is_extended or raw & (6 if is_extended else 15):
        return "FAIL", "TEMP format marker or reserved bits are inconsistent"
    if counts != expected or abs(celsius - counts * 0.0625) > 0.000051:
        return "FAIL", "raw word, signed counts and Celsius disagree"
    if not minimum <= celsius <= maximum:
        return "FAIL", f"{celsius:.4f} C outside operator window [{minimum}, {maximum}]"
    if extended is not None and extended != is_extended:
        return "FAIL", "sample format does not match requested format"
    if match[6] != "known" or (fresh and match[7] != "confirmed"):
        return "FAIL", "missing acquisition clock or confirmed one-shot provenance"
    return "PASS", "sample format, signed conversion and operator temperature window verified"


def check_format_continuity(temperatures: list[float], tolerance: float) -> tuple[str, str]:
    if len(temperatures) != 3:
        return "INCONCLUSIVE", "normal/extended/normal sequence did not finish"
    spread = max(temperatures) - min(temperatures)
    if spread > tolerance:
        return "FAIL", f"normal/extended/normal spread {spread:.4f} C exceeds stable-fixture allowance {tolerance:.4f} C"
    return "PASS", f"normal/extended/normal confirmed samples agree within {tolerance:.4f} C"


def check_comparator(output: str, polarity: str, faults: int, active: bool) -> tuple[str, str]:
    output = clean(output)
    raw = re.search(r"CONFIG=0x([0-9A-Fa-f]{4})", output)
    decoded = re.search(r"AL=([01]) ALERT-active=(yes|no)", output)
    if not raw or not decoded:
        return "INCONCLUSIVE", "missing raw and decoded comparator report"
    word = int(raw[1], 16)
    queue_code = {1: 0, 2: 1, 4: 2, 6: 3}[faults]
    if bool(word & 0x0400) != (polarity == "high") or (word >> 11) & 3 != queue_code or word & 0x0300:
        return "FAIL", "raw CONFIG does not select requested comparator, polarity, fault queue and continuous mode"
    raw_al = bool(word & 0x0020)
    if raw_al != (active == (polarity == "high")) or raw_al != bool(int(decoded[1])) or (decoded[2] == "yes") != active:
        return "FAIL", "raw AL and decoded state do not match the applied threshold stimulus"
    return "PASS", "raw AL and decoded comparator state match the stable threshold stimulus"


class SerialSession:
    def __init__(self, serial, command_timeout: float, long_timeout: float,
                 clock: Callable = time.monotonic, transcript_file=None):
        self.serial = serial
        self.command_timeout = command_timeout
        self.long_timeout = long_timeout
        self.clock = clock
        self.transcript: list[str] = []
        self.transcript_file = transcript_file
        self.transcript_error = None
        self.retained_chars = 0

    def log(self, data: bytes):
        if self.transcript_file is not None and self.transcript_error is None:
            try:
                self.transcript_file.write(data)
                self.transcript_file.flush()
            except OSError as exc:
                # Keep cleanup possible even if the evidence disk becomes full.
                self.transcript_error = str(exc)

    def retain(self, text: str):
        self.transcript.append(text)
        self.retained_chars += len(text)
        while self.retained_chars > MAX_RETAINED_CHARS and len(self.transcript) > 1:
            self.retained_chars -= len(self.transcript.pop(0))

    def read(self, deadline: float):
        remaining = max(0.0, deadline - self.clock())
        if not remaining:
            return b""
        if hasattr(self.serial, "timeout"):
            self.serial.timeout = min(0.05, remaining)
        return self.serial.read(max(1, min(getattr(self.serial, "in_waiting", 0), 4096)))

    def capture_boot(self, seconds: float) -> str:
        end = self.clock() + seconds
        chunks = bytearray()
        self.log(b"=== startup ===\n")
        try:
            while self.clock() < end:
                data = self.read(end)
                if data:
                    self.log(data)
                    if len(chunks) + len(data) > MAX_RESPONSE_BYTES:
                        raise OSError("startup output exceeded 8 MiB safety bound")
                    chunks.extend(data)
        finally:
            output = chunks.decode("utf-8", errors="replace")
            self.retain("=== startup ===\n" + output)
        return output

    def command(self, command: str, timeout_s=None) -> Result:
        started = self.clock()
        timeout = self.long_timeout if command == "selftest full" or command.startswith(("stress ", "stress_mix ")) or command == "stop" else self.command_timeout
        if timeout_s is not None:
            timeout = min(timeout, max(0.0, timeout_s))
        deadline = started + timeout
        chunks = bytearray()
        complete = False
        self.log(f"\n=== {command} ===\n".encode("utf-8"))
        try:
            if timeout <= 0:
                raise OSError("command deadline already expired; command was not sent")
            payload = (command + "\n").encode("ascii")
            if hasattr(self.serial, "write_timeout"):
                self.serial.write_timeout = min(self.command_timeout, timeout)
            if self.serial.write(payload) != len(payload):
                raise OSError("partial serial command write")
            # No serial.flush(): some host drivers have an unbounded drain wait.
            while self.clock() < deadline:
                data = self.read(deadline)
                if data:
                    self.log(data)
                    if len(chunks) + len(data) > MAX_RESPONSE_BYTES:
                        raise OSError("response exceeded 8 MiB safety bound")
                    chunks.extend(data)
                    # CLI prompts end in these ASCII bytes. Avoid reparsing every
                    # sample in a long stress stream while no prompt is present.
                    if chunks.endswith(b"> ") and response_complete(command, chunks.decode("utf-8", errors="replace")):
                        complete = True
                        break
        except (OSError, KeyboardInterrupt) as exc:
            output = chunks.decode("utf-8", errors="replace")
            result = Result(command, command, "INCONCLUSIVE", str(exc) or "operator interrupted", output, self.clock() - started)
        else:
            output = chunks.decode("utf-8", errors="replace")
            outcome, reason = classify(command, output, complete)
            result = Result(command, command, outcome, reason, output, self.clock() - started)
        self.retain(f"=== {command} [{result.outcome}] ===\n{result.output}")
        self.log(f"\n=== {result.outcome}: {result.reason} ===\n".encode("utf-8"))
        return result


class Runner:
    def __init__(self, session: SerialSession, args):
        self.session = session
        self.args = args
        self.results: list[Result] = []
        self.baseline = None
        self.restoration = "not required"
        self.mutated = False
        self.background_started = False
        self.suites = [dict(name=name, outcome="NOT_RUN") for name in args.suite]
        self.retained_chars = 0
        self.cleanup_active = False

    def record(self, result: Result) -> Result:
        if len(self.results) >= MAX_RESULTS and not self.cleanup_active:
            self.results.append(Result("session evidence limit", "", "INCONCLUSIVE",
                                       "result limit reached; stopping suites and attempting cleanup"))
            raise CheckError("session exceeded result limit")
        if self.session.transcript_error is not None and not self.cleanup_active and result.outcome == "PASS":
            result.outcome = "INCONCLUSIVE"
            result.reason = "transcript write failed: " + str(self.session.transcript_error)
        self.results.append(result)
        self.retained_chars += len(result.output)
        if self.retained_chars > MAX_RETAINED_CHARS:
            for previous in self.results:
                if self.retained_chars <= MAX_RETAINED_CHARS or previous is result:
                    break
                if previous.output:
                    self.retained_chars -= len(previous.output)
                    previous.output = ""
                    previous.reason += "; output retained in transcript file only"
        print(f"{result.outcome:12} {result.check}: {result.reason}", flush=True)
        self.session.log(f"\n=== check: {result.check} [{result.outcome}] {result.reason} ===\n".encode("utf-8"))
        return result

    def assertion(self, name: str, condition: bool, reason: str) -> None:
        result = self.record(Result(name, "", "PASS" if condition else "FAIL", reason))
        if result.outcome != "PASS":
            raise CheckError(reason)

    def command(self, command: str, required=True, timeout_s=None) -> Result:
        result = self.record(self.session.command(command, timeout_s=timeout_s))
        if required and result.outcome != "PASS":
            raise CheckError(f"{command}: {result.reason}")
        return result

    def sample(self, extended=None, fresh=False):
        result = self.command("sample")
        outcome, reason = check_sample(result.output, self.args.min_c, self.args.max_c, extended, fresh)
        self.record(Result("sample semantics", "sample", outcome, reason))
        if outcome != "PASS":
            raise CheckError(reason)
        return float(SAMPLE.search(clean(result.output))[1])

    def healthy(self):
        health = parse_health(self.command("health").output)
        self.assertion("driver health and trust", health["state"] == "READY" and health["online"] and health["bound"] and health["initialized"] and not health["dirty"] and not health["pending"],
                       "requires READY, trusted initialized configuration and no pending one-shot")
        return health

    def preflight(self):
        self.command("version")
        job = self.command("job").output
        self.assertion("idle owner operation", "Operation: active=no " in clean(job) and "active=yes" not in clean(job), "CLI must have no active operation or diagnostic")
        stats = self.command("stats").output
        self.assertion("idle sampler", "Run: active " not in stats, "CLI must have no active watch or stress run")
        self.baseline = parse_settings(self.command("settings").output)
        normalized_model = self.baseline["model"].lower().replace("_address_select", "")
        self.assertion("selected BOM model", normalized_model == self.args.model, "firmware model must match operator-selected BOM; no unique chip ID exists")
        self.assertion("selected address", self.baseline["address"] == self.args.address, "firmware target address must match operator-selected strap/package")
        self.healthy()
        self.command("verify")

    def smoke(self):
        self.background_started = True
        entry = parse_settings(self.command("settings").output)
        self.command("probe")
        self.command("settings read")
        self.command("snapshot read")
        result = self.command("selfcheck")
        for label, reason in re.findall(r"\[SKIP\] ([^:\n]+): ([^\n]+)", clean(result.output)):
            self.record(Result(label, "selfcheck", "SKIP", reason))
        self.command("read")
        self.sample(entry["extended"])
        self.command("xfer_stats")

    def config(self):
        entry = parse_settings(self.command("settings").output)
        self.mutated = True
        self.command("selftest full")
        profile = parse_settings(self.command("settings").output)
        self.assertion("full-test baseline", profile == entry, "all desired settings must match the profile captured on entry to full test")

    def oneshot(self):
        self.mutated = True
        self.command("mode shutdown")
        # Use a normal-representable window before selecting normal mode, even
        # when the original profile uses extended-only thresholds.
        self.command("threshold 20 30")
        temperatures = []
        for extended in (False, True, False):
            self.command(f"extended {int(extended)}")
            self.command("measure")
            temperatures.append(self.sample(extended, fresh=True))
            self.command("snapshot read")
        outcome, reason = check_format_continuity(temperatures, self.args.format_tolerance_c)
        self.record(Result("EM transition temperature continuity", "", outcome, reason))
        if outcome != "PASS":
            raise CheckError(reason)
        self.command("verify")

    def stress(self):
        self.background_started = True
        if parse_settings(self.command("settings").output)["mode"] == "shutdown":
            self.mutated = True  # The sampler triggers physical OS writes.
        for command in (f"stress {self.args.stress_count}", f"stress_mix {self.args.stress_count}"):
            self.command(command)
            self.sample(fresh=parse_settings(self.command("settings").output)["mode"] == "shutdown")
        # Check that cached observations after stop cause no hidden traffic.
        self.command("stop")
        before = self.command("xfer_stats").output
        time.sleep(self.args.silence_seconds)
        after = self.command("xfer_stats").output
        extract = lambda text: re.search(r"Bus: attempts=(\d+) ok=(\d+) fail=(\d+)", text).groups()
        if any(int(value) >= 0xFFFFFFFF for value in extract(before) + extract(after)):
            self.record(Result("stop is bus-silent", "", "INCONCLUSIVE", "saturated counters cannot establish bus silence"))
            raise CheckError("adapter counters saturated")
        self.assertion("stop is bus-silent", extract(before) == extract(after), "adapter counters must remain unchanged after stop")

    def alert(self):
        self.mutated = True
        self.command("extended 1")
        self.command("mode cont")
        self.command("rate 8")
        self.command("alert comparator")
        physical = self.baseline["alert_output"] and self.baseline["alert_pin"] >= 0
        if not physical:
            self.record(Result("physical ALERT", "", "SKIP", "no configured physical ALERT input; internal comparator only"))
        for polarity in ("low", "high"):
            self.command("polarity " + polarity)
            for faults in (1, 2, 4, 6):
                self.command(f"faults {faults}")
                for active in (False, True, False):
                    sample = self.command("read").output
                    self.sample(extended=True)
                    temperature = float(SAMPLE.search(clean(sample))[1])
                    low, high = ((temperature - 10, temperature - 5) if active else
                                 (temperature + 5, temperature + 10))
                    if low < -256 or high > 255.9375:
                        raise CheckError("temperature leaves no representable ALERT stimulus window")
                    self.command(f"threshold {low:.4f} {high:.4f}")
                    # Deliberately tests stable response, not exact queue timing.
                    time.sleep((faults + 1) / 8.0 + 0.1)
                    result = self.command("config")
                    outcome, reason = check_comparator(result.output, polarity, faults, active)
                    self.record(Result(f"comparator {polarity}, faults {faults}, {'assert' if active else 'release'}", "config", outcome, reason))
                    if outcome != "PASS":
                        raise CheckError(reason)
                    if physical:
                        pin = self.command("alertpin").output
                        self.assertion("physical comparator response", ("Physical ALERT: active " in clean(pin)) == active,
                                       "physical GPIO must agree with expected comparator state")

    def disconnect(self):
        self.mutated = True  # Recovery is an explicit device write operation.
        self.command("mode cont")
        initial = self.healthy()
        print(f"Disconnect only the sensor now; waiting up to {self.args.fault_window:g} s for OFFLINE.", flush=True)
        end = time.monotonic() + self.args.fault_window
        offline = None
        while time.monotonic() < end:
            result = self.session.command("read", timeout_s=end - time.monotonic())
            error = ERROR.search(clean(result.output))
            if expected_error(result, TRANSPORT_ERRORS):
                result.outcome, result.reason = "PASS", "expected transport failure during guided disconnection: " + error[1]
            self.record(result)
            if result.outcome != "PASS":
                raise CheckError("unexpected disconnected read result")
            health = parse_health(self.command("health", timeout_s=end - time.monotonic()).output)
            if health["state"] == "OFFLINE":
                offline = health
                break
            time.sleep(min(0.1, max(0.0, end - time.monotonic())))
        if offline is None:
            self.record(Result("disconnect stimulus", "", "INCONCLUSIVE", "sensor did not reach OFFLINE during the operator window"))
            raise CheckError("disconnect stimulus not observed")
        self.assertion("tracked failure health", offline["consecutive"] >= self.baseline["offline_threshold"] and offline["fail"] - initial["fail"] >= self.baseline["offline_threshold"], "physical failures must reach configured OFFLINE threshold")
        print(f"Reconnect the sensor now; waiting up to {self.args.fault_window:g} s for a successful probe.", flush=True)
        end = time.monotonic() + self.args.fault_window
        present = False
        while time.monotonic() < end:
            result = self.session.command("probe", timeout_s=end - time.monotonic())
            present = result.outcome == "PASS"
            error = ERROR.search(clean(result.output))
            if expected_error(result, TRANSPORT_ERRORS):
                result.outcome, result.reason = "PASS", "expected absent probe while awaiting reconnection: " + error[1]
            self.record(result)
            if result.outcome != "PASS":
                raise CheckError("unexpected reconnect probe result")
            if present:
                break
            time.sleep(min(0.1, max(0.0, end - time.monotonic())))
        if not present:
            self.record(Result("reconnect stimulus", "", "INCONCLUSIVE", "sensor did not reappear during the operator window"))
            raise CheckError("reconnection not observed")
        after = parse_health(self.command("health").output)
        self.assertion("probe bypasses health", all(after[key] == offline[key] for key in ("ok", "fail", "consecutive", "state")), "probes must not change tracked failure state or totals")
        self.command("recover")
        self.healthy()
        self.command("read")
        self.sample()

    def stop_owned_work(self):
        self.command("stop")
        job = clean(self.command("job").output)
        self.assertion("cleanup owner idle", "Operation: active=no " in job and "active=yes" not in job,
                       "stop must finish cancellation/restoration before any recovery writes")
        health = parse_health(self.command("health").output)
        if health["pending"]:
            end = time.monotonic() + self.args.command_timeout
            for _ in range(100):
                if time.monotonic() >= end:
                    break
                result = self.session.command("tryread", timeout_s=end - time.monotonic())
                error = ERROR.search(clean(result.output))
                if expected_error(result, {"MEASUREMENT_NOT_READY"}):
                    result.outcome, result.reason = "PASS", "waiting for retained manual one-shot to finish"
                    self.record(result)
                    time.sleep(min(0.01, max(0.0, end - time.monotonic())))
                    continue
                self.record(result)
                if result.outcome != "PASS":
                    raise CheckError("cannot consume retained one-shot during cleanup")
                break
            health = parse_health(self.command("health").output)
            self.assertion("cleanup one-shot consumed", not health["pending"], "retained conversion must complete before profile recovery")

    def restore(self):
        self.cleanup_active = True
        if self.baseline is None or not (self.mutated or self.background_started):
            return
        if self.mutated:
            self.restoration = "not verified"
            print("Restoring captured desired profile and verifying hardware readback.", flush=True)
        try:
            self.stop_owned_work()
            if not self.mutated:
                return
            for command in restore_commands(self.baseline):
                self.command(command)
            profile = parse_settings(self.command("settings").output)
            self.assertion("restored desired profile", profile == self.baseline, "restored profile must exactly match captured settings")
            self.command("snapshot read")
            self.healthy()
            self.restoration = "verified"
        except (CheckError, OSError, KeyboardInterrupt) as exc:
            self.record(Result("baseline restoration", "", "FAIL", f"not verified: {exc}; captured profile is in report; manual recovery required"))

    def run(self):
        current_suite = None
        try:
            self.preflight()
            for suite in self.suites:
                current_suite = suite
                suite["outcome"] = "INCONCLUSIVE"
                getattr(self, suite["name"])()
                suite["outcome"] = "PASS"
                current_suite = None
            self.healthy()
        except CheckError as exc:
            if current_suite and any(result.outcome == "FAIL" for result in self.results):
                current_suite["outcome"] = "FAIL"
            if not any(result.outcome in {"FAIL", "INCONCLUSIVE"} for result in self.results):
                self.record(Result("session", "", "INCONCLUSIVE", str(exc)))
        except (OSError, ValueError, KeyboardInterrupt) as exc:
            self.record(Result("session", "", "INCONCLUSIVE", str(exc) or "operator interrupted"))
        finally:
            self.restore()


def git_value(*args: str) -> str:
    try:
        return subprocess.check_output(["git", *args], cwd=ROOT, text=True, stderr=subprocess.DEVNULL, timeout=5).strip()
    except (OSError, subprocess.SubprocessError):
        return "unknown"


def write_report(path: Path, runner: Runner, evidence=None) -> int:
    owns_evidence = evidence is None
    try:
        if evidence is None:
            evidence = EvidenceFiles(path, runner.args)
            evidence.transcript.write("\n\n".join(runner.session.transcript).encode("utf-8"))
            evidence.transcript.flush()
        if runner.session.transcript_error is not None:
            runner.cleanup_active = True
            runner.record(Result("transcript", "", "INCONCLUSIVE", "transcript write failed: " + runner.session.transcript_error))
        # Close before final JSON so delayed filesystem failures cannot leave a
        # PASS report paired with an evidence-error exit status.
        runner.session.transcript_file = None
        try:
            evidence.close_transcript()
        except OSError as exc:
            runner.cleanup_active = True
            runner.record(Result("transcript close", "", "INCONCLUSIVE", str(exc)))
        return _write_report(path, runner, evidence)
    except (OSError, ValueError) as exc:
        print(f"Could not finalize HIL evidence at {path}: {exc}. Run is INCONCLUSIVE; preserve the existing transcript and initial report.", file=sys.stderr)
        print(json.dumps(dict(outcome="INCONCLUSIVE", reason="report write failed", restoration=runner.restoration,
                              baseline=runner.baseline), allow_nan=False))
        return 2
    finally:
        if owns_evidence and evidence is not None:
            try:
                evidence.close()
            except OSError as exc:
                print(f"Could not remove HIL reservation lock; preserve the completed evidence: {exc}", file=sys.stderr)


def _write_report(path: Path, runner: Runner, evidence: EvidenceFiles) -> int:
    counts = {outcome: sum(result.outcome == outcome for result in runner.results) for outcome in ("PASS", "FAIL", "SKIP", "INCONCLUSIVE")}
    outcome = "FAIL" if counts["FAIL"] else "INCONCLUSIVE" if counts["INCONCLUSIVE"] or not counts["PASS"] else "PASS"
    dirty_status = git_value("status", "--porcelain")
    report = dict(schema_version=1, recorded_utc=dt.datetime.now(dt.timezone.utc).isoformat(),
                  outcome=outcome, scope="selected automated checks only; physical accuracy/ALERT/timing require separate evidence",
                  hardware_run=bool(getattr(runner.session, "hardware_connected", False)), metadata=vars(runner.args), git_commit=git_value("rev-parse", "HEAD"),
                  git_dirty=None if dirty_status == "unknown" else bool(dirty_status), baseline=runner.baseline,
                  restoration=runner.restoration, counts=counts, suites=runner.suites,
                  manual_gates=[dict(name=name, outcome="NOT_RUN") for name in
                                ("calibrated thermal accuracy", "exact conversion and fault-queue timing",
                                 "interrupt ALERT acknowledgement", "ARA arbitration", "general-call reset", "bus electrical margins")],
                  results=[asdict(result) for result in runner.results])
    evidence.write_json(report)
    print(f"{outcome}: {path}; restoration: {runner.restoration}")
    return 1 if outcome == "FAIL" else 2 if outcome == "INCONCLUSIVE" else 0


def positive(value: str) -> float:
    number = float(value)
    if not math.isfinite(number) or number <= 0:
        raise argparse.ArgumentTypeError("must be finite and greater than zero")
    return number


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="serial port, for example COM7 or /dev/ttyACM0")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--model", choices=("tmp102", "tmp112", "tmp112d"))
    parser.add_argument("--address", type=lambda value: int(value, 0), help="explicit package/strap address, e.g. 0x48")
    parser.add_argument("--suite", action="append", choices=("smoke", "config", "oneshot", "stress", "alert", "disconnect"), help="repeat for ordered suites; default smoke")
    parser.add_argument("--dry-run", action="store_true", help="show selected actions without serial or a hardware result")
    parser.add_argument("--board", default="unspecified")
    parser.add_argument("--part", default="unspecified", help="full orderable part/package and marking")
    parser.add_argument("--operator", default="unspecified")
    parser.add_argument("--notes", default="", help="supply, pullups, bus clock, reference thermometer, wiring")
    parser.add_argument("--min-c", type=float, default=-40.0, help="operator expected temperature window (not an accuracy specification)")
    parser.add_argument("--max-c", type=float, default=125.0)
    parser.add_argument("--format-tolerance-c", type=positive, default=1.0, help="maximum normal/extended/normal spread in a thermally stable fixture")
    parser.add_argument("--stress-count", type=int, default=20)
    parser.add_argument("--command-timeout", type=positive, default=5.0)
    parser.add_argument("--long-timeout", type=positive, default=120.0)
    parser.add_argument("--startup-seconds", type=positive, default=3.0)
    parser.add_argument("--silence-seconds", type=positive, default=0.25)
    parser.add_argument("--fault-window", type=positive, default=30.0, help="operator disconnect/reconnect window per phase")
    parser.add_argument("--report", default="hil_logs/tmp1x2-" + dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ") + ".json")
    args = parser.parse_args(argv)
    args.suite = args.suite or ["smoke"]
    if not 1 <= args.stress_count <= 100000 or args.baud <= 0:
        parser.error("stress count must be 1..100000 and baud must be positive")
    if not all(math.isfinite(value) for value in (args.min_c, args.max_c)) or args.min_c > args.max_c:
        parser.error("temperature window must be finite and ordered")
    for name in ("command_timeout", "long_timeout", "startup_seconds", "silence_seconds", "fault_window"):
        if getattr(args, name) > 86400:
            parser.error("time windows must not exceed 86400 seconds")
    if len(args.suite) > 32:
        parser.error("select at most 32 suites per run")
    if Path(args.report).suffix.lower() != ".json":
        parser.error("--report must end in .json")
    if args.model and args.address is not None:
        base = 0x40 if args.model == "tmp112d" else 0x48
        if not base <= args.address <= base + 3:
            parser.error("address is outside the selected model family")
    if not args.dry_run and (not args.port or not args.model or args.address is None):
        parser.error("hardware runs require explicit --port, --model and --address")
    return args


def main(argv=None) -> int:
    args = parse_args(argv)
    if args.dry_run:
        print("DRY RUN: no serial connection, hardware result or firmware upload")
        print("preflight: version; job; stats; settings; health; verify")
        descriptions = {
            "smoke": "probe; settings read; snapshot read; selfcheck; read; sample; xfer_stats",
            "config": "selftest full (18 settings cases + verified firmware restoration)",
            "oneshot": "shutdown; representable thresholds; normal/extended/normal confirmed samples + snapshots + thermal continuity",
            "stress": f"stress {args.stress_count}; stress_mix {args.stress_count}; sample checks; stop bus-silence check",
            "alert": "comparator threshold stimulus at both polarities/all fault queues; raw AL + optional GPIO; restores profile",
            "disconnect": "guided sensor disconnect; OFFLINE threshold; guided reconnect; health-bypassing probes; explicit recover",
        }
        for suite in args.suite:
            print(f"{suite}: {descriptions[suite]}")
        if MUTATING.intersection(args.suite):
            print("finally: stop once; restore captured profile using recover/typed settings; verify snapshot and health")
        return 0
    report_path = Path(args.report)
    try:
        evidence = EvidenceFiles(report_path, args)
    except (OSError, ValueError) as exc:
        print(f"Cannot reserve HIL evidence at {report_path}: {exc}. No serial connection opened.", file=sys.stderr)
        return 2
    connection = None
    session = SerialSession(None, args.command_timeout, args.long_timeout, transcript_file=evidence.transcript)
    runner = Runner(session, args)
    try:
        import serial
        connection = serial.Serial(port=None, baudrate=args.baud, timeout=0.05, write_timeout=args.command_timeout)
        session.serial = connection
        connection.dtr = False
        connection.rts = False
        connection.port = args.port
        connection.open()
        session.hardware_connected = True
        session.capture_boot(args.startup_seconds)
        runner.run()
    except ImportError:
        runner.record(Result("serial setup", "", "INCONCLUSIVE", "pyserial is required: python -m pip install pyserial"))
    except (OSError, ValueError, KeyboardInterrupt) as exc:
        runner.record(Result("serial setup", "", "INCONCLUSIVE", str(exc) or "operator interrupted"))
    finally:
        if connection is not None:
            try:
                connection.close()
            except (OSError, ValueError, KeyboardInterrupt) as exc:
                runner.cleanup_active = True
                runner.record(Result("serial close", "", "INCONCLUSIVE", str(exc)))
    code = write_report(report_path, runner, evidence)
    try:
        evidence.close()
    except OSError as exc:
        print(f"Could not remove HIL reservation lock; preserve the completed evidence: {exc}", file=sys.stderr)
    return code


if __name__ == "__main__":
    sys.exit(main())
