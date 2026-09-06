#!/usr/bin/env python3
"""Read one completed proof-series bundle; print checkpoint CPU/wall and packing JSON."""

import argparse
from collections import Counter, defaultdict
import hashlib
import json
import math
from pathlib import Path
import sys

STAGES = (
    'native_stat_checkpoint_rebuild', 'native_proof_preflight', 'native_commit',
    'native_staged_dict_set', 'native_execute', 'native_account_cell_build',
    'native_state_install', 'native_batch_serialize', 'create_state_merkle_update', 'total',
)
COUNTERS = (
    'native_microbatch_accepted', 'native_microbatch_input', 'native_microbatch_delayed',
    'native_microbatch_permanent', 'native_microbatches', 'native_microbatch_unique_accounts',
    'native_account_cells_built', 'native_staged_dict_sets', 'native_state_accounts_installed',
    'native_stat_checkpoint_base_snapshots', 'native_stat_checkpoint_rebuilds',
    'native_checkpoint_groups', 'native_checkpoint_group_entries', 'native_checkpoint_group_fragments',
    'native_checkpoint_rollbacks', 'native_checkpoint_rollback_entries',
    'native_checkpoint_ingress_retentions', 'native_checkpoint_flush_capacity',
    'native_checkpoint_flush_ingress', 'native_checkpoint_flush_deadline',
    'native_checkpoint_flush_fanout', 'native_checkpoint_flush_headroom',
    'native_checkpoint_flush_latency', 'native_deferral_state_nonce_mismatch_entries',
    'native_hard_preflight_failures', 'native_size_guard_deferrals',
)
MAXIMA = ('native_checkpoint_group_max_entries', 'native_checkpoint_group_max_fragments',
          'native_microbatch_max_input', 'native_microbatch_max_unique_accounts')
COARSE_BINS = ('le64', '65_80', '81_128', '129_256', '257_511', '512', 'gt512')
FINE_BINS = ('le8', '9_16', '17_32', '33_64')
HISTOGRAMS = {
    'microbatch_accounts': ['native_microbatch_accounts_' + key for key in COARSE_BINS],
    'staged_updates': ['native_staged_updates_' + key for key in COARSE_BINS],
    'staged_workers': ['native_staged_workers_' + key for key in ('1', '2', '4', '8', 'other')],
    'microbatch_accounts_small': ['native_microbatch_accounts_' + key for key in FINE_BINS],
    'staged_updates_small': ['native_staged_updates_' + key for key in FINE_BINS],
}


def obj(value):
    return value if isinstance(value, dict) else {}


def number(value, integer=False):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    if not math.isfinite(value) or value < 0 or (integer and value != int(value)):
        return None
    return int(value) if integer else value


def divide(top, bottom, scale=1.0):
    return scale * top / bottom if top is not None and bottom is not None and bottom > 0 else None


def parse_fields(value):
    result = defaultdict(list)
    if isinstance(value, str):
        for token in value.split():
            if '=' in token:
                key, data = token.split('=', 1)
                result[key].append(data)
    return result


def field(fields, key, integer=False):
    values = fields.get(key, [])
    if len(values) != 1:
        return None
    try:
        value = float(values[0])
    except ValueError:
        return None
    return number(value, integer)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def analyze(bundle):
    summary_path = bundle / 'benchmark-summary.json'
    summary = json.loads(summary_path.read_text())
    final = obj(obj(summary.get('generator')).get('final'))
    if not obj(summary.get('run')).get('finished_at') or final.get('final') is not True:
        raise ValueError('bundle is not finalized; do not analyze the active arm')
    pipeline = obj(summary.get('validator_pipeline'))
    window = obj(pipeline.get('measured_window_unix_s'))
    start, end = number(window.get('start')), number(window.get('end'))
    if start is None or end is None or end <= start:
        raise ValueError('completed bundle has no valid pipeline measurement window')
    rows = 0
    invalid_lines = 0
    unexpected_types = Counter()
    selected_per_lane = Counter()
    sums = {kind: defaultdict(float) for kind in ('real', 'cpu', 'counter')}
    maxima = defaultdict(float)
    present = {kind: Counter() for kind in ('real', 'cpu', 'counter', 'maximum')}
    all_counters = COUNTERS + tuple(key for fields in HISTOGRAMS.values() for key in fields)
    block_transactions = 0
    missing_transaction_rows = 0
    raw_path = bundle / 'validator-session-stats.jsonl'
    with raw_path.open() as stream:
        for line in stream:
            try:
                row = json.loads(line)
            except ValueError:
                if line.strip():
                    invalid_lines += 1
                continue
            if not isinstance(row, dict) or row.get('block_stats') is None:
                continue
            block_id = obj(row.get('block_id'))
            wc = block_id.get('workchain')
            if wc is None:
                wc = block_id.get('workchain_id', -1)
            at = number(row.get('collated_at'))
            if wc != 0 or at is None or not start <= at < end:
                continue
            # This mirrors the harness selector exactly. Type is audited rather
            # than adding a narrower filter that would change the population.
            if row.get('@type') != 'validatorStats.collatedBlock':
                unexpected_types[str(row.get('@type'))] += 1
            rows += 1
            selected_per_lane[str(block_id.get('shard', block_id.get('shard_id', 'unknown')))] += 1
            transactions = number(obj(row.get('block_stats')).get('transactions'), True)
            if transactions is None:
                missing_transaction_rows += 1
            else:
                block_transactions += transactions
            real = parse_fields(row.get('work_time_real_stats'))
            cpu = parse_fields(row.get('work_time_cpu_stats'))
            for kind, parsed in (('real', real), ('cpu', cpu)):
                for key in STAGES:
                    value = field(parsed, key)
                    if value is not None:
                        present[kind][key] += 1
                        sums[kind][key] += value
            for key in all_counters:
                value = field(real, key, True)
                if value is not None:
                    present['counter'][key] += 1
                    sums['counter'][key] += value
            for key in MAXIMA:
                value = field(real, key, True)
                if value is not None:
                    present['maximum'][key] += 1
                    maxima[key] = max(maxima[key], value)
    if rows == 0:
        raise ValueError('no measured basechain collated records')

    def total(kind, key):
        if present[kind][key] != rows:
            return None
        value = sums[kind][key]
        return int(value) if kind == 'counter' else value

    counts = {key: total('counter', key) for key in COUNTERS}
    accepted = counts['native_microbatch_accepted']
    rebuilds = counts['native_stat_checkpoint_rebuilds']
    groups = counts['native_checkpoint_groups']
    stages = {}
    for key in STAGES:
        stages[key] = {}
        for kind in ('real', 'cpu'):
            value = total(kind, key)
            stages[key][kind] = {
                'capture_complete': present[kind][key] == rows,
                'records_with_value': present[kind][key], 'sum_s': value,
                'us_per_accepted_candidate_transfer': divide(value, accepted, 1e6),
                'us_per_checkpoint_rebuild': divide(value, rebuilds, 1e6)
                    if key == 'native_stat_checkpoint_rebuild' else None,
            }
    histograms = {}
    for name, fields in HISTOGRAMS.items():
        available = all(present['counter'][key] == rows for key in fields)
        histograms[name] = {
            'capture_complete': available,
            'counts': {key: total('counter', key) for key in fields} if available else None,
            'missing_or_invalid_rows_by_field': {key: rows - present['counter'][key]
                for key in fields if present['counter'][key] != rows},
        }
    measured = obj(obj(pipeline.get('measured')).get('collated_basechain'))
    fast = obj(measured.get('native_fast_path_counters'))
    consistency = {
        'raw_candidate_count_matches_pipeline': rows == measured.get('blocks'),
        'raw_accepted_matches_pipeline': accepted == fast.get('accepted') if accepted is not None else None,
        'raw_rebuild_count_matches_pipeline': rebuilds == fast.get('checkpoint_rebuilds') if rebuilds is not None else None,
        'raw_groups_match_pipeline': groups == obj(fast.get('checkpoint_coalescing')).get('native_checkpoint_groups')
            if groups is not None else None,
        'raw_transactions_match_pipeline': block_transactions == measured.get('transfers')
            if missing_transaction_rows == 0 else None,
        'unexpected_selected_record_types': dict(unexpected_types),
        'invalid_jsonl_lines_skipped_like_harness': invalid_lines,
    }
    for key in STAGES:
        reported = obj(obj(measured.get('stages_real_s')).get(key))
        if key == 'total':
            reported = obj(measured.get('work_time_s'))
        value = stages[key]['real']['sum_s']
        avg, samples = number(reported.get('avg')), number(reported.get('samples'), True)
        consistency['real_stage_matches_pipeline:' + key] = math.isclose(
            value, avg * samples, rel_tol=1e-10, abs_tol=1e-8) and samples == rows \
            if value is not None and avg is not None and samples is not None else None
    diagnostics = obj(obj(summary.get('validator_pool')).get('batch_diagnostics'))
    pacing = obj(final.get('pacing_telemetry'))
    attempts = number(final.get('native_signed_run_submission_attempts'), True)
    retries = number(obj(final.get('retries_by_reason')).get('not_ready'), True)
    run = obj(summary.get('run'))
    return {
        'schema': 'native-proof-arm-analysis-v1', 'bundle': str(bundle),
        'comparison_scope': 'Incumbent 4caa92df versus combined diagnostics/proof candidate 6a96c953; no pure-proof live TPS attribution.',
        'run': {key: run.get(key) for key in ('run_id', 'started_at', 'finished_at', 'elapsed_seconds',
                                            'benchmark_exit_code', 'generator_container_exit_code', 'env_sha256')},
        'provenance': {'benchmark_summary_sha256': sha256(summary_path),
                       'validator_session_stats_sha256': sha256(raw_path),
                       'analysis_script_sha256': sha256(Path(__file__))},
        'measurement_window': {'start_unix_s': start, 'end_unix_s': end, 'duration_s': end - start,
            'selection': 'Non-null block_stats; basechain workchain/workchain_id=0; start <= collated_at < end. Pipeline prefers rounded generator *_unix_ms / 1000.',
            'scope': 'Full accumulated cost of candidate records finishing inside the phase; not cost clipped at boundaries, unique canonical transfers, or the offered cohort.'},
        'candidate_records': rows, 'candidate_records_by_shard': dict(selected_per_lane),
        'accepted_candidate_transfers': accepted,
        'block_stats_transactions': block_transactions if missing_transaction_rows == 0 else None,
        'consistency': consistency,
        'stages': stages,
        'stage_semantics': 'Checkpoint timing includes base/trial ProofStorageStat copying and add_proof traversal. CPU uses the executing thread clock; checkpoint scope is synchronous. Native commit encloses component stages: do not add parent/child timings. Values are printed to microsecond precision per candidate.',
        'packing': {'counters': counts,
            'maxima_across_candidates': {key: int(maxima[key]) if present['maximum'][key] == rows else None for key in MAXIMA},
            'counter_missing_or_invalid_rows': {key: rows - present['counter'][key] for key in COUNTERS if present['counter'][key] != rows},
            'checkpoint_rebuilds_per_million_accepted': divide(rebuilds, accepted, 1e6),
            'group_entries_per_group': divide(counts['native_checkpoint_group_entries'], groups),
            'group_fragments_per_group': divide(counts['native_checkpoint_group_fragments'], groups),
            'staged_sets_per_accepted': divide(counts['native_staged_dict_sets'], accepted),
            'histograms': histograms,
            'semantics': 'Histogram populations include attempts/rollback; bins are not per-checkpoint timing strata. Missing fine telemetry in the incumbent is unavailable, never zero.'},
        'admission_full_run': {'capture_complete': diagnostics.get('capture_complete') is True,
            'unavailable_reason': None if diagnostics.get('capture_complete') is True else 'missing_or_incomplete_additive_telemetry_old_image_or_capture',
            'counter_deltas_valid': diagnostics.get('counter_deltas_valid'),
            'counters_delta': diagnostics.get('counters_delta'), 'timings': diagnostics.get('timings'),
            'native_parent_attempts': attempts, 'not_ready_retry_schedules': retries,
            'not_ready_schedules_per_parent_attempt': divide(retries, attempts),
            'semantics': 'Admission causes and timing means cover before/after full-run pool captures; retry counts cover generator attempts including retries/repairs. They cannot be divided by measured candidate accepted transfers.'},
        'generator_measurement': {key: final.get(key) for key in ('target_tps', 'steady_offered', 'steady_offered_avg_tps',
            'measure_elapsed_s', 'canonical_gen_utime_bucket_duration_s', 'canonical_chain_measure_transfers',
            'canonical_chain_measure_avg_tps', 'measure_canonical_backpressure_fraction', 'canonical_backlog_at_measure_end')},
        'pacing_measured': {'available': bool(pacing),
            'values': {key: value for key, value in pacing.items() if key.startswith('measure_')} if pacing else None,
            'semantics': 'Measured clipped credits, exact boundary balances, worker-second issue holds and phase-intersected maxima. Worker seconds are not wall-time union; no values are synthesized for old images.'},
        'external_wait_measured': measured.get('external_wait_breakdown'),
        'acceptance': summary.get('acceptance'),
        'load_level_acceptance': summary.get('load_level_acceptance'),
        'strict_image_reuse': summary.get('strict_image_reuse'),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bundle', type=Path)
    args = parser.parse_args()
    print(json.dumps(analyze(args.bundle.resolve()), indent=2, allow_nan=False))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError) as error:
        print(f'proof arm analysis: {error}', file=sys.stderr)
        sys.exit(2)
