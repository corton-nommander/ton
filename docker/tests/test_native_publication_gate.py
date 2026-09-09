#!/usr/bin/env python3
"""Exercise publication-gate failures without a compiler, Docker, or validator."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


GATE = Path(__file__).resolve().parents[1] / "run-native-publication-tests.sh"
BINARIES = (
    "test/test-native-parent-metadata",
    "test/validator/consensus/test-finalize-metadata",
    "test/test-overlay-local-signature",
    "test/test-keyring-prepared-signing",
    "test/validator/test-native-admission-batch",
    "test/validator/test-native-admission-refresh",
    "test/validator/test-native-admission-refresh-batch",
    "test/validator/test-ext-message-pool-scheduler",
    "test/validator/test-collator-external-wait-stats",
    "test/validator/consensus/test-state-resolver-policy",
    "test/validator/test-native-work-scratch",
    "test/validator/test-external-message-routing",
    "test-cells",
    "lite-client/test-native-load-generator-policy",
)
GOOD = """#!/usr/bin/env python3
import os, pathlib, sys
name = pathlib.Path(sys.argv[0]).name
if name == 'test-cells':
    assert sys.argv[1:] == ['--filter', 'NativeStateEngine'], sys.argv
if name == 'test-finalize-metadata':
    print('projection=' + os.environ['TON_NATIVE_CANDIDATE_METADATA_PROJECTION'])
if name != 'test-state-resolver-policy':
    print('Running test Test_Contract_selected...')
    print('PASS in 1.0ms')
    print('1 test(s) passed')
"""


class PublicationGateTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="native publication gate ")
        self.addCleanup(self.temp.cleanup)
        self.build = Path(self.temp.name) / "build with spaces"
        self.build.mkdir()
        for binary in BINARIES:
            self.write_executable(binary, GOOD)

    def write_executable(self, relative, content):
        path = self.build / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        path.chmod(0o700)

    def run_gate(self, *options, run_only=True, env=None):
        return subprocess.run(
            ["bash", str(GATE), "--build-dir", str(self.build),
             *(["--run-only"] if run_only else []), *options],
            text=True, capture_output=True, timeout=20, env=env,
        )

    def test_complete_gate_runs_both_modes_and_only_native_state_filter(self):
        result = self.run_gate()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("passed: 15 invocations", result.stdout)
        logs = self.build / "native-publication-logs"
        for mode, suffix in (("0", "off"), ("1", "on")):
            self.assertIn("projection=" + mode, (logs / f"finalize-metadata-{suffix}.log").read_text())

    def test_zero_selected_tests_is_failure_even_with_zero_exit(self):
        self.write_executable(BINARIES[0], "#!/bin/sh\necho '0 test(s) passed'\n")
        result = self.run_gate()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("complete nonempty TD test run", result.stderr)
        self.assertFalse((self.build / "native-publication-logs/finalize-metadata-off.log").exists())

    def test_partial_success_output_is_not_a_passing_suite(self):
        self.write_executable(BINARIES[0], GOOD + "print('Running test Test_Contract_unfinished...')\n")
        result = self.run_gate()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("complete nonempty TD test run", result.stderr)

    def test_nonzero_exit_cannot_be_hidden_by_success_output(self):
        self.write_executable(BINARIES[0], GOOD + "sys.exit(42)\n")
        result = self.run_gate()
        self.assertEqual(result.returncode, 42)
        self.assertIn("failed with exit 42", result.stderr)

    def test_hung_test_times_out_and_prevents_later_suites(self):
        self.write_executable(BINARIES[0], "#!/usr/bin/env python3\nimport time\ntime.sleep(60)\n")
        result = self.run_gate("--test-timeout", "1")
        self.assertEqual(result.returncode, 124)
        self.assertIn("failed with exit 124", result.stderr)
        self.assertFalse((self.build / "native-publication-logs/finalize-metadata-off.log").exists())

    def test_missing_executable_fails(self):
        (self.build / BINARIES[0]).unlink()
        result = self.run_gate()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missing executable", result.stderr)

    def test_default_route_build_failure_prevents_any_test(self):
        tool_dir = Path(self.temp.name) / "tools"
        tool_dir.mkdir()
        cmake = tool_dir / "cmake"
        cmake.write_text("#!/bin/sh\nexit 38\n")
        cmake.chmod(0o700)
        env = dict(os.environ, PATH=str(tool_dir) + os.pathsep + os.environ["PATH"])
        result = self.run_gate(run_only=False, env=env)
        self.assertEqual(result.returncode, 38)
        self.assertFalse((self.build / "native-publication-logs").exists())


if __name__ == "__main__":
    unittest.main()
