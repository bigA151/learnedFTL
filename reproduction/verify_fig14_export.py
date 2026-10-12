#!/usr/bin/env python3
"""Verify the published raw-evidence archive without requiring VM binaries.

This checks archive integrity and matrix coverage. Full local measurement
validation is performed by summarize_results.py before the archive is exported.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('directory', type=Path, nargs='?',
                    default=Path(__file__).resolve().parent / 'evidence/fig14')
args = parser.parse_args()
root = args.directory.resolve()
index = json.loads((root / 'index.json').read_text())
assert len(index['cases']) == 40, 'Expected 30 fresh-write and 10 prewarmed-read VMs'
formal = Counter()
supplement = Counter()
binary_hashes = defaultdict(set)
directories = set()
files_checked = 0
for case in index['cases']:
    folder = (root / case['exported_directory']).resolve()
    assert folder.parent == root and folder not in directories, 'Invalid or duplicate case path'
    directories.add(folder)
    recorded = case['files_sha256']
    actual = {str(p.relative_to(folder)) for p in folder.rglob('*') if p.is_file()}
    assert actual == set(recorded), 'Archive has missing or unindexed files'
    for relative, expected in recorded.items():
        path = folder / relative
        assert not path.is_symlink() and path.resolve().is_relative_to(folder)
        assert path.name != 'qemu-binary' and path.suffix not in {'.qcow2', '.iso', '.pem'}
        assert hashlib.sha256(path.read_bytes()).hexdigest() == expected, str(path)
        files_checked += 1
    method = json.loads((folder / 'method.json').read_text())
    assert method['status'] == 'complete'
    variant, pattern = method['variant'], method['pattern']
    assert (variant, pattern) == (case['variant'], case['pattern'])
    assert method['build']['sha256'] == case['binary_sha256']
    binary_hashes[variant].add(case['binary_sha256'])
    repeats = method['measurement']['repeat_indices']
    assert (repeats == [1, 2, 3] if pattern.endswith('read') else repeats in [[1], [2], [3]])
    for repeat in repeats:
        formal[variant, pattern, repeat] += 1
        jobs = json.loads((folder / f'run{repeat}.json').read_text())['jobs']
        assert all(j['error'] == 0 for j in jobs)
    if pattern.endswith('read'):
        assert method['prewarm_passes'] == 6
        jobs = json.loads((folder / 'prewarm.json').read_text())['jobs']
        assert all(j['error'] == 0 for j in jobs)
        assert sum(j['write']['io_bytes'] for j in jobs) == 6 * method['logical_bytes']
        probe = method['steady_write_probe']
        assert probe['diagnostic_only'] and probe['repeat_indices'] == [1, 2, 3]
        for repeat in probe['repeat_indices']:
            supplement[variant, probe['pattern'], repeat] += 1
            jobs = json.loads((folder / f'steady-write{repeat}.json').read_text())['jobs']
            assert all(j['error'] == 0 for j in jobs)
    else:
        assert method['prewarm_passes'] == 0
variants = {'learnedftl', 'dftl', 'tpftl', 'leaftl', 'ideal'}
assert set(binary_hashes) == variants and all(len(h) == 1 for h in binary_hashes.values())
assert set(formal) == {(v, p, n) for v in variants
                       for p in ('read', 'randread', 'write', 'randwrite') for n in (1, 2, 3)}
assert set(formal.values()) == {1}
assert set(supplement) == {(v, p, n) for v in variants
                           for p in ('write', 'randwrite') for n in (1, 2, 3)}
assert set(supplement.values()) == {1}
print(json.dumps({'status': 'passed', 'archive_cases': 40, 'formal_windows': 60,
                  'supplemental_windows': 30, 'files_sha256_verified': files_checked,
                  'scope': 'Published archive integrity and coverage; binaries verified before export.'},
                 ensure_ascii=False, indent=2))
