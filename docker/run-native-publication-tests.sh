#!/usr/bin/env bash
# Mandatory native correctness gate for both architecture publication builds.
set -euo pipefail

build_dir=build
jobs=2
test_timeout=300
run_only=0
usage() {
  echo "Usage: $0 [--build-dir DIR] [--jobs N] [--test-timeout SECONDS] [--run-only]"
  echo 'The default builds and runs the native tests. --run-only checks existing local binaries.'
}
fail() { echo "native-publication-tests: $*" >&2; exit 1; }
while (($#)); do
  case "$1" in
    --build-dir|--jobs|--test-timeout)
      (($# >= 2)) || fail "missing value for $1"
      case "$1" in
        --build-dir) build_dir=$2 ;;
        --jobs) jobs=$2 ;;
        --test-timeout) test_timeout=$2 ;;
      esac
      shift 2 ;;
    --run-only) run_only=1; shift ;;
    --help|-h) usage; exit 0 ;;
    *) fail "unknown argument: $1" ;;
  esac
done
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || fail 'jobs must be a positive integer'
[[ "$test_timeout" =~ ^[1-9][0-9]*$ ]] || fail 'test timeout must be a positive integer'
[[ -d "$build_dir" ]] || fail "build directory does not exist: $build_dir"
build_dir=$(cd -- "$build_dir" && pwd)
command -v timeout >/dev/null || fail 'GNU timeout is required'

targets=(
  test-native-parent-metadata
  test-finalize-metadata
  test-overlay-local-signature
  test-keyring-prepared-signing
  test-native-admission-batch
  test-native-admission-refresh
  test-native-admission-refresh-batch
  test-ext-message-pool-scheduler
  test-collator-external-wait-stats
  test-state-resolver-policy
  test-native-work-scratch
  test-external-message-routing
  test-cells
  test-native-load-generator-policy
)
if (( ! run_only )); then
  # Production targets have already populated the builder's shared dependencies.
  # Bound this additional build independently; no all-tests or unrelated TVM run.
  timeout --kill-after=30s 3600s cmake --build "$build_dir" --parallel "$jobs" --target "${targets[@]}"
fi

log_dir="$build_dir/native-publication-logs"
mkdir -p -- "$log_dir"
run_number=0
run_test() {
  local format=$1 name=$2 relative_binary=$3
  shift 3
  local binary="$build_dir/$relative_binary" log status summary
  [[ -f "$binary" && -x "$binary" ]] || fail "missing executable: $binary"
  run_number=$((run_number + 1))
  log="$log_dir/$name.log"
  printf 'Native publication gate: %s (timeout %ss)\n' "$name" "$test_timeout"
  if timeout --kill-after=10s "${test_timeout}s" "$@" "$binary" >"$log" 2>&1; then
    :
  else
    status=$?
    cat -- "$log" >&2
    echo "native-publication-tests: $name failed with exit $status; log: $log" >&2
    return "$status"
  fi
  if [[ "$format" == td ]]; then
    # td-test-main exits zero if its filter selects nothing. Require a complete
    # nonempty run, including matching started/PASS/final summary counts.
    if ! summary=$(awk '
      /^Running test / { started++ }
      /^PASS in / { passed++ }
      /^[0-9]+ test\(s\) passed$/ { summaries++; declared=$1 }
      END {
        if (started < 1 || started != passed || summaries != 1 || declared != passed) exit 1
        printf "%d tests passed", passed
      }' "$log"); then
      cat -- "$log" >&2
      fail "$name did not report a complete nonempty TD test run; log: $log"
    fi
    printf 'Native publication gate: %s: %s\n' "$name" "$summary"
  else
    # This standalone executable has four unconditional assertion functions and
    # no test discovery/filter mechanism. A failed require() aborts the process.
    printf 'Native publication gate: %s: standalone assertions passed\n' "$name"
  fi
}

run_test td parent-metadata test/test-native-parent-metadata env
run_test td finalize-metadata-off test/validator/consensus/test-finalize-metadata \
  env TON_NATIVE_CANDIDATE_METADATA_PROJECTION=0
run_test td finalize-metadata-on test/validator/consensus/test-finalize-metadata \
  env TON_NATIVE_CANDIDATE_METADATA_PROJECTION=1
run_test td overlay-local-signature test/test-overlay-local-signature env
run_test td keyring-prepared-signing test/test-keyring-prepared-signing env
run_test td admission-batch test/validator/test-native-admission-batch env
run_test td admission-refresh test/validator/test-native-admission-refresh env
run_test td admission-refresh-batch test/validator/test-native-admission-refresh-batch env
run_test td external-pool-scheduler test/validator/test-ext-message-pool-scheduler env
run_test td collator-external-wait test/validator/test-collator-external-wait-stats env
run_test standalone state-resolver-policy test/validator/consensus/test-state-resolver-policy env
run_test td native-work-scratch test/validator/test-native-work-scratch env
run_test td external-message-routing test/validator/test-external-message-routing env
# Put binary arguments after its pathname while preserving the shared runner's
# environment-command prefix. No general test-cells/VM suite is selected.
run_test td native-state test-cells bash -c 'exec "$1" --filter NativeStateEngine' native-state
run_test td native-load-policy lite-client/test-native-load-generator-policy env
printf 'Native publication gate passed: %s invocations; logs: %s\n' "$run_number" "$log_dir"
