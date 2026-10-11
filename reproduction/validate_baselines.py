#!/usr/bin/env python3
"""Run small-geometry CRC/GC smoke for each isolated baseline, serially.

This is a basic runtime check, not a proof of baseline mechanism fidelity.
Paper sources must differ from diagnostic sources only in NAND geometry.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys

HERE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser()
parser.add_argument('variants', nargs='*', choices=('dftl', 'tpftl', 'ideal', 'leaftl'))
args = parser.parse_args()
for variant in args.variants or ['dftl', 'tpftl', 'ideal', 'leaftl']:
    stem = {'ideal': 'ftl'}.get(variant, variant)
    paper_dir = HERE / 'variants' / variant
    diagnostic_dir = HERE / 'variants-diagnostic' / variant
    paper = json.loads((paper_dir / 'build-manifest.json').read_text())
    diagnostic = json.loads((diagnostic_dir / 'build-manifest.json').read_text())
    assert diagnostic['paper_binary_sha256'] == paper['sha256']
    cfile = Path('femu/bbssd') / (stem + '.c')
    restored = (diagnostic_dir / cfile).read_text()
    for name, small, full in (('nchs', 4, 8), ('luns_per_ch', 4, 8),
                               ('blks_per_pl', 32, 256), ('pgs_per_blk', 64, 512)):
        old = f'spp->{name} = {small};'
        assert restored.count(old) == 1
        restored = restored.replace(old, f'spp->{name} = {full};')
    assert restored == (paper_dir / cfile).read_text()
    for name in (stem + '.h', 'bb.c'):
        relative = Path('femu/bbssd') / name
        assert (paper_dir / relative).read_bytes() == (diagnostic_dir / relative).read_bytes()
    print('Validating:', variant, flush=True)
    process = subprocess.run([sys.executable, str(HERE / 'run_vm_smoke.py'), '--manifest',
                              str(diagnostic_dir / 'build-manifest.json')],
                             env=dict(os.environ, VM_SMOKE_MIB='96'),
                             capture_output=True, text=True)
    (diagnostic_dir / 'validation-output.log').write_text(process.stdout + process.stderr)
    matches = re.findall(r'^Output: (.*?) acceleration:', process.stdout, re.M)
    record = {'status': 'passed' if process.returncode == 0 else 'failed',
              'paper_binary_sha256': paper['sha256'],
              'diagnostic_binary_sha256': diagnostic['sha256'],
              'only_geometry_differs': True, 'exit_code': process.returncode,
              'scope': '96 MiB namespace, six actual CRC-verified write passes, random read and GC counter',
              'limitations': 'Logical payload backend; baseline internals not exhaustively checked.'}
    if matches:
        record['run_directory'] = matches[-1]
        method_path = Path(matches[-1]) / 'method.json'
        if method_path.exists():
            record['method'] = json.loads(method_path.read_text())
    (paper_dir / 'validation.json').write_text(json.dumps(record, indent=2, ensure_ascii=False) + '\n')
    print(variant, record['status'], flush=True)
