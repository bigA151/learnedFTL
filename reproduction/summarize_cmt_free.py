#!/usr/bin/env python3
"""Validate sampled queue contents and summarize an actual CMT diagnostic run."""
import csv
import json
import re
import sys
from pathlib import Path


def summarize(run):
    run = Path(run).resolve()
    snaps = list(csv.DictReader((run / 'cmt-free-snapshots.csv').open()))
    assert snaps, 'No snapshots'
    for s in snaps:
        for k in s.keys() - {'reason'}:
            s[k] = int(s[k])
        assert s['free_count'] + s['used_count'] == s['capacity']
        assert s['walk_count'] == s['free_count']
    by_id = {s['snapshot_id']: s for s in snaps}
    assert len(by_id) == len(snaps)
    seen = {k: set() for k in by_id}
    samples = {k: [] for k in by_id}
    with (run / 'cmt-free-entries.csv').open() as f:
        for row in csv.DictReader(f):
            row = {k: int(v) for k, v in row.items()}
            sid, slot = row['snapshot_id'], row['slot_id']
            assert sid in by_id
            assert row['queue_position'] == len(seen[sid])
            assert 0 <= slot < by_id[sid]['capacity'] and slot not in seen[sid]
            assert row['lpn'] == row['ppn'] == (1 << 64) - 1
            assert row['dirty'] == row['prefetch'] == row['next_avail_time'] == 0
            seen[sid].add(slot)
            if len(samples[sid]) < 12:
                samples[sid].append(slot)
    for sid, s in by_id.items():
        assert len(seen[sid]) == s['free_count'], f'Incomplete contents {sid}'
    log = (run / 'qemu.log').read_text(errors='replace')
    assert re.search(r'CMT_FREE_DONE.*dropped=0 ', log), 'Missing/incomplete dump'
    assert not re.search(r'CMT_FREE_INVARIANT|CMT_FREE_DUMP_ERROR', log)
    method = json.loads((run / 'method.json').read_text())
    assert method['fio_returncode'] == method['report_returncode'] == 0
    fio = json.loads((run / 'fio.json').read_text())
    assert all(j['error'] == 0 for j in fio['jobs'])
    measured = [s for s in snaps if s['phase'] == snaps[-1]['phase']]
    first, last = measured[0], measured[-1]
    assert first['reason'] == 'before_fio' and last['reason'] == 'after_fio'
    assert last['host_write_pages'] * 4096 == sum(j['write']['io_bytes'] for j in fio['jobs'])
    assert last['free_count'] == first['free_count'] - last['allocations'] + last['returns']
    assert any(s['reason'] == 'first_empty' for s in measured), 'Cache did not fill'
    assert sum(s['reason'] == 'eviction_return' for s in measured) == 8
    assert sum(s['reason'] == 'allocated_after_return' for s in measured) == 8
    w = fio['jobs'][0]['write']
    result = dict(run=str(run), workload=method['fio_pattern'], capacity=last['capacity'],
                  before_free=first['free_count'], after_free=last['free_count'],
                  min_sampled_free=min(s['free_count'] for s in measured),
                  allocations=last['allocations'], returns=last['returns'],
                  host_write_pages=last['host_write_pages'], snapshots=len(snaps),
                  skipped_snapshots=0, fio_mib_s=w['bw_bytes']/1048576, fio_iops=w['iops'],
                  validation='PASS')
    (run/'cmt-free-summary.json').write_text(json.dumps(result, indent=2)+'\n')
    lines = ['# free_cmt_entry_list 实测快照', '',
             f"负载：`{method['fio_pattern']}`，容量 {last['capacity']} 个槽位。完整链表内容保存在 `cmt-free-entries.csv`。", '',
             '|快照|阶段|原因|相对测量开始/秒|已完成写页|分配次数|归还次数|free|used|队首最多12个槽位|',
             '|---|---|---|---:|---:|---:|---:|---:|---:|---|']
    for s in snaps:
        sid = s['snapshot_id']
        elapsed = (s['elapsed_ns'] - first['elapsed_ns']) / 1e9
        lines.append(f"|{sid}|{s['phase']}|{s['reason']}|{elapsed:.6f}|{s['host_write_pages']}|{s['allocations']}|{s['returns']}|{s['free_count']}|{s['used_count']}|{', '.join(map(str,samples[sid])) or '空链表'}|")
    lines += ['', '**验证：PASS。** 每份快照的完整行数、槽位唯一性、清空字段、计数守恒，以及主机写页数与 fio 字节数一致。',
              '', '这是受仪器开销影响的新盘短诊断，不是论文稳态性能结果。空闲队列暂时为零不等于 NAND 空间耗尽。']
    (run/'CMT_FREE_CONTENTS.zh-CN.md').write_text('\n'.join(lines)+'\n')
    print(json.dumps(result, ensure_ascii=False))
    return result


if __name__ == '__main__':
    for arg in sys.argv[1:]:
        summarize(arg)
