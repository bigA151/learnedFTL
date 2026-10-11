#!/usr/bin/env python3
"""Summarize completed raw cases, keeping diagnostics out of final comparisons."""
import argparse
import configparser
import csv
import hashlib
import json
from pathlib import Path
import statistics

HERE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser()
parser.add_argument('--include-diagnostic', action='store_true')
args = parser.parse_args()
rows, cases = [], []
for path in sorted(HERE.glob('paper-*/method.json')):
    method = json.loads(path.read_text())
    case = {'directory': str(path.parent), 'status': method['status']}
    cases.append(case)
    diagnostic = (path.parent / 'DIAGNOSTIC-ONLY.txt').exists()
    case['diagnostic_only'] = diagnostic
    if method['status'] != 'complete' or diagnostic and not args.include_diagnostic:
        continue
    try:
        binary = path.parent / 'qemu-binary'
        assert hashlib.sha256(binary.read_bytes()).hexdigest() == method['build']['sha256']
        snapshot = path.parent / 'source-snapshot'
        index = json.loads((snapshot / 'index.json').read_text())
        for original, digest in method['build']['sources'].items():
            entry = index[original]
            assert entry['sha256'] == digest
            assert hashlib.sha256((snapshot / entry['snapshot']).read_bytes()).hexdigest() == digest
        if method['prewarm_passes']:
            warmup = json.loads((path.parent / 'prewarm.json').read_text())
            assert all(j['error'] == 0 for j in warmup['jobs'])
            assert sum(j['write']['io_bytes'] for j in warmup['jobs']) == method['prewarm_passes'] * method['logical_bytes']
        jobfile = configparser.ConfigParser()
        jobfile.read(path.parent / 'measure.fio')
        assert len(jobfile.sections()) - 1 == method['measurement']['jobs'] == 64
        assert len(method['metrics']) == 3
        kind = 'write' if method['pattern'].endswith('write') else 'read'
        rates = []
        for repeat, metric in enumerate(method['metrics'], 1):
            data = json.loads((path.parent / f'run{repeat}.json').read_text())
            jobs = data['jobs']
            assert all(j['error'] == 0 for j in jobs)
            total_bytes = sum(j[kind]['io_bytes'] for j in jobs)
            assert total_bytes > 0 and total_bytes % 4096 == 0
            rate = sum(j[kind]['io_bytes'] * 1000 / j[kind]['runtime'] for j in jobs)
            count = sum(j[kind]['total_ios'] for j in jobs)
            mean_us = sum(j[kind]['lat_ns']['mean'] * j[kind]['total_ios'] for j in jobs) / count / 1000
            access = int(metric['access'])
            cmt, model, miss = (int(metric[k]) for k in ('cmt_hit', 'model_hit', 'cmt_miss'))
            variant = method['variant']
            if variant in ('learnedftl', 'LearnedFTL current workspace'):
                assert access == cmt + model + miss
            row = {'variant': variant, 'pattern': method['pattern'], 'repeat': repeat,
                   'diagnostic_only': diagnostic, 'throughput_MB_s': rate / 1e6,
                   'host_io_bytes': total_bytes, 'mean_latency_us': mean_us,
                   'cmt_hit_ratio': cmt / access if access else None,
                   'model_hit_ratio': model / access if access else None,
                   'translation_miss_ratio': miss / access if access else None,
                   'mapping_accesses': access, 'gc_counter': int(metric['gc']),
                   'nand_writes': int(metric['writes']),
                   'write_amplification': int(metric['writes']) / (total_bytes / 4096) if kind == 'write' else None,
                   'energy_model_units': sum(float(metric[k]) for k in ('read_energy', 'write_energy', 'erase_energy')),
                   'binary_sha256': method['build']['sha256'], 'directory': str(path.parent)}
            rows.append(row)
            rates.append(row['throughput_MB_s'])
        case['mean_throughput_MB_s'] = statistics.mean(rates)
        case['sample_stdev_MB_s'] = statistics.stdev(rates)
        case['validated_raw_files'] = True
    except (AssertionError, KeyError, ValueError, OSError, ZeroDivisionError) as exc:
        rows = [r for r in rows if r['directory'] != str(path.parent)]
        case['validation_error'] = f'{type(exc).__name__}: {exc}'

payload = {'notes': ['MB/s uses decimal megabytes; latency is fio total latency.',
                     'Repeats share one prewarmed VM; they are not independent devices.',
                     'Energy is the source model counter sum, not measured host energy.',
                     'A successful fio run does not prove all FTL mechanisms correct.'],
           'cases': cases, 'measurements': rows}
(HERE / 'paper-summary.json').write_text(json.dumps(payload, indent=2, ensure_ascii=False) + '\n')
with (HERE / 'paper-summary.csv').open('w', newline='') as output:
    if rows:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
print(json.dumps({'completed_measurement_rows': len(rows), 'cases': cases}, ensure_ascii=False, indent=2))
