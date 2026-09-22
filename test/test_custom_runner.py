"""PlatformIO runner for the same dependency-free native regression executable."""
from platformio.test.runners.base import TestRunnerBase
from platformio.test.result import TestCase, TestStatus


class CustomTestRunner(TestRunnerBase):
    def on_testing_line_output(self, line):
        super().on_testing_line_output(line)
        if line.startswith("[PASS] "):
            self.test_suite.add_case(TestCase(line[7:].strip(), TestStatus.PASSED))
        elif line.startswith("[FAIL] "):
            self.test_suite.add_case(TestCase(line[7:].strip(), TestStatus.FAILED))
