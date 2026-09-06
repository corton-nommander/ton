#!/usr/bin/env python3
"""Offline audit of the fixed 50b175f5 proof A/B/B/A series; never invokes Docker.

Usage: python3 build/benchmarks/cycles-20260906b/audit-proof-series.py \
         --output build/benchmarks/cycles-20260906b/proof-series-audit.json
Run only after all four raw bundles and save-result reports are complete.
Exit 0: complete eligible series (selection can still retain incumbent).
Exit 3: missing/mismatched evidence or any failed declared capacity gate.
Exit 2: invocation/plan/output error. Output is exclusive-create, never overwritten.
No significance test, batching-default decision, or isolated proof attribution.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess
import sys

PLAN_COMMIT = "50b175f5232789cb65b0926e25b66f1930b49554"
PLAN_PATH = "doc/benchmarks/results/cycles-20260906b-proof-live-plan.json"
LABELS = ("proof-a1", "proof-b1", "proof-b2", "proof-a2")
SERVICES = ("genesis", "native-load-generator", "session-stats")
PREFIX = ["bash", "-c", 'source benchmark/native-payment-lanes-profile.sh; native_payment_lanes_profile_env 2 env "$@"', "cycle-profile"]
SHA256 = re.compile(r"sha256:[0-9a-f]{64}\Z")
HEX64 = re.compile(r"[0-9a-f]{64}\Z")
MISSING = object()


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key: " + key)
        result[key] = value
    return result


def parse_json(data):
    def bad_constant(value):
        raise ValueError("nonfinite JSON number: " + value)
    def finite_float(value):
        parsed = float(value)
        if not math.isfinite(parsed):
            raise ValueError("nonfinite JSON float: " + value)
        return parsed
    result = json.loads(data, object_pairs_hook=unique_object, parse_constant=bad_constant, parse_float=finite_float)
    if not isinstance(result, dict):
        raise ValueError("JSON root must be an object")
    return result


def digest(data):
    return hashlib.sha256(data).hexdigest()


def get(obj, path, default=None):
    for part in path.split("."):
        if not isinstance(obj, dict) or part not in obj:
            return default
        obj = obj[part]
    return obj


def number(value):
    return type(value) in (int, float) and math.isfinite(value)


def integer(value, minimum=0):
    return type(value) is int and value >= minimum


def close(a, b):
    return number(a) and number(b) and math.isclose(a, b, rel_tol=1e-12, abs_tol=1e-8)


def timestamp(value):
    if not isinstance(value, str):
        raise ValueError("missing timestamp")
    parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    if parsed.tzinfo is None:
        raise ValueError("timestamp has no timezone")
    return parsed.timestamp()


class Checks:
    def __init__(self):
        self.items = []

    def check(self, name, passed, detail=None):
        item = {"check": name, "passed": passed is True}
        if detail is not None:
            item["detail"] = detail
        self.items.append(item)
        return item["passed"]

    def equal(self, name, actual, expected):
        # Python True == 1 is unsuitable for fail-closed gate/counter checks.
        same = actual is not MISSING and actual == expected
        if type(expected) is bool or type(actual) is bool:
            same = same and type(actual) is type(expected)
        return self.check(name, same, None if same else {"expected": None if expected is MISSING else expected, "actual": None if actual is MISSING else actual})

    def field(self, obj, path, expected):
        return self.equal(path, get(obj, path, MISSING), expected)

    @property
    def valid(self):
        return all(item["passed"] for item in self.items)


def env_dict(items):
    if not isinstance(items, list):
        raise ValueError("missing environment list")
    result = {}
    for item in items:
        if not isinstance(item, str) or not re.match(r"^[A-Z][A-Z0-9_]*=", item):
            raise ValueError("malformed environment assignment")
        key, value = item.split("=", 1)
        if key in result:
            raise ValueError("duplicate environment assignment: " + key)
        result[key] = value
    return result


def settings_env(settings):
    mapping = {
        "target_tps": "TARGET_TPS", "ramp_seconds": "RAMP_SECONDS",
        "warmup_seconds": "WARMUP_SECONDS", "measurement_seconds": "DURATION_SECONDS",
        "drain_timeout_seconds": "DRAIN_TIMEOUT_SECONDS", "canonical_backlog_limit": "MAX_CANONICAL_BACKLOG",
        "per_source_backlog_limit": "MAX_SOURCE_CANONICAL_BACKLOG", "adaptive_max_cwnd": "ADAPTIVE_MAX_CWND",
        "batching": "NATIVE_RUN_BATCHING", "queries_per_client": "SUBMIT_MAX_QUERIES_PER_CLIENT",
        "initial_rtt_seconds": "ADAPTIVE_INITIAL_RTT_SECONDS", "physical_batch_size": "SUBMIT_BATCH_SIZE",
        "coalesce_ms": "SUBMIT_COALESCE_MS",
    }
    return {"NATIVE_LOAD_" + suffix: str(settings[key]) for key, suffix in mapping.items()}


def validate_identity(checks, identity, prefix):
    checks.check(prefix + ".object", isinstance(identity, dict))
    for key in ("container_id", "image_id"):
        value = get(identity, key)
        pattern = HEX64 if key == "container_id" else SHA256
        checks.check(prefix + "." + key, isinstance(value, str) and pattern.fullmatch(value) is not None)
    checks.check(prefix + ".running", get(identity, "running") is True)
    for key, minimum in (("restart_count", 0), ("validator_process.pid", 1), ("validator_process.start_ticks", 1)):
        checks.check(prefix + "." + key, integer(get(identity, key), minimum))
    timestamp(get(identity, "started_at"))


def read_artifact(path, evidence):
    raw = path.read_bytes()
    evidence[str(path)] = {"sha256": digest(raw), "bytes": len(raw)}
    return parse_json(raw)


def audit_arm(label, plan, args):
    checks, evidence = Checks(), {}
    report = {"label": label, "checks": checks.items, "artifacts": evidence}
    context = {}
    bundle = args.bundle_root / ("cycles-20260906b-" + label)
    saved_path = args.results_dir / ("cycles-20260906b-" + label + ".json")
    command_path = args.commands_dir / (label + "-command.json")
    side = "A" if label.startswith("proof-a") else "B"
    revision = plan["control_revision" if side == "A" else "treatment_revision"]
    tag = plan["control_tag" if side == "A" else "treatment_tag"]
    settings = plan["settings"]
    frozen = dict(zip(("source", "genesis", "native-load-generator"), plan["images"][side]))
    try:
        # Establish byte-level binding BEFORE reading saved rates with raw fields.
        saved = read_artifact(saved_path, evidence)
        summary_path = bundle / "benchmark-summary.json"
        summary = read_artifact(summary_path, evidence)
        launch = read_artifact(command_path, evidence)
        bound = checks.equal("saved_summary_sha256", saved.get("summary_sha256"), evidence[str(summary_path)]["sha256"])
        bound = checks.equal("saved_launch_sha256", saved.get("launch_command_sha256"), evidence[str(command_path)]["sha256"]) and bound
        if not bound:
            raise ValueError("saved/raw hash binding failed; no mixed-field interpretation performed")
        checks.equal("saved_label", saved.get("label"), label)
        checks.equal("saved_bundle", saved.get("bundle"), str(bundle))
        run, final, acceptance = summary["run"], summary["generator"]["final"], summary["acceptance"]
        checks.equal("raw_run_metadata", read_artifact(bundle / "run-metadata.json", evidence), run)
        strict = read_artifact(bundle / "strict-image-reuse.json", evidence)
        checks.equal("raw_summary_strict_evidence", summary.get("strict_image_reuse"), strict)
        checks.equal("saved_strict_evidence", get(saved, "strict_identity.evidence"), strict)
        checks.equal("saved_acceptance", saved.get("acceptance"), acceptance)
        checks.equal("saved_load_level", saved.get("load_level_acceptance"), summary.get("load_level_acceptance"))
        checks.equal("saved_images", saved.get("images"), run.get("images"))
        checks.equal("raw_generator_final", read_artifact(bundle / "generator-summary.json", evidence).get("final"), final)
        for key in ("run_id", "started_at", "finished_at", "elapsed_seconds", "benchmark_exit_code", "generator_container_exit_code", "interrupted", "env_sha256", "reproducibility"):
            checks.equal("saved_run." + key, get(saved, "run." + key, MISSING), get(run, key, MISSING))
        checks.field(run, "git_revision", plan["harness_revision"])
        checks.field(run, "source.revision", plan["harness_revision"])
        checks.field(run, "git_dirty", False)
        checks.field(run, "source.dirty", False)
        for key in ("benchmark_exit_code", "generator_container_exit_code"):
            checks.field(run, key, 0)
        checks.field(run, "interrupted", False)
        checks.field(final, "final", True)
        checks.field(final, "phase", "finished")
        for key in ("interrupted", "drain_timed_out"):
            checks.field(final, key, False)
        checks.field(saved, "status", "capacity_eligible")
        checks.field(saved, "comparison_eligible", True)
        checks.field(saved, "capacity_claim_allowed", True)
        checks.field(saved, "failed_or_missing_gates", [])
        for path in ("strict_identity.valid", "transport_accounting.native_parent_attempts_equal_physical", "transport_accounting.query_identity", "transport_accounting.distinct_batch_source_groups"):
            checks.field(saved, path, True)
        checks.field(summary, "load_level_acceptance.valid", True)
        checks.field(summary, "load_level_acceptance.classification", "capacity_eligible")
        checks.field(summary, "load_level_acceptance.capacity_claim_allowed", True)
        checks.field(summary, "validator_pool.cleanup_acceptance.valid", True)
        checks.field(summary, "generator.valid_canonical_run", True)
        gates = ("chain_correctness_valid", "run_complete", "ingress_capacity_valid", "chain_capacity_valid", "native_signed_run_quantum_required", "native_signed_run_quantum_valid", "native_signed_run_quantum_telemetry_contract_valid", "native_signed_run_quantum_benchmark_profile_valid", "canonical_lane_balance_required", "canonical_lane_balance_valid", "native_run_batching_required", "native_run_batching_enforced", "native_run_batching_valid", "strict_image_reuse_required", "strict_image_reuse_valid", "validator_cleanup_valid")
        for key in gates:
            checks.field(acceptance, key, True)
        for key in ("correctness_invalid_reasons", "run_incomplete_reasons", "ingress_capacity_invalid_reasons", "chain_capacity_invalid_reasons", "native_signed_run_quantum_invalid_reasons", "canonical_lane_balance_invalid_reasons", "native_run_batching_invalid_reasons", "validator_cleanup_invalid_reasons"):
            checks.field(acceptance, key, [])
        for key in ("benchmark_result_valid", "chain_correctness_valid", "ingress_capacity_valid", "chain_capacity_valid", "canonical_result_valid", "canonical_follower_final_catchup_complete", "offer_target_attained"):
            checks.field(final, key, True)
        for key in ("correctness_invalid_reasons", "run_incomplete_reasons", "ingress_capacity_invalid_reasons", "chain_capacity_invalid_reasons"):
            checks.field(final, key, [])

        command = launch["command"]
        checks.equal("launch_prefix", command[:4], PREFIX)
        checks.equal("launch_suffix", command[-3:], ["./run-native-benchmark.sh", ".env.physical", "benchmark-results/cycles-20260906b-" + label])
        checks.equal("launch_cwd", launch.get("cwd"), str(args.bundle_root.parent))
        checks.equal("launch_result", launch.get("result"), str(bundle))
        expected_env = settings_env(settings)
        expected_launch = dict(expected_env, TON_BRANCH=tag, TON_SIMPLEX_MAX_TPS="1", TON_NATIVE_CHECKPOINT_RETAIN_INGRESS="0", BENCHMARK_IMAGES_PREBUILT="1", BENCHMARK_STRICT_IMAGE_REUSE="1")
        launch_env = env_dict(command[4:-3])
        checks.equal("launch_exact_declared_environment", launch_env, expected_launch)
        checks.equal("saved_launch_settings", saved.get("launch_settings"), {k: v for k, v in launch_env.items() if k != "TON_BRANCH"})
        fixed_final = {
            "target_tps": settings["target_tps"], "measure_elapsed_s": settings["measurement_seconds"],
            "adaptive_max_cwnd": settings["adaptive_max_cwnd"], "effective_cwnd_cap": settings["adaptive_max_cwnd"],
            "native_signed_runs_enabled": True, "native_run_batching_requested": True, "native_run_batching_enabled": True,
            "native_signed_run_target_size": settings["logical_outputs_per_parent"], "native_payment_lane_depth": settings["lane_depth"],
            "native_run_batching_coalesce_ms": settings["coalesce_ms"], "submit_coalesce_ms": settings["coalesce_ms"],
            "submit_max_queries_per_client": settings["queries_per_client"], "native_signed_run_normal_quantum_violations": 0,
            "native_signed_run_effective_quantum_min": settings["logical_outputs_per_parent"], "native_signed_run_effective_quantum_max": settings["logical_outputs_per_parent"],
            "max_source_canonical_backlog_configured": settings["per_source_backlog_limit"],
            "native_signed_run_max_size": 16, "native_signed_run_terminal_tail_messages": 0,
            "native_signed_run_terminal_tail_logical_transfers": 0,
        }
        for key, value in fixed_final.items():
            checks.field(final, key, value)
        counters = ("steady_offered", "offered", "canonical_chain_measure_transfers", "canonical_measured_offers_at_measure_end", "canonical_measured_offers_after_drain", "canonical_total_after_drain", "wire_attempts", "logical_submission_attempts", "native_signed_run_submission_attempts", "wire_queries", "wire_batches", "wire_batch_messages", "wire_batch_source_runs", "wire_batch_source_run_max_size", "wire_batch_max_size", "max_per_client_admission_queries", "inflight", "admission_queries_inflight", "native_signed_run_normal_messages", "native_signed_run_normal_logical_transfers")
        if not all([checks.check("counter_present_integer." + key, integer(final.get(key))) for key in counters]):
            raise ValueError("missing/invalid arithmetic counter")
        measure = final["measure_elapsed_s"]
        start_ms, end_ms = final["measure_start_unix_ms"], final["measure_end_unix_ms"]
        for key in ("load_start_unix_ms", "measure_start_unix_ms", "measure_end_unix_ms"):
            checks.check("timestamp_integer." + key, integer(final.get(key), 1))
        checks.equal("measure_window_ms", end_ms - start_ms, settings["measurement_seconds"] * 1000)
        checks.equal("ramp_and_warmup_ms", start_ms - final["load_start_unix_ms"], (settings["ramp_seconds"] + settings["warmup_seconds"]) * 1000)
        bucket_start, bucket_end = math.ceil(start_ms / 1000), math.floor(end_ms / 1000)
        bucket_seconds = bucket_end - bucket_start
        checks.field(final, "canonical_gen_utime_bucket_start_unix_s", bucket_start)
        checks.field(final, "canonical_gen_utime_bucket_end_unix_s", bucket_end)
        checks.field(final, "canonical_gen_utime_bucket_duration_s", bucket_seconds)
        for kind in ("load_start", "measure_start", "measure_end"):
            checks.check(kind + "_seconds_match_ms", number(final[kind + "_unix_s"]) and abs(final[kind + "_unix_s"] - final[kind + "_unix_ms"] / 1000) <= .000501)
        if not number(measure) or measure <= 0 or bucket_seconds <= 0:
            raise ValueError("invalid measured duration")
        offered = final["steady_offered"] / measure
        canonical = final["canonical_chain_measure_transfers"] / bucket_seconds
        cohort = final["canonical_measured_offers_at_measure_end"] / measure
        checks.check("offered_rate_from_count", close(final.get("steady_offered_avg_tps"), offered))
        checks.check("canonical_rate_from_count_and_bucket", close(final.get("canonical_chain_measure_avg_tps"), canonical))
        checks.check("cohort_rate_from_count", close(final.get("canonical_measured_offer_cohort_observed_avg_tps"), cohort))
        checks.check("positive_canonical_and_actual_overload", canonical > 0 and offered > canonical)
        checks.check("offer_target_at_least_95_percent", offered >= .95 * settings["target_tps"])
        checks.check("target_attainment_ratio", close(final.get("offer_target_attainment_ratio"), offered / settings["target_tps"]))
        backpressure = final.get("measure_canonical_backpressure_fraction")
        checks.check("canonical_backpressure_at_most_one_percent", number(backpressure) and 0 <= backpressure <= .01)
        checks.equal("all_measured_offers_proven_after_drain", final["canonical_measured_offers_after_drain"], final["steady_offered"])
        checks.equal("all_offers_proven_after_drain", final["canonical_total_after_drain"], final["offered"])
        rates = {"offered_tps": offered, "canonical_chain_tps": canonical, "proof_cohort_observed_tps": cohort, "measure_seconds": measure, "canonical_gen_utime_bucket_seconds": bucket_seconds, "canonical_chain_transfers": final["canonical_chain_measure_transfers"]}
        if canonical > 0:
            rates["offered_over_canonical"] = offered / canonical
        for key, value in rates.items():
            checks.check("saved_rate." + key, close(get(saved, "rates." + key), value))
        checks.field(saved, "rates.offered_above_canonical", offered > canonical)
        checks.field(acceptance, "capacity_load.offered_above_canonical", True)
        checks.field(acceptance, "capacity_load.invalid_reasons", [])
        checks.check("capacity_load_offered_rate", close(get(acceptance, "capacity_load.measured_offered_avg_tps"), offered))
        checks.check("capacity_load_canonical_rate", close(get(acceptance, "capacity_load.measured_canonical_chain_avg_tps"), canonical))
        checks.check("positive_parent_attempts", final["wire_attempts"] > 0)
        checks.check("positive_normal_signed_runs", final["native_signed_run_normal_messages"] > 0)
        checks.equal("normal_signed_run_quantum_16", final["native_signed_run_normal_logical_transfers"], 16 * final["native_signed_run_normal_messages"])
        checks.check("logical_attempt_bounds_with_valid_repair_tails", final["wire_attempts"] <= final["logical_submission_attempts"] <= 16 * final["wire_attempts"])
        checks.equal("native_attempts_equal_physical", final["native_signed_run_submission_attempts"], final["wire_attempts"])
        checks.equal("query_accounting_identity", final["wire_queries"], final["wire_attempts"] - final["wire_batch_messages"] + final["wire_batches"])
        checks.equal("one_source_group_per_batched_parent", final["wire_batch_source_runs"], final["wire_batch_messages"])
        checks.check("batching_count_bounds", 0 <= final["wire_batches"] <= final["wire_batch_messages"] <= final["wire_attempts"])
        checks.check("physical_batch_size_and_distinct_heads", 1 <= final["wire_batch_max_size"] <= settings["physical_batch_size"] and final["wire_batch_source_run_max_size"] == 1)
        checks.check("bounded_query_credit", final["max_per_client_admission_queries"] <= settings["queries_per_client"])
        checks.field(final, "inflight", 0)
        checks.field(final, "admission_queries_inflight", 0)

        checks.field(strict, "required", True)
        checks.field(strict, "valid", True)
        before, after = strict["validator_before"], strict["validator_after"]
        validate_identity(checks, before, "validator_before")
        validate_identity(checks, after, "validator_after")
        checks.equal("validator_full_identity_unchanged", before, after)
        checks.equal("strict_setup_identity", read_artifact(bundle / "strict-image-reuse-before.json", evidence), before)
        checks.equal("validator_frozen_image", before.get("image_id"), frozen["genesis"]["id"])
        for when in ("before", "after"):
            checks.equal("generator_frozen_image_" + when, strict.get("generator_image_" + when), frozen["native-load-generator"]["id"])
        gid = strict.get("generator_container_before")
        checks.check("generator_container_id", isinstance(gid, str) and HEX64.fullmatch(gid) is not None)
        checks.equal("generator_container_unchanged", strict.get("generator_container_after"), gid)
        containers = run["containers"]
        checks.check("exact_services_once", isinstance(containers, list) and sorted(c.get("name", "") for c in containers) == sorted(SERVICES))
        indexed = {c["name"]: c for c in containers}
        normalized = {}
        for name in SERVICES:
            container = indexed[name]
            env = env_dict(container.get("benchmark_environment"))
            mounts = container.get("mounts")
            checks.check(name + ".mounts_present", isinstance(mounts, list) and len(mounts) > 0)
            destinations = []
            for mount in mounts:
                checks.check(name + ".mount_record", set(mount) == {"type", "source", "destination", "rw"} and all(isinstance(mount.get(k), str) and mount[k] for k in ("type", "source", "destination")) and type(mount.get("rw")) is bool)
                destinations.append(mount["destination"])
            checks.check(name + ".mount_destinations_unique", len(destinations) == len(set(destinations)))
            checks.check(name + ".cpuset_present", isinstance(container.get("cpuset"), str) and bool(container["cpuset"]))
            for key in ("nano_cpus", "memory_limit_bytes", "restart_count", "exit_code"):
                checks.check(name + "." + key + "_integer", integer(container.get(key)))
            checks.equal(name + ".oom_killed", container.get("oom_killed"), False)
            checks.equal(name + ".exit_code", container.get("exit_code"), 0)
            normalized[name] = {"environment": env, "cpuset": container.get("cpuset"), "nano_cpus": container.get("nano_cpus"), "memory_limit_bytes": container.get("memory_limit_bytes"), "mounts": sorted(mounts, key=lambda m: (m["destination"], m["source"], m["type"], m["rw"]))}
            if name in frozen:
                for key, expected in (("image", frozen[name]["tag"]), ("image_id", frozen[name]["id"])):
                    checks.equal(name + ".frozen_" + key, container.get(key), expected)
                checks.equal(name + ".source_revision", get(container, "oci_labels.revision"), revision)
        generator_env = normalized["native-load-generator"]["environment"]
        expected_env.update({"NATIVE_LOAD_NATIVE_TRANSFER_RUNS": "1", "NATIVE_LOAD_NATIVE_TRANSFER_RUN_SIZE": "16", "NATIVE_LOAD_PAYMENT_LANE_DEPTH": "2", "NATIVE_PAYMENT_LANE_DEPTH": "2", "NATIVE_LOAD_ADAPTIVE_INFLIGHT": "1"})
        for key, value in expected_env.items():
            checks.equal("effective_generator_env." + key, generator_env.get(key), value)
        for key, value in {"TON_SIMPLEX_MAX_TPS": "1", "TON_NATIVE_CHECKPOINT_RETAIN_INGRESS": "0", "NATIVE_PAYMENT_LANE_DEPTH": "2", "NATIVE_PAYMENT_LANES_ENABLED": "1", "NATIVE_TRANSFER_RUNS_ENABLED": "1"}.items():
            checks.equal("effective_genesis_env." + key, normalized["genesis"]["environment"].get(key), value)
        checks.equal("runtime_validator_started_at", indexed["genesis"].get("started_at"), before.get("started_at"))
        checks.equal("runtime_validator_restart_count", indexed["genesis"].get("restart_count"), before.get("restart_count"))
        checks.equal("runtime_validator_running", indexed["genesis"].get("state"), "running")
        checks.equal("runtime_validator_health", indexed["genesis"].get("health"), "healthy")
        checks.equal("runtime_generator_exited", indexed["native-load-generator"].get("state"), "exited")
        images = run["images"]
        image_ids = [item.get("image_id") for item in images]
        checks.check("service_image_records_once", len(image_ids) == len(set(image_ids)) == 3 and set(image_ids) == {indexed[s]["image_id"] for s in SERVICES})
        for service in SERVICES:
            records = [item for item in images if item.get("image_id") == indexed[service]["image_id"]]
            if len(records) != 1:
                raise ValueError("missing/duplicate image record: " + service)
            item = records[0]
            checks.check(service + ".tag_resolves_to_image", indexed[service]["image"] in item.get("repo_tags", []))
            checks.check(service + ".image_created_before_plan", timestamp(item.get("created")) <= timestamp(plan["declared_utc"]))
            if service in frozen:
                checks.equal(service + ".image_source_revision", get(item, "oci_labels.revision"), revision)
        started, finished = timestamp(run["started_at"]), timestamp(run["finished_at"])
        container_started = timestamp(before["started_at"])
        measure_start, measure_end = start_ms / 1000, end_ms / 1000
        checks.check("timestamps_ordered", timestamp(plan["declared_utc"]) <= started <= measure_start < measure_end <= finished and container_started <= started)
        checks.check("run_elapsed_seconds", close(run.get("elapsed_seconds"), finished - started))
        topology = run.get("host_topology", {})
        host = {key: run.get(key) for key in ("kernel", "cpu_model", "host_vcpus", "host_memory_bytes", "docker")}
        host.update(smt_active=topology.get("smt_active"), numa_nodes=topology.get("numa_nodes"), compose_version=get(run, "compose.version"))
        checks.check("host_identity_present", all(v is not None for v in host.values()))
        context = {"normalized_runtime": normalized, "host": host, "service_image_ids": {name: indexed[name]["image_id"] for name in SERVICES}, "strict": strict, "started": started, "finished": finished, "rates": rates, "lane_manifest_sha256": get(run, "native_payment_lanes.manifest.sha256")}
        report.update(rates=rates, source_revision=revision, source_image_declared=frozen["source"], service_image_ids=context["service_image_ids"], normalized_runtime=normalized, host=host, validator_identity=before, container_uptime_at_measure_start_s=measure_start - container_started, reproducibility=run.get("reproducibility"), declared_parallel_threshold=settings["parallel_threshold"], lane_manifest_sha256=context["lane_manifest_sha256"])
    except (OSError, ValueError, KeyError, TypeError, IndexError, AttributeError, ArithmeticError) as error:
        checks.check("complete_well_formed_bound_evidence", False, str(error))
    report["capacity_comparison_eligible"] = checks.valid
    report["failed_checks"] = [item for item in checks.items if not item["passed"]]
    return report, context


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    default_repo = Path(__file__).resolve().parents[3]
    parser.add_argument("--repo-root", type=Path, default=default_repo)
    parser.add_argument("--bundle-root", type=Path)
    parser.add_argument("--results-dir", type=Path)
    parser.add_argument("--commands-dir", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.repo_root = args.repo_root.resolve()
    args.bundle_root = (args.bundle_root or args.repo_root.parent / "MyLocalTonDocker/benchmark-results").resolve()
    args.results_dir = (args.results_dir or args.repo_root / "doc/benchmarks/results").resolve()
    args.commands_dir = (args.commands_dir or args.repo_root / "build/benchmarks/cycles-20260906b").resolve()
    args.output = args.output.resolve()
    try:
        if args.output.exists():
            raise ValueError("output already exists; choose a new audit filename")
        plan_bytes = subprocess.check_output(["git", "show", PLAN_COMMIT + ":" + PLAN_PATH], cwd=args.repo_root)
        plan = parse_json(plan_bytes)
        if plan.get("schema") != "native-proof-live-abba-plan-v1" or tuple(plan.get("sequence", ())) != LABELS:
            raise ValueError("unexpected immutable plan schema or sequence")
        arms, contexts = {}, {}
        for label in LABELS:
            arms[label], contexts[label] = audit_arm(label, plan, args)
        series = Checks()
        series.check("all_four_arms_capacity_eligible", all(arms[label]["capacity_comparison_eligible"] for label in LABELS))
        contexts_complete = all(contexts[label] for label in LABELS)
        series.check("all_four_comparison_contexts_complete", contexts_complete)
        if contexts_complete:
            first = contexts[LABELS[0]]
            for label in LABELS[1:]:
                context = contexts[label]
                series.equal(label + ".fixed_effective_env_resources_mounts", context["normalized_runtime"], first["normalized_runtime"])
                series.equal(label + ".same_host_runtime", context["host"], first["host"])
                series.equal(label + ".same_auxiliary_image", context["service_image_ids"]["session-stats"], first["service_image_ids"]["session-stats"])
            for previous, following in zip(LABELS, LABELS[1:]):
                series.check(previous + "_before_" + following, contexts[previous]["finished"] <= contexts[following]["started"])
            for early, late in (("proof-a1", "proof-a2"), ("proof-b1", "proof-b2")):
                series.equal(early + "_" + late + ".immutable_service_images", contexts[early]["service_image_ids"], contexts[late]["service_image_ids"])
            series.equal("b1_b2_full_validator_container_and_daemon_identity", contexts["proof-b1"]["strict"]["validator_after"], contexts["proof-b2"]["strict"]["validator_before"])
        pairs = []
        for control, treatment in (("proof-a1", "proof-b1"), ("proof-a2", "proof-b2")):
            a, b = contexts[control].get("rates", {}).get("canonical_chain_tps"), contexts[treatment].get("rates", {}).get("canonical_chain_tps")
            gain = (b / a - 1) * 100 if number(a) and a > 0 and number(b) else None
            pairs.append({"control": control, "treatment": treatment, "control_canonical_tps": a, "treatment_canonical_tps": b, "canonical_tps_gain_pct": gain, "descriptive_only_unless_all_series_gates_pass": True})
        eligible = series.valid
        higher = eligible and all(number(pair["canonical_tps_gain_pct"]) and pair["canonical_tps_gain_pct"] > 0 for pair in pairs)
        two_percent = higher and all(pair["canonical_tps_gain_pct"] >= 2 for pair in pairs)
        output = {
            "schema": "native-proof-live-abba-audit-v1", "audited_utc": datetime.now(timezone.utc).isoformat(),
            "auditor_sha256": digest(Path(__file__).read_bytes()),
            "plan": {"commit": PLAN_COMMIT, "path": PLAN_PATH, "sha256": digest(plan_bytes), "document": plan},
            "arms": arms, "series_checks": series.items, "capacity_comparison_eligible": eligible, "matched_pairs": pairs,
            "lane_manifest_observation": {"sha256_by_arm": {label: contexts[label].get("lane_manifest_sha256") for label in LABELS}, "identical_nonempty_all_four": contexts_complete and isinstance(contexts[LABELS[0]].get("lane_manifest_sha256"), str) and bool(HEX64.fullmatch(contexts[LABELS[0]]["lane_manifest_sha256"])) and all(contexts[label].get("lane_manifest_sha256") == contexts[LABELS[0]]["lane_manifest_sha256"] for label in LABELS), "gate": False, "semantics": "Report-only source-population corroboration; immutable manifest content was not a separate predeclared promotion gate."},
            "selection": {"selected_revision": plan["treatment_revision"] if higher else plan["control_revision"], "measured_higher_tps_candidate": higher, "repeatable_at_least_two_percent_claim_allowed": two_percent, "reason": "both_paired_gains_positive_and_all_declared_gates_pass" if higher else ("at_least_one_pair_not_positive_retain_incumbent" if eligible else "ineligible_or_incomplete_series_retain_incumbent"), "statistical_significance_claim": False, "isolated_proof_tps_attribution": False, "changes_batching_default": False},
            "limitations": [
                "Two fixed pairs support descriptive repeatability only; no statistical significance or universal capacity claim.",
                "Direct incumbent versus additive telemetry plus proof optimization; TPS changes cannot be assigned solely to proof traversal.",
                "Container age at measurement start is observed from timestamps. PID/start ticks establish daemon identity, not absolute daemon uptime without boot-time/clock-tick calibration.",
                "No cache-readiness telemetry or maximum cache-age limit was predeclared. B2 intentionally reuses the B1 process; A2 returns to the control image. Cache and database history remain possible confounds despite fixed mounts and warmup.",
                "Equal mount sources establish storage-location continuity, not unchanged data contents or database/cache working sets.",
                "Session-stats may lack OCI source revision; its immutable image must match across all arms. The predeclared auxiliary provenance gap is disclosed separately from core correctness/capacity acceptance.",
                "Source base-image IDs and parallel threshold are declared in the immutable plan. Runtime evidence independently checks exact derived service IDs and OCI revisions, not Docker ancestry or an unexported threshold.",
                "Image creation times precede the declaration and strict identities cover setup/load boundaries. Absence of unrelated compiler/tests/microbenchmarks is an operator declaration; these artifacts cannot independently prove host-wide workload exclusion.",
            ],
        }
        with args.output.open("x") as stream:
            json.dump(output, stream, indent=2, allow_nan=False)
            stream.write("\n")
        print(json.dumps({"output": str(args.output), "capacity_comparison_eligible": eligible, "selection": output["selection"], "failed_checks": sum(len(arm["failed_checks"]) for arm in arms.values()) + sum(not item["passed"] for item in series.items)}))
        return 0 if eligible else 3
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print("audit error: " + str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
