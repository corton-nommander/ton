#!/usr/bin/env python3
"""Validate and compare a complete frozen-driver offline A/B/B/A matrix.

Read-only inputs; writes every case to a new JSON/CSV aggregate directory.
No benchmarks, binaries, builds, or tests are invoked. Positive percent change
means higher component cost (regression); negative means lower cost.
"""
import argparse
import csv
import hashlib
import itertools
import json
import math
from pathlib import Path
import statistics
import sys

LABELS = ('a1', 'b1', 'b2', 'a2')
POPULATIONS = (8, 16, 32, 64, 80, 128, 192, 256, 512)
CHECKPOINTS = (1, 4, 16)
MODES = ('plain_prior', 'tracked_prior', 'foreign_usage')
DISTRIBUTIONS = ('uniform', 'prefix2')
CASE_FIELDS = ('base_accounts', 'base_proof_leaves', 'distribution', 'mode', 'updates', 'checkpoints')
METRICS = tuple(stage + '_' + clock + '_us_per_checkpoint'
                for stage in ('copy', 'proof', 'trial') for clock in ('wall', 'cpu'))
HEADER = (*CASE_FIELDS, 'round', 'iterations', *METRICS)
DEFAULTS = {'rounds': 3, 'iterations': 3, 'base_accounts': 4096, 'base_proof_leaves': 64, 'mode': 'all'}
OPTION_FIELDS = {'--rounds': 'rounds', '--iterations': 'iterations', '--base-accounts': 'base_accounts',
                 '--base-proof-leaves': 'base_proof_leaves', '--mode': 'mode'}


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(chunk)
    return result.hexdigest()


def valid_sha(value):
    return isinstance(value, str) and len(value) == 64 and all(c in '0123456789abcdef' for c in value)


def exact_int(value, context, minimum=0):
    require(type(value) is int and value >= minimum, context + ': expected integer >= ' + str(minimum))
    return value


def csv_int(value, context):
    require(isinstance(value, str) and value.isascii() and value.isdecimal(), context + ': invalid integer token')
    return int(value)


def number(value, context):
    require(type(value) in (int, float), context + ': expected numeric value')
    result = float(value)
    require(math.isfinite(result) and result >= 0, context + ': expected finite nonnegative value')
    return result


def parse_options(arguments):
    require(isinstance(arguments, list) and all(isinstance(item, str) for item in arguments),
            'driver_arguments must be a string array')
    require(len(arguments) % 2 == 0, 'driver arguments must contain complete option/value pairs')
    result = dict(DEFAULTS)
    for option, token in zip(arguments[::2], arguments[1::2]):
        require(option in OPTION_FIELDS, 'unknown driver option: ' + option)
        name = OPTION_FIELDS[option]
        if name == 'mode':
            require(token in (*MODES, 'all'), 'invalid driver mode: ' + token)
            result[name] = token
        else:
            # The frozen C++ driver accepts an optional plus sign and leading
            # whitespace through stoul; normalize only for expected matrix keys.
            digits = token.lstrip(' \t\n\r\f\v')
            if digits.startswith('+'):
                digits = digits[1:]
            require(digits.isascii() and digits.isdecimal(), 'invalid driver integer: ' + token)
            value = int(digits)
            minimum = 0 if name == 'base_proof_leaves' else 1
            require(minimum <= value <= 1_000_000, 'driver value outside frozen bounds: ' + token)
            result[name] = value
    require(result['base_accounts'] >= 512, 'base_accounts must be at least 512')
    return result


def checked_identity(provenance, name):
    identity = provenance.get(name)
    require(isinstance(identity, dict), 'missing ' + name + ' provenance')
    build = identity.get('build_metadata')
    require(isinstance(build, dict), name + ': missing embedded build metadata')
    require(isinstance(identity.get('path'), str) and bool(identity['path']), name + ': missing executable path')
    require(valid_sha(identity.get('sha256')), name + ': invalid binary SHA256')
    require(build.get('binary_sha256') == identity['sha256'], name + ': embedded binary SHA256 mismatch')
    require(build.get('output') == identity['path'], name + ': embedded executable path mismatch')
    require(valid_sha(identity.get('build_metadata_sha256')), name + ': invalid build sidecar SHA256')
    for field in ('driver_sha256', 'oracle_sha256'):
        require(valid_sha(build.get(field)), name + ': missing/invalid ' + field)
    # Archived analysis relies on the hashes captured by the runner; it does
    # not require binaries or absolute sidecar paths to remain on this host.
    return identity


def expected_cases(options):
    modes = MODES if options['mode'] == 'all' else (options['mode'],)
    return set(itertools.product((options['base_accounts'],), (options['base_proof_leaves'],),
                                 DISTRIBUTIONS, modes, POPULATIONS, CHECKPOINTS))


def read_run(directory, run, identity, arguments, options, cases):
    label = run['label']
    exact_int(run.get('exit_code'), label + '.exit_code')
    require(run['exit_code'] == 0, label + ': run was not successful')
    require(run.get('command') == [identity['path'], *arguments], label + ': binary path or exact driver arguments differ')
    number(run.get('wall_seconds'), label + '.wall_seconds')
    path = directory / (label + '.csv')
    require(path.is_file(), label + ': missing CSV')
    require(valid_sha(run.get('csv_sha256')) and digest(path) == run['csv_sha256'], label + ': CSV SHA256 mismatch')
    observed = {}
    with path.open(newline='') as stream:
        reader = csv.DictReader(stream)
        require(tuple(reader.fieldnames or ()) == HEADER, label + ': unexpected CSV schema/order')
        for line, raw in enumerate(reader, 2):
            context = label + ': line ' + str(line)
            require(None not in raw and all(value is not None for value in raw.values()), context + ': malformed CSV row')
            row = {name: csv_int(raw[name], context + ' ' + name)
                   for name in ('base_accounts', 'base_proof_leaves', 'updates', 'checkpoints', 'round', 'iterations')}
            row.update(distribution=raw['distribution'], mode=raw['mode'])
            case = tuple(row[name] for name in CASE_FIELDS)
            require(case in cases, context + ': unexpected case ' + repr(case))
            require(row['round'] < options['rounds'], context + ': round exceeds configured range')
            require(row['iterations'] == options['iterations'], context + ': iteration count differs from driver arguments')
            key = (*case, row['round'])
            require(key not in observed, context + ': duplicate case/round')
            for metric in METRICS:
                try:
                    value = float(raw[metric])
                except (TypeError, ValueError) as error:
                    raise ValueError(context + ': invalid ' + metric) from error
                row[metric] = number(value, context + ' ' + metric)
            for clock in ('wall', 'cpu'):
                subtotal = row['copy_' + clock + '_us_per_checkpoint'] + row['proof_' + clock + '_us_per_checkpoint']
                trial = row['trial_' + clock + '_us_per_checkpoint']
                # Each independently printed column rounds to 0.001 us.
                require(abs(trial - subtotal) <= 0.001501, context + ': inconsistent rounded copy+proof/trial ' + clock)
            observed[key] = row
    expected_row_count = len(cases) * options['rounds']
    require(len(observed) == expected_row_count, label + ': incomplete matrix (' + str(len(observed)) + '/' + str(expected_row_count) + ')')
    require(set(observed) == {(*case, round_) for case in cases for round_ in range(options['rounds'])},
            label + ': case/round keys do not match the complete frozen-driver matrix')
    require(exact_int(run.get('rows'), label + '.rows') == len(observed), label + ': provenance row count mismatch')
    comparisons = sum(row['iterations'] * row['checkpoints'] for row in observed.values())
    require(exact_int(run.get('timed_checkpoint_stat_comparisons'), label + '.timed_checkpoint_stat_comparisons') == comparisons,
            label + ': provenance timed checkpoint count mismatch')
    return observed


def compare(control, treatment):
    control = number(control, 'derived control median')
    treatment = number(treatment, 'derived treatment median')
    delta = treatment - control
    percent_change = None if control == 0 else (delta / control) * 100.0
    require(percent_change is None or math.isfinite(percent_change), 'derived percentage overflows finite range')
    direction = 'improved' if delta < 0 else 'regressed' if delta > 0 else 'unchanged'
    return {'control_us': control, 'treatment_us': treatment, 'delta_us': delta,
            'percent_change': percent_change,
            'direction': direction, 'zero_control': control == 0,
            'ratio_unavailable_reason': 'control_rounded_to_zero' if control == 0 else None}


def consistency(first, second):
    if first == second:
        return 'both_' + first
    return 'mixed'


def describe_case(key):
    return dict(zip(CASE_FIELDS, key))


def analyze(directory):
    provenance_path = directory / 'provenance.json'
    require(provenance_path.is_file(), 'missing provenance.json')
    provenance = json.loads(provenance_path.read_text())
    require(provenance.get('schema') == 'native-proof-offline-abba-v1', 'unexpected provenance schema')
    arguments = provenance.get('driver_arguments')
    options = parse_options(arguments)
    baseline, candidate = checked_identity(provenance, 'baseline'), checked_identity(provenance, 'candidate')
    for field in ('driver_sha256', 'oracle_sha256'):
        require(baseline['build_metadata'][field] == candidate['build_metadata'][field],
                'baseline/candidate ' + field + ' differs')
    runs = provenance.get('runs')
    require(isinstance(runs, list) and len(runs) == 4 and all(isinstance(run, dict) for run in runs),
            'exactly four complete runs are required')
    require([run.get('label') for run in runs] == list(LABELS), 'runs must be exactly A1/B1/B2/A2 in order')
    cases = expected_cases(options)
    data = {run['label']: read_run(directory, run,
            baseline if run['label'].startswith('a') else candidate, arguments, options, cases) for run in runs}
    # Equality follows from the full expected matrix check; keep it explicit
    # to protect later schema changes from quietly changing paired populations.
    require(all(set(data[label]) == set(data['a1']) for label in LABELS), 'paired case/round keys differ')
    results, flat = [], []
    directions = {mode: {metric: {'cases': 0, 'both_improved': 0, 'both_regressed': 0,
                                  'both_unchanged': 0, 'mixed': 0} for metric in METRICS}
                  for mode in sorted({case[3] for case in cases})}
    fallback = []
    for case in sorted(cases):
        entry = {'case': describe_case(case), 'metrics': {}}
        for metric in METRICS:
            samples = {label: [data[label][(*case, round_)][metric] for round_ in range(options['rounds'])]
                       for label in LABELS}
            medians = {label: statistics.median(values) for label, values in samples.items()}
            comparisons = {'pair1': compare(medians['a1'], medians['b1']),
                           'pair2': compare(medians['a2'], medians['b2']),
                           'pooled': compare(statistics.median(samples['a1'] + samples['a2']),
                                             statistics.median(samples['b1'] + samples['b2']))}
            direction = consistency(comparisons['pair1']['direction'], comparisons['pair2']['direction'])
            entry['metrics'][metric] = {'run_median_us': medians, **comparisons,
                                        'direction_consistency': direction,
                                        'same_pair_direction': direction != 'mixed'}
            count = directions[case[3]][metric]
            count['cases'] += 1
            count[direction] += 1
            record = {**entry['case'], 'metric': metric, **{label + '_median_us': medians[label] for label in LABELS},
                      'direction_consistency': direction, 'same_pair_direction': direction != 'mixed'}
            for name, comparison in comparisons.items():
                for field, value in comparison.items():
                    record[name + '_' + field] = value
                if case[3] == 'foreign_usage':
                    fallback.append({'case': entry['case'], 'metric': metric, 'comparison': name, **comparison})
            flat.append(record)
        results.append(entry)
    fallback_summary = {'present': bool(fallback), 'cases': sum(case[3] == 'foreign_usage' for case in cases),
                        'semantics': 'Unweighted worst observed foreign_usage case; positive cost change is a regression. This is not a production distribution.'}
    if not fallback:
        fallback_summary['unavailable_reason'] = 'foreign_usage mode not requested'
    else:
        fallback_summary['by_metric'] = {}
        for metric in METRICS:
            metric_records = [item for item in fallback if item['metric'] == metric]
            regressions = [item for item in metric_records if item['delta_us'] > 0]
            quantifiable = [item for item in regressions if item['percent_change'] is not None]
            fallback_summary['by_metric'][metric] = {
                'worst_relative_regression': max(quantifiable, key=lambda item: item['percent_change'], default=None),
                'worst_absolute_regression': max(regressions, key=lambda item: item['delta_us'], default=None),
                'increases_from_zero_control': [item for item in regressions if item['zero_control']]}
    output = {'schema': 'native-proof-offline-abba-analysis-v1', 'valid': True,
              'semantics': {'cost_sign': 'negative percent change is lower cost; positive is regression',
                            'pairing': 'pair1=A1/B1; pair2=A2/B2 despite reversed acquisition order',
                            'per_arm': 'median across round-level means, each with identical iteration/checkpoint counts',
                            'pooled': 'median of all A1+A2 round means versus all B1+B2 round means for the same case',
                            'directions': 'exact directions of the printed medians; no significance or noise tolerance is implied',
                            'zero_control': 'ratio is null when the control median rounds to zero; absolute delta and direction are retained',
                            'weighting': 'every case is preserved; no production weighting or TPS/capacity inference',
                            'identity': 'embedded build provenance and CSV hashes are checked; analysis does not require archived binaries to remain installed'},
              'input_directory': str(directory), 'input_provenance_sha256': digest(provenance_path),
              'input_provenance': provenance, 'resolved_options': options,
              'validation': {'successful_runs': 4, 'exact_case_keys_and_iterations': True,
                             'cases_per_run': len(cases), 'rows_per_run': len(data['a1']),
                             'total_timed_checkpoint_stat_comparisons': sum(run['timed_checkpoint_stat_comparisons'] for run in runs)},
              'direction_counts_by_mode': directions, 'fallback': fallback_summary, 'cases': results}
    return output, flat


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input-dir', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    directory, output_dir = args.input_dir.resolve(), args.output_dir.resolve()
    try:
        require(directory.is_dir(), 'input directory does not exist')
        require(directory != output_dir, 'output directory must differ from input directory')
        require(not output_dir.exists() or (output_dir.is_dir() and not any(output_dir.iterdir())),
                'output directory must be new or empty; existing results are never overwritten')
        result, rows = analyze(directory)
    except (OSError, ValueError, KeyError, TypeError, OverflowError, csv.Error) as error:
        print(json.dumps({'valid': False, 'error': str(error)}), file=sys.stderr)
        return 2
    # No aggregate is created for a missing, partial, failed, or mismatched set.
    output_dir.mkdir(parents=True, exist_ok=True)
    csv_path = output_dir / 'cases.csv'
    with csv_path.open('x', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    result['aggregate_csv_sha256'] = digest(csv_path)
    with (output_dir / 'analysis.json').open('x') as stream:
        json.dump(result, stream, indent=2, allow_nan=False)
        stream.write('\n')
    print(json.dumps({'valid': True, 'cases': result['validation']['cases_per_run'],
                      'output_directory': str(output_dir)}))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
