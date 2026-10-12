#!/usr/bin/env python3
"""Summarize completed raw cases, keeping diagnostics out of final comparisons."""
import argparse
import configparser
import csv
import hashlib
import json
from pathlib import Path
import re
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
        assert hashlib.sha256((path.parent / 'runner.py').read_bytes()).hexdigest() == method['runner_sha256'], 'Runner snapshot hash'
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
        settings = jobfile['global']
        for key, value in {'rw': method['pattern'], 'bs': '4k',
                           'ioengine': 'psync', 'direct': '1',
                           'time_based': '1', 'runtime': '60'}.items():
            assert settings[key] == value, f'Measurement setting {key}'
        region = 476 * 1024**2
        stride = region + (4096 if method['pattern'] == 'read' else 0)
        for job in range(64):
            settings = jobfile[f'job{job:02d}']
            assert int(settings['offset']) == job * stride, 'Job offset'
            assert int(settings['size']) == region, 'Job size'
            assert int(settings['filesize']) == job * stride + region, 'Job end'
        repeat_indices = method['measurement'].get('repeat_indices', [1, 2, 3])
        assert len(method['metrics']) == len(repeat_indices)
        raw_metrics = [dict(re.findall(r'(\w+)=([\d.]+)', line))
                       for line in re.findall(r'METRICS (.*)', (path.parent / 'qemu.log').read_text())]
        expected_metrics = ([method['prewarm_metrics']] if method['prewarm_passes'] else []) + method['metrics']
        if 'shared_read_probe' in method:
            expected_metrics.append(method['shared_read_probe']['metrics'])
        if 'steady_write_probe' in method:
            expected_metrics.extend(method['steady_write_probe']['metrics'])
        assert raw_metrics == expected_metrics, 'Statistics order differs from raw QEMU log'
        kind = 'write' if method['pattern'].endswith('write') else 'read'
        rates = []
        for repeat, metric in zip(repeat_indices, method['metrics']):
            data = json.loads((path.parent / f'run{repeat}.json').read_text())
            jobs = data['jobs']
            assert all(j['error'] == 0 for j in jobs)
            total_bytes = sum(j[kind]['io_bytes'] for j in jobs)
            assert total_bytes > 0 and total_bytes % 4096 == 0
            rate = sum(j[kind]['io_bytes'] * 1000 / j[kind]['runtime'] for j in jobs)
            count = sum(j[kind]['total_ios'] for j in jobs)
            assert count * 4096 == total_bytes, 'FIO page/operation count'
            assert all(j[kind]['runtime'] >= 60000 for j in jobs), 'Incomplete 60s window'
            mean_us = sum(j[kind]['lat_ns']['mean'] * j[kind]['total_ios'] for j in jobs) / count / 1000
            access = int(metric['access'])
            cmt, model, miss = (int(metric[k]) for k in ('cmt_hit', 'model_hit', 'cmt_miss'))
            variant = method['variant']
            if variant in ('learnedftl', 'LearnedFTL current workspace'):
                assert access == cmt + model + miss
            row = {'variant': variant, 'pattern': method['pattern'], 'repeat': repeat,
                   'diagnostic_only': diagnostic, 'throughput_MB_s': rate / 1e6,
                   'host_io_bytes': total_bytes, 'mean_latency_us': mean_us,
                   'fio_runtime_seconds': max(j[kind]['runtime'] for j in jobs) / 1000,
                   'max_io_latency_ms': max(j[kind]['lat_ns']['max'] for j in jobs) / 1e6,
                   'cmt_hit_ratio': cmt / access if access else None,
                   'model_hit_ratio': model / access if access else None,
                   'translation_miss_ratio': miss / access if access else None,
                   'single_flash_read_ratio': int(metric['single_read']) / access if 'single_read' in metric and access else None,
                   'double_flash_read_ratio': int(metric['double_read']) / access if 'double_read' in metric and access else None,
                   'triple_flash_read_ratio': int(metric['triple_read']) / access if 'triple_read' in metric and access else None,
                   'mapping_accesses': access, 'gc_counter': int(metric['gc']),
                   'nand_writes': int(metric['writes']),
                   'write_amplification': int(metric['writes']) / (total_bytes / 4096) if kind == 'write' else None,
                   'energy_model_units': sum(float(metric[k]) for k in ('read_energy', 'write_energy', 'erase_energy')),
                   'binary_sha256': method['build']['sha256'], 'directory': str(path.parent)}
            rows.append(row)
            rates.append(row['throughput_MB_s'])
        case['mean_throughput_MB_s'] = statistics.mean(rates)
        case['sample_stdev_MB_s'] = statistics.stdev(rates) if len(rates) > 1 else None
        case['validated_raw_files'] = True
    except (AssertionError, KeyError, ValueError, OSError, ZeroDivisionError) as exc:
        rows = [r for r in rows if r['directory'] != str(path.parent)]
        case['validation_error'] = f'{type(exc).__name__}: {exc}'

payload = {'notes': ['MB/s uses decimal megabytes; latency is fio total latency.',
                     'Read repeats share a static prewarmed VM; final fresh-write repeats use independent VMs.',
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
