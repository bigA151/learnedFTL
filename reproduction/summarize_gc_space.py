#!/usr/bin/env python3
"""Summarize data-page relocation per returned FEMU line from one GC diagnosis.

Uncopied slots include invalid and never-programmed pages. This is not WAF.
"""
import argparse
import json
import re
from pathlib import Path

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('result', type=Path)
ap.add_argument('--pages-per-line', type=int, default=32768,
                help='8 channels × 8 LUN × 512 pages/block in this FEMU build')
args = ap.parse_args()
assert args.pages_per_line > 0
folder = args.result
method = json.loads((folder / 'method.json').read_text())
log = (folder / 'qemu.log').read_text(errors='replace')
summary = re.search(
    r'GC_DIAG calls=(\d+) wp=(\d+) trans=(\d+) low=(\d+) none=(\d+) '
    r'alloc=(\d+) empty=(\d+) empty_with_victim=(\d+) freed=(\d+) '
    r'user_writes=(\d+) gc_writes=(\d+)', log)
identity = re.search(r'PHYS_ID_SUMMARY writes=(\d+) copies=(\d+) reads=(\d+)', log)
donor = re.search(r'CG_DONOR_POLICY free=(\d+) open=(\d+) room=(\d+) '
                  r'eligible=(\d+) eligible_pages=(\d+) gate_open=(\d+)', log)
assert summary and identity and donor, 'missing completed GC diagnostics'
assert method['fio_returncode'] == method['report_returncode'] == 0
calls, wp, trans, low, none, alloc, empty, empty_with_victim, freed, user_writes, gc_writes = map(int, summary.groups())
physical_writes, copies, checked_reads = map(int, identity.groups())
free, open_groups, room_groups, eligible_groups, eligible_pages, gate_open = map(int, donor.groups())
assert freed > 0 and copies <= freed * args.pages_per_line
pre_measure_writes = 0
for filename in ('warmup.json', 'sparse-prepare.json', 'prepare.json'):
    source = folder / filename
    if not source.exists():
        continue
    data = json.loads(source.read_text())
    if data.get('skipped'):
        continue
    before_bytes = data['jobs'][0]['write']['io_bytes']
    assert before_bytes % 4096 == 0, f'{filename}: non-page-aligned write count'
    pre_measure_writes += before_bytes // 4096
assert physical_writes == pre_measure_writes + user_writes, 'host write accounting differs from phase counters'
assert copies == gc_writes, 'other GC write path needs separate accounting'
uncopied = freed * args.pages_per_line - copies
result = {
    'result': str(folder.resolve()),
    'address_layout': method.get('address_layout'),
    'fio_region_mib': method.get('fio_size_mib'),
    'pages_per_line': args.pages_per_line,
    'data_lines_returned': freed,
    'data_pages_copied': copies,
    'copied_pages_per_returned_line': copies / freed,
    'uncopied_slots_total': uncopied,
    'uncopied_slots_per_returned_line': uncopied / freed,
    'uncopied_slots_include_invalid_and_unwritten': True,
    'host_write_pages': user_writes,
    'pre_measure_host_write_pages': pre_measure_writes,
    'translation_gc_calls': trans,
    'low_free_gc_calls': low,
    'end_free_lines': free,
    'end_open_group_lines': open_groups,
    'end_eligible_donors': eligible_groups,
    'checked_physical_reads': checked_reads,
    'not_full_waf': True,
}
(folder / 'gc-space-summary.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result, ensure_ascii=False))
