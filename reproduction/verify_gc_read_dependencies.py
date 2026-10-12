#!/usr/bin/env python3
"""Verify the saved before/after NAND timeline regression and its provenance."""
import hashlib
import json
from pathlib import Path
import re

HERE = Path(__file__).resolve().parent
BASE = HERE if (HERE / 'native-parallel-gc-fixed').exists() else HERE / 'evidence/current-native'
folders = [BASE / 'native-parallel-gc-full-continued', BASE / 'native-parallel-gc-fixed']
pattern = re.compile(r'submitted=(\d+) max_latency_ms=([\d.]+) free=(\d+) gc=(\d+) nand_writes=(\d+) WAF=([\d.]+)')
logs, records, sources = [], [], []
for folder in folders:
    manifest = json.loads((folder / 'native-manifest.json').read_text())
    assert manifest['status'] == 'passed'
    assert manifest['sanitizer_set'] == ['address', 'undefined', 'float-cast-overflow']
    arguments = manifest['compile_arguments']
    for flag in ('-DNATIVE_FULL_GEOMETRY', '-DNATIVE_CONTINUE_AFTER_30S', '-UNDEBUG',
                 '-fsanitize=address,undefined,float-cast-overflow'):
        assert flag in arguments, f'Missing compile flag {flag}'
    snapshot = folder / 'source-snapshot'
    index = json.loads((snapshot / 'index.json').read_text())
    by_name = {}
    for original, digest in manifest['sources'].items():
        entry = index[original]
        assert entry['sha256'] == digest
        source = snapshot / entry['snapshot']
        assert source.resolve().parent == snapshot.resolve()
        assert hashlib.sha256(source.read_bytes()).hexdigest() == digest
        by_name[Path(original).name] = digest
    sources.append(by_name)
    text = (folder / 'native-results.log').read_text()
    assert 'runtime error:' not in text and 'ERROR: AddressSanitizer' not in text
    assert 'geometry=8ch/8lun/512pg/256blocks' in text
    assert 'prewarm=6 ' in text
    assert 'PASS concurrent virtual-time consistency' in text
    points = {}
    for line in text.splitlines():
        if line.startswith('submitted='):
            match = pattern.fullmatch(line)
            assert match, line
            submitted, latency, free, gc, writes, waf = match.groups()
            points[int(submitted)] = (float(latency), int(free), int(gc), int(writes), float(waf))
    records.append(points)
    logs.append({'path': str((folder / 'native-results.log').relative_to(HERE)),
                 'sha256': hashlib.sha256((folder / 'native-results.log').read_bytes()).hexdigest()})
for name in ('native_parallel_gc_test.c', 'native_gc_test.c', 'ld-tpftl.h', 'util.c', 'pqueue.c'):
    assert sources[0][name] == sources[1][name], f'Changed fixture or dependency: {name}'
assert sources[0]['ld-tpftl.c'] != sources[1]['ld-tpftl.c']
current = HERE.parent / 'learnedFTL-codex-learnedftl-chinese-comments/bbssd/ld-tpftl.c'
if not current.exists():
    current = HERE.parent / 'bbssd/ld-tpftl.c'
assert hashlib.sha256(current.read_bytes()).hexdigest() == sources[1]['ld-tpftl.c'], 'Current FTL differs from validated candidate'
before, after = records
assert set(after) == set(range(1, 4097, 64)), 'Need every 64-request checkpoint'
assert set(after) <= set(before)
for checkpoint in after:
    assert before[checkpoint][1:] == after[checkpoint][1:], f'Changed work counters at {checkpoint}'
old_max = max(p[0] for p in before.values())
new_max = max(p[0] for p in after.values())
assert old_max > 30000 and new_max < 30000
result = {'status': 'passed', 'before_max_virtual_latency_ms': old_max,
          'after_max_virtual_latency_ms': new_max,
          'identical_common_checkpoints': len(after),
          'checks': 'Same full-geometry fixture, six prewarm passes, sanitizers and all source snapshots verified; GC/free-line/NAND-write/WAF counters identical at 64 checkpoints. Virtual queued latency crosses below 30s. Not a throughput measurement.',
          'logs': logs, 'before_source_sha256': sources[0]['ld-tpftl.c'],
          'after_source_sha256': sources[1]['ld-tpftl.c']}
(HERE / 'gc-read-dependency-verification.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result, indent=2))
