#!/usr/bin/env python3
"""Build isolated author baselines using existing paper parameter transforms.

Parameter and diagnostic transforms are imported read-only from the user's FEMU
reproduction tools. All generated sources, objects and binaries stay here.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parent / 'learnedFTL-codex-learnedftl-chinese-comments'
if not SOURCE.exists(): SOURCE = HERE.parent  # published FEMU subtree layout
SDK = Path(os.environ.get('FEMU_ROOT', '/home/zheng/FEMU-ext4'))
BUILD = SDK / 'reproduction/upstream/build-femu'
ENV = [str(SDK / 'femu-local-env.sh')]
spec = importlib.util.spec_from_file_location('existing_paper_transforms', SDK / 'reproduction/build_paper.py')
transforms = importlib.util.module_from_spec(spec)
spec.loader.exec_module(transforms)
parser = argparse.ArgumentParser()
parser.add_argument('variants', nargs='*', choices=('dftl', 'tpftl', 'ideal', 'leaftl'))
parser.add_argument('--rebuild', action='store_true', help='Compile reviewed existing sources without reapplying transforms')
parser.add_argument('--diagnostic-geometry', action='store_true',
                    help='Build 128 MiB NAND geometry for correctness/GC, not paper performance')
args = parser.parse_args()
variants = args.variants or ['dftl', 'tpftl', 'ideal', 'leaftl']
entries = json.loads((BUILD / 'compile_commands.json').read_text())
ninja = (BUILD / 'build.ninja').read_text()
first = ninja.index('build qemu-system-x86_64:')
block = ninja[first:ninja.index('\n\n', first)].splitlines()
original_inputs = shlex.split(block[0].split(': cpp_LINKER_RSP ', 1)[1].split(' |', 1)[0])
link_args = shlex.split(next(x.split(' = ', 1)[1] for x in block if x.startswith(' LINK_ARGS = ')))
for variant in variants:
    dest = HERE / ('variants-diagnostic' if args.diagnostic_geometry else 'variants') / variant
    src = dest / 'femu'
    stem = {'dftl': 'dftl', 'tpftl': 'tpftl', 'ideal': 'ftl', 'leaftl': 'leaftl'}[variant]
    cfile = src / 'bbssd' / (stem + '.c')
    hfile = src / 'bbssd' / (stem + '.h')
    bbfile = src / 'bbssd/bb.c'
    if not args.rebuild:
        if dest.exists():
            raise RuntimeError(f'Existing isolated build must be reviewed before replacing: {dest}')
        shutil.copytree(SOURCE, src)
        cfile.write_text(transforms.patched_source(variant, cfile.read_text()))
        hfile.write_text(transforms.patched_header(variant, hfile.read_text()))
        if args.diagnostic_geometry:
            content = cfile.read_text()
            for name, before, after in (('nchs', 8, 4), ('luns_per_ch', 8, 4),
                                        ('blks_per_pl', 256, 32), ('pgs_per_blk', 512, 64)):
                old = f'spp->{name} = {before};'
                assert content.count(old) == 1, old
                content = content.replace(old, f'spp->{name} = {after};')
            cfile.write_text(content)
        bb = bbfile.read_text().replace('"./ld-tpftl.h"', f'"./{stem}.h"')
        first = bb.index('static void reset_stat(')
        last = bb.index('static void bb_flip(', first)
        bbfile.write_text(bb[:first] + transforms.metric_code(variant) + bb[last:])
    else:
        assert (dest / 'build-manifest.json').exists()
    replacements = {}
    with (dest / 'build.log').open('w') as log:
        for original, new in (('ld-tpftl.c', stem + '.c'), ('bb.c', 'bb.c'), ('util.c', 'util.c')):
            entry = next(e for e in entries if e['file'].endswith('/bbssd/' + original))
            command = shlex.split(entry['command'])
            for flag in ('-MD', '-MQ', '-MF'):
                if flag in command:
                    i = command.index(flag)
                    del command[i:i + (1 if flag == '-MD' else 2)]
            output = dest / (new + '.o')
            command[command.index('-o') + 1] = str(output)
            command[-1] = str(src / 'bbssd' / new)
            subprocess.run(ENV + command, cwd=BUILD, stdout=log, stderr=subprocess.STDOUT, check=True)
            replacements[entry['output']] = str(output)
        inputs = [replacements.get(x, x) for x in original_inputs]
        binary = dest / ('qemu-' + variant + '.staging')
        subprocess.run(ENV + ['c++', '-m64', '-mcx16', '-o', str(binary), *inputs, *link_args],
                       cwd=BUILD, stdout=log, stderr=subprocess.STDOUT, check=True)
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    frozen_binary = dest / ('qemu-' + variant + '-' + digest[:16])
    binary.replace(frozen_binary)
    binary = frozen_binary
    previous = dest / 'build-manifest.json'
    if previous.exists():
        old_manifest = json.loads(previous.read_text())
        shutil.copy2(previous, dest / ('build-manifest-' + old_manifest['sha256'][:16] + '.json'))
    paths = [cfile, hfile, bbfile, SDK / 'reproduction/build_paper.py', Path(__file__).resolve()]
    repair = dest / 'mechanism-repairs.patch'
    if repair.exists(): paths += [repair, HERE / 'repair_leaftl.py']
    manifest = {'variant': variant, 'binary': str(binary), 'base_build': str(BUILD),
                'sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
                'sources': {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},
                'notes': 'Existing parameter/minimal diagnostic corrections; baseline mechanisms require separate validation.'}
    if args.diagnostic_geometry:
        parent = json.loads((HERE / 'variants' / variant / 'build-manifest.json').read_text())
        manifest.update(physical_bytes=134217728, geometry='4ch/4lun/32blk/64pg/4KiB',
                        paper_binary_sha256=parent['sha256'],
                        diagnostic_only=True, geometry_overrides=['nchs', 'luns_per_ch', 'blks_per_pl', 'pgs_per_blk'])
    (dest / 'build-manifest.json').write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + '\n')
    print(variant, binary, flush=True)
