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
parser.add_argument('--min-component-groups', type=int, default=2)
parser.add_argument('--expect-budget-reject', action='store_true')
parser.add_argument('--require-shared-line', action='store_true')
parser.add_argument('--expect-target-fail', action='store_true')
args = parser.parse_args()
p = args.result
method = json.loads((p / 'method.json').read_text())
log = (p / 'qemu.log').read_text(errors='replace')
if args.expect_target_fail:
    assert method.get('test_fail_target_at') is not None, 'target failure injection not recorded'
    assert method['fio_returncode'] != 0 and method['report_returncode'] != 0
    match = re.search(r'CG_TARGET_ALLOC_INJECT group=(\d+) allocated=(\d+)', log)
    assert match, 'target failure was not triggered'
    assert int(match.group(2)) == int(method['test_fail_target_at'])
    assert 'CG_COPY_DONE' not in log and 'CG_ERASE_DONE' not in log
    assert 'CG_COMPONENT_GC' not in log and 'PHYS_ID_MISMATCH' not in log
    print(f'PASS expected-target-fail {p} after_allocated={match.group(2)}')
    raise SystemExit(0)
if args.expect_budget_reject:
    assert method.get('test_budget_free') is not None, 'budget injection not recorded'
    assert method['fio_returncode'] != 0 and method['report_returncode'] != 0
    component = re.search(r'CG_COMPONENT hot=\d+ donor=\d+ groups=(\d+) lines=(\d+)', log)
    budget = re.search(r'CG_BUDGET free=(\d+) live_pages=(\d+) estimate_pages=(\d+) reserve_lines=(\d+) actual_free=(\d+)', log)
    reject = re.search(r'CG_BUDGET_REJECT free=(\d+) actual_free=(\d+) need=(\d+)', log)
    assert component and budget and reject, 'no preflight rejection evidence'
    assert int(budget.group(1)) < int(budget.group(4))
    assert int(reject.group(1)) == int(budget.group(1))
    assert int(reject.group(2)) == int(budget.group(5))
    assert int(reject.group(3)) == int(budget.group(4))
    assert 'CG_COMPONENT_GC' not in log and 'PHYS_ID_MISMATCH' not in log
    assert 'GC_INVARIANT' not in log and 'GC_GROUP_NO_TARGET' not in log
    print(f'PASS expected-budget-reject {p} groups={component.group(1)} need={reject.group(3)}')
    raise SystemExit(0)
fio = json.loads((p / 'fio.json').read_text())['jobs'][0]
assert method['fio_returncode'] == method['report_returncode'] == fio['error'] == 0
for bad in ('PHYS_ID_MISMATCH', 'PHYS_ID_NO_SOURCE', 'GC_EXHAUST',
            'GC_BAD_FREE_TARGET', 'GC_INVARIANT', 'GC_GROUP_NO_TARGET',
            'CG_RELATION_MISMATCH', 'CG_COMPONENT_UNSUPPORTED',
            'CG_BUDGET_REJECT'):
    assert bad not in log, bad
borrow = re.search(r'CG_BORROW attempts=(\d+) success=(\d+) pages=(\d+) paired_gc=(\d+)', log)
identity = re.search(r'PHYS_ID_SUMMARY writes=(\d+) copies=(\d+) reads=(\d+)', log)
assert borrow and identity, 'missing end-of-run counters'
model = re.search(r'MODEL_BITMAP_AUDIT attempts=(\d+) exact=(\d+) wrong=(\d+)', log)
if method.get('assert_model_bitmap'):
    assert model, 'missing model bitmap audit'
if model:
    model_attempts, model_exact, model_wrong = map(int, model.groups())
    assert model_attempts == model_exact + model_wrong
    if method.get('assert_model_bitmap'):
        assert model_attempts > 0 and model_wrong == 0, 'model bitmap claim was not validated'

attempts, success, pages, paired = map(int, borrow.groups())
writes, copies, reads = map(int, identity.groups())
assert attempts >= success == pages and writes > 0
reasons = re.search(r'CG_BORROW_REASONS attempts=(\d+) high=(\d+) low=(\d+) no_open=(\d+) no_room=(\d+) trained=(\d+) busy=(\d+) selected=(\d+) gated_with_candidate=(\d+)', log)
snapshot = re.search(r'CG_DONOR_POLICY free=(\d+) open=(\d+) room=(\d+) eligible=(\d+) eligible_pages=(\d+) gate_open=(\d+)', log)
assert reasons and snapshot, 'missing donor decision audit'
relation = re.search(r'CG_RELATION phase=after-fio links=(\d+) shared_lines=(\d+)', log)
assert relation, 'missing end-of-run relation audit'
links, shared_lines = map(int, relation.groups())
assert links >= shared_lines >= 0
r_attempts, high, low, no_open, no_room, trained, busy, selected, gated = map(int, reasons.groups())
assert r_attempts == attempts == high + low + no_open + no_room + trained + busy + selected
assert selected == success and gated <= high + low
free, open_count, room_count, eligible, eligible_pages, gate_open = map(int, snapshot.groups())
assert open_count >= room_count >= eligible >= 0
assert gate_open == int(free <= 128 and (free > 32 or method.get('allow_low_free')))
if args.require_borrow:
    assert success > 0, 'borrowing was not exercised'
if args.require_shared_line:
    shared = re.findall(r'CG_SHARED_LINE line=(\d+) owner=(\d+) members=(\d+)', log)
    assert any(int(members) >= 3 for _, _, members in shared), 'two borrowers did not share one physical line'
if args.require_pair:
    assert paired > 0 and copies > 0, 'paired mixed-line GC was not exercised'
    components = re.findall(r'CG_COMPONENT hot=(\d+) donor=(\d+) groups=(\d+) lines=(\d+)', log)
    budgets = re.findall(r'CG_BUDGET free=(\d+) live_pages=(\d+) estimate_pages=(\d+) reserve_lines=(\d+)', log)
    assert len(components) == len(budgets) == paired, 'missing component or budget preflight'
    assert any(int(groups) >= args.min_component_groups
               for _, _, groups, _ in components), 'required component size was not exercised'
    assert len(re.findall(r'CG_COMPONENT_GC hot=', log)) == paired, 'component GC did not finish'
    copy_events = [m.start() for m in re.finditer(r'CG_COPY_DONE groups=', log)]
    erase_events = [m.start() for m in re.finditer(r'CG_ERASE_DONE lines=', log)]
    if copy_events or erase_events:
        assert len(copy_events) == len(erase_events) == paired
        assert all(copy < erase for copy, erase in zip(copy_events, erase_events)), 'source erased before copy completion'
    stage_budgets = re.findall(r'CG_STAGE_BUDGET gtds=(\d+) bytes=(\d+) limit=(\d+)', log)
    if stage_budgets:
        assert len(stage_budgets) == paired
        assert all(int(gtds) > 0 and 0 < int(nbytes) <= int(limit)
                   for gtds, nbytes, limit in stage_budgets), 'staging memory budget exceeded'

    assert all(int(free) >= int(reserve) and int(estimate) >= int(live)
               for free, live, estimate, reserve in budgets)
if method.get('post_read'):
    targets = {'post-hot-read.json': 128 << 20, 'post-donor-read.json': 32 << 20}
    if method['fio_jobs'] >= 2:
        targets['post-second-hot-read.json'] = 16 << 20
    for name, nbytes in targets.items():
        entry = json.loads((p / name).read_text())['jobs'][0]
        assert entry['error'] == 0 and entry['read']['io_bytes'] == nbytes, name
print(f'PASS {p} borrow={success} paired_gc={paired} gc_copies={copies} checked_reads={reads}')
