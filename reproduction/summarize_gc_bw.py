#!/usr/bin/env python3
"""Aggregate fio bandwidth log across every job, once per second.

fio's --per_job_logs=0 can still emit one row per job per interval. Values are
KiB/s; treating the final row as device throughput is incorrect.
"""
import argparse
import csv
import json
from collections import defaultdict
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('result', type=Path, help='one run_gc_diag.sh result directory')
args = parser.parse_args()
root = args.result
method = json.loads((root / 'method.json').read_text())
job = json.loads((root / 'fio.json').read_text())['jobs'][0]
assert method['fio_returncode'] == 0 and job['error'] == 0
assert (root / 'fio-bw.log').exists(), 'rerun with GC_BW_LOG=1'
by_second = defaultdict(lambda: [0, 0])
with (root / 'fio-bw.log').open() as source:
    for raw in source:
        if not raw.strip():
            continue
        timestamp, kib_per_second, *_ = raw.split(',')
        second = (int(timestamp.strip()) + 500) // 1000
        if second < 1 or second > method['fio_seconds']:
            continue
        item = by_second[second]
        item[0] += int(kib_per_second.strip())
        item[1] += 1
expected_seconds = set(range(1, method['fio_seconds'] + 1))
missing_seconds = sorted(expected_seconds - set(by_second))
for second in missing_seconds:
    by_second[second] = [0, 0]  # No log row; keep visible, do not claim measured zero I/O.
out = root / 'fio-bw-per-second.csv'
with out.open('w', newline='') as f:
    writer = csv.writer(f)
    writer.writerow(('second', 'job_samples', 'total_mib_per_s'))
    for second in sorted(by_second):
        total_kib, samples = by_second[second]
        writer.writerow((second, samples, f'{total_kib / 1024:.6f}'))
values = {second: by_second[second][0] / 1024 for second in by_second}
n = min(10, method['fio_seconds'])
summary = {
    'result': str(root.resolve()),
    'seconds': len(values),
    'missing_log_seconds': missing_seconds,
    'fio_jobs': method['fio_jobs'],
    'fio_mean_mib_per_s': job['write']['bw_bytes'] / 1048576,
    'log_mean_mib_per_s': sum(values.values()) / len(values),  # Missing log seconds provisionally zero; cross-check fio JSON.
    'first_window_mib_per_s': sum(values[i] for i in range(1, n + 1)) / n,
    'last_window_mib_per_s': sum(values[i] for i in range(method['fio_seconds'] - n + 1, method['fio_seconds'] + 1)) / n,
    'window_seconds': n,
    'last_window_full_sample_seconds': sum(by_second[i][1] == method['fio_jobs'] for i in range(method['fio_seconds'] - n + 1, method['fio_seconds'] + 1)),
    'min_job_samples_in_second': min(x[1] for x in by_second.values()),
    'max_job_samples_in_second': max(x[1] for x in by_second.values()),
}
assert abs(summary['fio_mean_mib_per_s'] - summary['log_mean_mib_per_s']) / summary['fio_mean_mib_per_s'] < 0.05, 'log and fio disagree by >=5%'
(root / 'fio-bw-summary.json').write_text(json.dumps(summary, indent=2) + '\n')
print(json.dumps(summary, ensure_ascii=False))
