#!/usr/bin/env python3
"""Plot actual sampled CMT queue depletion and its first eviction/reuse pairs."""
import csv
import sys
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

runs = [Path(p) for p in sys.argv[2:]]
fig, axes = plt.subplots(len(runs), 2, figsize=(11, 3.7*len(runs)), squeeze=False)
for i, run in enumerate(runs):
    with (run/'cmt-free-snapshots.csv').open() as f:
        rows = list(csv.DictReader(f))
    rows = [s for s in rows if s['phase'] == rows[-1]['phase']]
    first = next(s for s in rows if s['reason'] == 'allocation_milestone')
    t0 = int(first['elapsed_ns'])
    filling = [s for s in rows if s['reason'] in ('allocation_milestone', 'first_empty', 'periodic') and int(s['elapsed_ns'])-t0 < 1500000000]
    ax = axes[i,0]
    ax.plot([(int(s['elapsed_ns'])-t0)/1e9 for s in filling],
            [int(s['free_count']) for s in filling], 'o-', color='#23659e', markersize=4)
    ax.set(title=f'{"Sequential" if i==0 else "Random"} write: filling CMT',
           xlabel='Seconds since first cache allocation (instrumented)', ylabel='Free cache slots')
    ax.ticklabel_format(style='plain', axis='y')
    ax.grid(alpha=.25)
    transition = [s for s in rows if s['reason'] in ('first_empty','eviction_return','allocated_after_return')]
    t1=int(transition[0]['elapsed_ns'])
    ax=axes[i,1]
    ax.plot([(int(s['elapsed_ns'])-t1)/1e3 for s in transition],
            [int(s['free_count']) for s in transition], 'o-', color='#c55a19', markersize=4)
    ax.set(title='First 8 eviction / allocation pairs', xlabel='Microseconds since first empty snapshot', ylabel='Free cache slots', yticks=[0,1])
    ax.grid(alpha=.25)
fig.suptitle('ld-tpftl: free_cmt_entry_list snapshots (64 jobs, 4 KiB, fresh FEMU)')
fig.tight_layout(rect=(0,0,1,.96))
fig.savefig(sys.argv[1], dpi=170)
fig.savefig(str(Path(sys.argv[1]).with_suffix('.pdf')))
