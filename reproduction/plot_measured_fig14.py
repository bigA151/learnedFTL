#!/usr/bin/env python3
"""Draw LearnedFTL Fig.14 layout using validated current-run measurements only.

Run summarize_results.py first. Require all 20 cases by default. The explicit
--partial option labels unfinished plots and leaves absent bars blank.
"""
import argparse
from collections import defaultdict
import csv
import hashlib
import json
import os
from pathlib import Path
import statistics
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / 'plot-libs'))
os.environ.setdefault('MPLCONFIGDIR', str(HERE / 'plot-cache'))
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import Patch

VARIANTS = ['dftl', 'tpftl', 'leaftl', 'learnedftl', 'ideal']
NAMES = ['DFTL', 'TPFTL', 'LeaFTL', 'LearnedFTL', 'ideal']
SHORT = ['D', 'TP', 'LF', 'LD', 'I']
PATTERNS = ['randread', 'read', 'randwrite', 'write']
LABELS = ['RandRead', 'SeqRead', 'RandWrite', 'SeqWrite']
COLORS = ['#4ab6df', '#f2bc26', '#999999', '#70439b', '#83b94e']
HATCHES = ['xxx', '///', '...', '', '--']
p = argparse.ArgumentParser()
p.add_argument('--partial', action='store_true')
p.add_argument('--prewarmed-writes', action='store_true', help='Label the separate post-read write cohort')
p.add_argument('--summary', type=Path, default=HERE / 'paper-summary.json')
p.add_argument('--output', type=Path, default=HERE / 'figures')
a = p.parse_args()
summary = json.loads(a.summary.read_text())
is_supplement = any(r.get('cohort','').startswith('supplemental') for r in summary['measurements'])
if is_supplement != a.prewarmed_writes:
    raise SystemExit('Supplemental write data require explicit --prewarmed-writes labeling')
groups = defaultdict(list)
for row in summary['measurements']:
    if row['diagnostic_only']: continue
    groups[row['variant'], row['pattern']].append(row)
missing = []
for v in VARIANTS:
    for pattern in PATTERNS:
        rows = groups[v, pattern]
        if len(rows) != 3 or {r['repeat'] for r in rows} != {1, 2, 3}:
            missing.append(v + '/' + pattern)
        if len({r['binary_sha256'] for r in rows}) > 1:
            raise SystemExit('Mixed source revisions: ' + v + '/' + pattern)
for v in VARIANTS:
    hashes = {r['binary_sha256'] for pattern in PATTERNS for r in groups[v, pattern]}
    if len(hashes) > 1: raise SystemExit('Inconsistent binary across patterns: ' + v)
if missing and not a.partial:
    raise SystemExit('Incomplete Fig.14 data; refusing final plot: ' + ', '.join(missing))
if not any(groups.values()): raise SystemExit('No validated measurements to plot')

def values(v, pattern, field):
    return [float(r[field]) for r in groups[v, pattern] if r[field] is not None]

def mean_error(vals):
    return (statistics.mean(vals), statistics.stdev(vals) if len(vals) > 1 else 0)

plt.rcParams.update({'font.family': 'DejaVu Sans', 'font.size': 10,
                     'axes.spines.top': False, 'axes.spines.right': False})
fig, axes = plt.subplots(1, 3, figsize=(14, 4.4))
width = .15
export = []
for i, v in enumerate(VARIANTS):
    for j, pattern in enumerate(PATTERNS):
        vals = values(v, pattern, 'throughput_MB_s')
        if vals:
            mean, error = mean_error(vals)
            axes[0].bar(j + (i - 2) * width, mean, width, yerr=error,
                        capsize=2, color=COLORS[i], hatch=HATCHES[i], edgecolor='black', linewidth=.4)
            export.append({'variant': v, 'pattern': pattern, 'metric': 'throughput_MB_s',
                           'mean': mean, 'sample_stddev': error, 'samples': len(vals)})
axes[0].set(xticks=range(4), xticklabels=LABELS, ylabel='Throughput (MB/s)',
            title='(a) Throughput')
axes[0].tick_params(axis='x', rotation=15)
fig.legend([Patch(facecolor=c, hatch=h, edgecolor='black', linewidth=.4)
            for c,h in zip(COLORS,HATCHES)], NAMES, fontsize=8, ncol=5,
            loc='upper center', bbox_to_anchor=(.5,.93), frameon=False)

for j, pattern in enumerate(PATTERNS[:2]):
    for i, v in enumerate(VARIANTS):
        rows = groups[v, pattern]
        if not rows: continue
        # Ideal has a resident full mapping table; its log has no cache-access
        # counter. Show the design's 100% lookup coverage, not a measured ratio.
        if v == 'ideal':
            cmt, model = 100, 0
        elif v == 'leaftl':
            hits = values(v, pattern, 'single_flash_read_ratio')
            if len(hits) != len(rows): raise SystemExit('Missing LeaFTL flash-read multiplicity counters')
            cmt, model = 0, statistics.mean(hits) * 100
        else:
            cmt = statistics.mean(values(v, pattern, 'cmt_hit_ratio')) * 100
            model = statistics.mean(values(v, pattern, 'model_hit_ratio')) * 100
        x = j * 6 + i
        axes[1].bar(x, cmt, .72, color='#ffc107', edgecolor='black', linewidth=.4)
        axes[1].bar(x, model, .72, bottom=cmt, color='#49304f',
                    edgecolor='black', linewidth=.4)
        export += [{'variant': v, 'pattern': pattern, 'metric': metric,
                    'mean': value, 'sample_stddev': '', 'samples': len(rows)}
                   for metric, value in [('cmt_hit_percent', cmt), ('model_hit_percent', model)]]
axes[1].set_xticks([j * 6 + i for j in range(2) for i in range(5)], SHORT * 2)
axes[1].set(ylabel='Hit Ratio (%)', ylim=(0, 105), title='(b) Model and CMT hit ratio')
for x, label in [(2, 'RandRead'), (8, 'SeqRead')]:
    axes[1].text(x, -18, label, ha='center', va='top', clip_on=False)
axes[1].legend([Patch(facecolor='#49304f', edgecolor='black'),
                Patch(facecolor='#ffc107', edgecolor='black')], ['Model hit', 'CMT hit'], fontsize=8)

for i, v in enumerate(VARIANTS):
    for j, pattern in enumerate(PATTERNS[2:]):
        vals = values(v, pattern, 'write_amplification')
        if vals:
            mean, error = mean_error(vals)
            bar = axes[2].bar(j + (i - 2) * width, mean, width, yerr=error,
                             capsize=2, color=COLORS[i], hatch=HATCHES[i], edgecolor='black', linewidth=.4)
            axes[2].bar_label(bar, labels=[f'{mean:.2f}'], padding=3, fontsize=7)
            export.append({'variant': v, 'pattern': pattern, 'metric': 'write_amplification',
                           'mean': mean, 'sample_stddev': error, 'samples': len(vals)})
axes[2].set(xticks=range(2), xticklabels=LABELS[2:], ylabel='Write Amplification',
            title='(c) Write amplification', ylim=(0, None))
waf_values = [r['write_amplification'] for rows in groups.values() for r in rows if r['write_amplification'] is not None]
waf_log_scale = bool(a.prewarmed_writes and waf_values and max(waf_values) > 100)
if waf_log_scale:
    if min(waf_values) <= 0: raise SystemExit('Logarithmic WAF requires positive measurements')
    axes[2].set_yscale('log')
    axes[2].set_ylabel('Write Amplification (log scale)')
    axes[2].set_ylim(min(1, min(waf_values) * .6), max(waf_values) * 1.8)
elif waf_values:
    axes[2].set_ylim(0, max(waf_values) * 1.3)
for ax in axes: ax.grid(axis='y', alpha=.18); ax.set_axisbelow(True)
title = 'LearnedFTL Fig. 14 — local FEMU reproduction'
if a.prewarmed_writes: title += ' (supplement: prewarmed writes)'
fig.suptitle(title + (' (INCOMPLETE)' if missing else ''), fontsize=13)
write_note = 'writes: consecutive post-read windows on prewarmed devices' if a.prewarmed_writes else 'writes from fresh VMs'
fig.text(.5, .015, '64 jobs, 4 KiB, psync; mean ± sample SD of 3 runs. Ideal mapping coverage = 100% by design.\n'
         'D: DFTL; TP: TPFTL; LF: LeaFTL (single flash reads); LD: LearnedFTL; I: ideal. Missing cases are omitted.\n'
         'Disjoint job ranges; 60 s windows; reads after 6 SSD-size write passes; ' + write_note + '.',
         ha='center', fontsize=8)
fig.subplots_adjust(left=.065, right=.985, bottom=.28, top=.80, wspace=.32)
a.output.mkdir(parents=True, exist_ok=True)
name = 'fig14-partial' if missing else 'fig14-measured'
if a.prewarmed_writes: name = 'fig14-prewarmed-writes' + ('-partial' if missing else '')
for ext in ('png', 'pdf', 'svg'):
    fig.savefig(a.output / (name + '.' + ext), dpi=300)
with (a.output / (name + '.csv')).open('w', newline='') as out:
    w = csv.DictWriter(out, fieldnames=list(export[0])); w.writeheader(); w.writerows(export)
(a.output / (name + '-provenance.json')).write_text(json.dumps(
    {'summary': str(a.summary), 'missing': missing, 'cases': summary['cases'],
     'summary_sha256': hashlib.sha256(a.summary.read_bytes()).hexdigest(),
     'plotter_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
     'waf_axis_scale': 'log' if waf_log_scale else 'linear',
     'waf_axis_limits': list(axes[2].get_ylim()),
     'notes': summary['notes']}, indent=2, ensure_ascii=False) + '\n')
print(a.output / (name + '.png'))
