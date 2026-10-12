#!/usr/bin/env python3
"""Recompile this workspace's FTL; link against read-only FEMU build objects.

This never modifies the existing FEMU checkout, binaries or experiment results.
The binary and manifest identify the exact source incorporated in this build.
"""
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parent / 'learnedFTL-codex-learnedftl-chinese-comments'
if not SOURCE.exists(): SOURCE = HERE.parent  # published FEMU subtree layout
SDK = Path(os.environ.get('FEMU_ROOT', '/home/zheng/FEMU-ext4'))
BUILD = SDK / 'reproduction/upstream/build-femu'
ENV = [str(SDK / 'femu-local-env.sh')]
entries = json.loads((BUILD / 'compile_commands.json').read_text())
replacements = {}
with (HERE / 'femu-build.log').open('w') as log:
    for basename in ('ld-tpftl.c', 'bb.c', 'util.c'):
        entry = next(e for e in entries if e['file'].endswith('/bbssd/' + basename))
        args = shlex.split(entry['command'])
        for flag in ('-MD', '-MQ', '-MF'):
            if flag in args:
                i = args.index(flag)
                del args[i:i + (1 if flag == '-MD' else 2)]
        output = HERE / (basename + '.o')
        args[args.index('-o') + 1] = str(output)
        args[-1] = str(SOURCE / 'bbssd' / basename)
        subprocess.run(ENV + args, cwd=BUILD, stdout=log, stderr=subprocess.STDOUT, check=True)
        replacements[entry['output']] = str(output)

    ninja = (BUILD / 'build.ninja').read_text()
    first = ninja.index('build qemu-system-x86_64:')
    block = ninja[first:ninja.index('\n\n', first)].splitlines()
    inputs = shlex.split(block[0].split(': cpp_LINKER_RSP ', 1)[1].split(' |', 1)[0])
    inputs = [replacements.get(x, x) for x in inputs]
    link_args = shlex.split(next(x.split(' = ', 1)[1] for x in block if x.startswith(' LINK_ARGS = ')))
    staging = HERE / 'qemu-learnedftl-build.tmp'
    subprocess.run(ENV + ['c++', '-m64', '-mcx16', '-o', str(staging), *inputs, *link_args],
                   cwd=BUILD, stdout=log, stderr=subprocess.STDOUT, check=True)
    binary_digest = hashlib.sha256(staging.read_bytes()).hexdigest()
    binary = HERE / ('qemu-learnedftl-' + binary_digest[:16])
    # A running experiment retains its inode and snapshot; never truncate it.
    os.replace(staging, binary)
manifest = {'binary': str(binary), 'base_build': str(BUILD), 'sources': {}}
for path in (SOURCE / 'bbssd').glob('*'):
    if path.suffix in ('.c', '.h'):
        manifest['sources'][str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
manifest['sha256'] = binary_digest
(HERE / 'build-manifest.json').write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + '\n')
print(binary)
