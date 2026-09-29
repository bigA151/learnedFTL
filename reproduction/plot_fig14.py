#!/usr/bin/env python3
"""Plot measured FIO data in the three-panel layout of LearnedFTL Fig. 14.

Uses only rows collected by run_fio.py. Missing baselines remain blank.
"""
import argparse
import csv
import hashlib
import json
import math
from collections import defaultdict
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

BASE = Path(__file__).resolve().parent
PATTERNS = ['randread', 'read', 'randwrite', 'write']
LABEL = {'randread': 'RandRead', 'read': 'SeqRead',
         'randwrite': 'RandWrite', 'write': 'SeqWrite'}
VARIANTS = ['dftl', 'tpftl', 'leaftl', 'learnedftl', 'ideal']
COLORS = {'dftl': '#5175a5', 'tpftl': '#dd8452', 'leaftl': '#55a868',
          'learnedftl': '#c44e52', 'ideal': '#8172b3'}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--variant-suffix', default='', help='e.g. -paper')
    ap.add_argument('--output', type=Path, default=BASE / 'results/fig14-measured.png')
    ap.add_argument('--require-complete', action='store_true')
    ap.add_argument('--subtitle', default='Measured on local FEMU; unavailable bars omitted')
    args = ap.parse_args()
    rows = defaultdict(list)
    with (BASE / 'results/fio_runs.csv').open(newline='') as f:
        for r in csv.DictReader(f):
            for name in VARIANTS:
                if r['variant'] == name + args.variant_suffix:
                    rows[name, r['pattern']].append(r)
    if args.require_complete:
        if args.variant_suffix != '-paper-full':
            raise SystemExit('Strict Figure 14 requires --variant-suffix=-paper-full')
        missing = []
        for name in VARIANTS:
            for pat in PATTERNS:
                folder = BASE / 'results/paper_fig14' / name / pat
                try:
                    method = json.loads((folder / 'method.json').read_text())
                    warmup = json.loads((folder / 'warmup.json').read_text())['jobs'][0]
                    expected = 30518 * 6 * 1048576
                    valid = (method.get('status') == 'complete' and
                             method.get('logical_bytes') == 30518 * 1048576 and
                             method.get('warmup_passes') == 6 and
                             method.get('repeats') == 3 and
                             method.get('fio_duration_seconds') == 60 and
                             method.get('fio_address_layout_version') == 'explicit-per-job-end-v1' and
                             warmup['error'] == 0 and warmup['write']['io_bytes'] == expected and
                             all((folder / f'run{n}-console.json').exists() for n in (1, 2, 3)))
                    for n in (1, 2, 3):
                        if not valid: break
                        report = json.loads((folder / f'run{n}-console.json').read_text())
                        jobfile = BASE / 'results' / (name + '-paper-full') / pat / f'run{n}-jobfile.fio'
                        valid = (report.get('address_layout_version') == 'explicit-per-job-end-v1' and
                                 hashlib.sha256(jobfile.read_bytes()).hexdigest() == report.get('jobfile_sha256'))
                    if not valid:
                        missing.append(f'{name}/{pat}: metadata or warmup mismatch')
                except (OSError, ValueError, KeyError, IndexError):
                    missing.append(f'{name}/{pat}: warmup or run incomplete')
                found = {int(r['run']) for r in rows[name, pat]}
                if found != {1, 2, 3}:
                    missing.append(f'{name}/{pat}: repeats {sorted(found)}')
                if any(r.get('address_layout_version') != 'explicit-per-job-end-v1' for r in rows[name, pat]):
                    missing.append(f'{name}/{pat}: old fio address layout')
        if missing:
            raise SystemExit('Incomplete Figure 14; refusing strict plot:\n' + '\n'.join(missing))
    fig, axes = plt.subplots(1, 3, figsize=(14, 4.5), constrained_layout=True)
    n = len(VARIANTS)
    width = .15
    xpos = np.arange(len(PATTERNS))
    for i, name in enumerate(VARIANTS):
        xs = xpos + (i - 2) * width
        ys = []
        es = []
        for pat in PATTERNS:
            vals = [float(r['throughput_MiB_s']) for r in rows[name, pat]]
            ys.append(np.mean(vals) if vals else np.nan)
            es.append(np.std(vals, ddof=1) if len(vals) > 1 else 0)
        axes[0].bar(xs, ys, width, yerr=es, capsize=2, label=name,
                    color=COLORS[name])
    axes[0].set_xticks(xpos, [LABEL[p] for p in PATTERNS], rotation=25)
    axes[0].set_ylabel('Throughput (MiB/s)')
    axes[0].set_title('(a) Throughput')
    axes[0].legend(fontsize=8, ncol=3)

    read_pats = ['randread', 'read']
    for i, name in enumerate(VARIANTS):
        for j, pat in enumerate(read_pats):
            sample = rows[name, pat]
            sample = [r for r in sample if r.get("access") and r.get("cmt_hit") and r.get("model_hit")]
            if name == 'ideal' and rows[name, pat]:
                axes[1].bar(j * 6 + i, 100, color=COLORS[name], edgecolor='black', linewidth=.5)
                continue
            if not sample:
                continue
            cmt = np.mean([float(r['cmt_hit']) / max(1, float(r['access'])) * 100
                           for r in sample])
            model = np.mean([float(r['model_hit']) / max(1, float(r['access'])) * 100
                             for r in sample])
            x = j * 6 + i
            axes[1].bar(x, cmt, color=COLORS[name], edgecolor='black', linewidth=.5)
            axes[1].bar(x, model, bottom=cmt, color=COLORS[name], alpha=.4,
                        edgecolor='black', linewidth=.5)
    axes[1].set_xticks([2, 8], ['RandRead', 'SeqRead'])
    axes[1].set_ylabel('CMT + model hit ratio (%)')
    axes[1].set_ylim(0, 105)
    axes[1].set_title('(b) Mapping hits (solid CMT, pale model)')

    for i, name in enumerate(VARIANTS):
        vals = []
        for pat in ['randwrite', 'write']:
            sample = rows[name, pat]
            estimates = [float(r['writes']) / (float(r['io_bytes']) / 4096)
                         for r in sample if r.get('writes') and float(r['io_bytes'])]
            vals.append(np.mean(estimates) if estimates else np.nan)
        axes[2].bar(np.arange(2) + (i - 2) * width, vals, width,
                    color=COLORS[name])
    axes[2].set_xticks([0, 1], ['RandWrite', 'SeqWrite'])
    axes[2].set_ylabel('NAND page writes / host 4-KiB writes')
    axes[2].set_yscale('log')
    axes[2].set_title('(c) Observed write amplification')
    fig.suptitle('LearnedFTL Figure 14 measurement layout\n' + args.subtitle, fontsize=11)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.output, dpi=170)
    fig.savefig(args.output.with_suffix('.pdf'))
    print(args.output)


if __name__ == '__main__':
    main()
