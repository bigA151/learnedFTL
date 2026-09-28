#!/usr/bin/env python3
"""Validate one isolated cross-group diagnostic result directory."""
import argparse
import json
import re
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('result', type=Path)
parser.add_argument('--require-borrow', action='store_true')
parser.add_argument('--require-pair', action='store_true')
args = parser.parse_args()
p = args.result
method = json.loads((p / 'method.json').read_text())
fio = json.loads((p / 'fio.json').read_text())['jobs'][0]
log = (p / 'qemu.log').read_text(errors='replace')
assert method['fio_returncode'] == method['report_returncode'] == fio['error'] == 0
for bad in ('PHYS_ID_MISMATCH', 'PHYS_ID_NO_SOURCE', 'GC_EXHAUST',
            'GC_BAD_FREE_TARGET', 'GC_INVARIANT', 'GC_GROUP_NO_TARGET'):
    assert bad not in log, bad
borrow = re.search(r'CG_BORROW attempts=(\d+) success=(\d+) pages=(\d+) paired_gc=(\d+)', log)
identity = re.search(r'PHYS_ID_SUMMARY writes=(\d+) copies=(\d+) reads=(\d+)', log)
assert borrow and identity, 'missing end-of-run counters'
attempts, success, pages, paired = map(int, borrow.groups())
writes, copies, reads = map(int, identity.groups())
assert attempts >= success == pages and writes > 0
if args.require_borrow:
    assert success > 0, 'borrowing was not exercised'
if args.require_pair:
    assert paired > 0 and copies > 0, 'paired mixed-line GC was not exercised'
if method.get('post_read'):
    targets = {'post-hot-read.json': 128 << 20, 'post-donor-read.json': 32 << 20}
    if method['fio_jobs'] >= 2:
        targets['post-second-hot-read.json'] = 16 << 20
    for name, nbytes in targets.items():
        entry = json.loads((p / name).read_text())['jobs'][0]
        assert entry['error'] == 0 and entry['read']['io_bytes'] == nbytes, name
print(f'PASS {p} borrow={success} paired_gc={paired} gc_copies={copies} checked_reads={reads}')
