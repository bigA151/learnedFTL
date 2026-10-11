#!/usr/bin/env python3
"""Compile and execute current production source using the existing FEMU SDK."""
import json
import hashlib
import os
from pathlib import Path
import shlex
import subprocess

HERE = Path(__file__).resolve().parent
test_source = Path(os.environ.get('FTL_TEST_SOURCE', str(
    HERE.parent / 'learnedFTL-codex-learnedftl-chinese-comments/bbssd/ld-tpftl.c'))).resolve()
SDK = Path(os.environ.get('FEMU_ROOT', '/home/zheng/FEMU-ext4'))
BUILD = SDK / 'reproduction/upstream/build-femu'
entries = json.loads((BUILD / 'compile_commands.json').read_text())
entry = next(e for e in entries if e['file'].endswith('/ld-tpftl.c'))
args = shlex.split(entry['command'])
for flag in ('-MD', '-MQ', '-MF'):
    if flag in args:
        i = args.index(flag)
        del args[i:i + (1 if flag == '-MD' else 2)]
del args[args.index('-o'):args.index('-o') + 2]
args.remove('-c')
harness = Path(os.environ.get('NATIVE_HARNESS', str(HERE / 'native_gc_test.c'))).resolve()
output_dir = Path(os.environ.get('NATIVE_OUTPUT', str(HERE))).resolve()
output_dir.mkdir(exist_ok=True)
args[-1] = str(harness)
args += ['-DLEARNED_FTL_SOURCE="' + str(test_source) + '"']
if os.environ.get('SANITIZE') == '1':
    args += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
args += ['-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
         str(HERE.parent / 'learnedFTL-codex-learnedftl-chinese-comments/lib/pqueue.c'),
         str(HERE.parent / 'learnedFTL-codex-learnedftl-chinese-comments/bbssd/util.c'),
         '-lglib-2.0', '-lm', '-o', str(output_dir / 'native_gc_test')]
env = [str(SDK / 'femu-local-env.sh')]
with (output_dir / 'native-build.log').open('w') as log:
    result = subprocess.run(env + args, cwd=BUILD, stdout=log, stderr=subprocess.STDOUT)
if result.returncode:
    raise SystemExit((output_dir / 'native-build.log').read_text())
run_env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
result = subprocess.run(env + [str(output_dir / 'native_gc_test')], cwd=HERE, env=run_env,
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
(output_dir / 'native-results.log').write_text(result.stdout)
manifest = {'status': 'passed' if result.returncode == 0 else 'failed',
            'sanitizers': os.environ.get('SANITIZE') == '1', 'seed': 20261011,
            'geometry': '4ch/4lun/32blk/64pg/4KiB; 512 mappings per TP; CMT 64',
            'binary_sha256': hashlib.sha256((HERE / 'native_gc_test').read_bytes()).hexdigest(),
            'sources': {}}
for path in (harness, HERE / 'native_gc_test.c',
             test_source, test_source.with_suffix('.h')):
    manifest['sources'][str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
(output_dir / 'native-manifest.json').write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + '\n')
print(result.stdout)
raise SystemExit(result.returncode)
