#!/usr/bin/env python3
"""Export a complete validated matrix without VM images, binaries or keys."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import shutil

HERE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser()
parser.add_argument('destination', type=Path)
args = parser.parse_args()
summary = json.loads((HERE / 'paper-summary.json').read_text())
rows = [r for r in summary['measurements'] if not r['diagnostic_only']]
assert len(rows) == 60, 'Finish and summarize the complete matrix first'
groups = Counter((r['variant'], r['pattern']) for r in rows)
assert len(groups) == 20 and set(groups.values()) == {3}
assert not any(c.get('validation_error') for c in summary['cases'] if not c['diagnostic_only'])
folders = sorted({Path(r['directory']) for r in rows})
assert len(folders) == 40, '30 independent write VMs and 10 prewarmed read VMs'
destination = args.destination.resolve()
destination.mkdir(parents=True, exist_ok=True)
index = {'scope': '60 formal windows and 30 separately labeled post-read write windows',
         'binary_verification': 'Frozen binary hashes verified locally; executable files are intentionally omitted.',
         'original_paths': 'method.json and summary retain original absolute paths; use this index to locate exported cases.',
         'cases': []}
for folder in folders:
    method = json.loads((folder / 'method.json').read_text())
    assert method['status'] == 'complete'
    assert any(c['directory'] == str(folder) and c.get('validated_raw_files') for c in summary['cases'])
    assert hashlib.sha256((folder / 'qemu-binary').read_bytes()).hexdigest() == method['build']['sha256']
    relative = folder.name
    target = destination / relative
    assert target != folder, 'Export into a separate evidence directory'
    names = ['method.json', 'runner.py', 'qemu.log', 'progress.jsonl', 'measure.fio']
    if method['prewarm_passes']:
        names += ['prewarm.json', 'prewarm.err']
    for repeat in method['measurement']['repeat_indices']:
        names += [f'run{repeat}.json', f'run{repeat}.err']
    if method.get('shared_read_probe_requested'):
        assert method['shared_read_probe']['diagnostic_only']
        names += ['shared-read-probe.fio', 'shared-read-probe.json', 'shared-read-probe.err']
    if method['pattern'].endswith('read'):
        probe = method['steady_write_probe']
        assert probe['diagnostic_only'] and probe['repeat_indices'] == [1, 2, 3]
        names += ['steady-write.fio']
        for repeat in (1, 2, 3):
            names += [f'steady-write{repeat}.json', f'steady-write{repeat}.err']
    snapshot = json.loads((folder / 'source-snapshot/index.json').read_text())
    names += ['source-snapshot/index.json']
    for original, digest in method['build']['sources'].items():
        entry = snapshot[original]
        assert entry['sha256'] == digest
        source = folder / 'source-snapshot' / entry['snapshot']
        assert source.resolve().parent == (folder / 'source-snapshot').resolve()
        assert hashlib.sha256(source.read_bytes()).hexdigest() == digest
        names.append('source-snapshot/' + entry['snapshot'])
    hashes = {}
    for name in names:
        source, output = folder / name, target / name
        assert source.is_file() and not source.is_symlink()
        output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, output)
        hashes[name] = hashlib.sha256(output.read_bytes()).hexdigest()
    index['cases'].append({'original_directory': str(folder), 'exported_directory': relative,
                           'variant': method['variant'], 'pattern': method['pattern'],
                           'binary_sha256': method['build']['sha256'], 'files_sha256': hashes})
(destination / 'index.json').write_text(json.dumps(index, indent=2, ensure_ascii=False) + '\n')
print(f'Exported {len(folders)} cases to {destination}')
