#!/usr/bin/env python3
"""Run all Fig.14 cases serially, resuming only immutable verified builds.

Fresh write repeats use independent VMs to avoid accidentally measuring the
second/third window after the first has already changed the flash state.
Read windows share an unchanged device after six matching prewarm passes.
"""
import datetime
import fcntl
import json
from pathlib import Path
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
VARIANTS = ['learnedftl', 'dftl', 'tpftl', 'leaftl', 'ideal']
state_path = HERE / 'fig14-matrix-state.json'
lock = (HERE / 'fig14-matrix.lock').open('w')
fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
state = {'status': 'running', 'started_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
         'tasks': [], 'notes': 'Writes: fresh VMs. Reads: three windows after six prewarm passes.'}
def save(): state_path.write_text(json.dumps(state, indent=2, ensure_ascii=False) + '\n')
def existing(v, pattern, repeat, sha):
    for path in HERE.glob(f'paper-{v}-{pattern}-*/method.json'):
        if (path.parent / 'DIAGNOSTIC-ONLY.txt').exists(): continue
        m = json.loads(path.read_text())
        ids = m['measurement'].get('repeat_indices', [1, 2, 3])
        if m['status'] == 'complete' and m['build']['sha256'] == sha and ids == repeat:
            return str(path.parent)
    return None
def maintenance_checkpoint():
    while (HERE / 'MATRIX_HOLD').exists():
        state['status'] = 'waiting_for_local_maintenance'; save()
        time.sleep(5)
    state['status'] = 'running'
try:
    for pattern in ('write', 'randwrite', 'read', 'randread'):
        for v in VARIANTS:
            manifest_path = HERE / ('build-manifest.json' if v == 'learnedftl' else f'variants/{v}/build-manifest.json')
            manifest = json.loads(manifest_path.read_text())
            for repeat in ([[1], [2], [3]] if pattern.endswith('write') else [[1, 2, 3]]):
                maintenance_checkpoint()
                record = {'variant': v, 'pattern': pattern, 'repeat_indices': repeat,
                          'binary_sha256': manifest['sha256']}
                state['tasks'].append(record)
                prior = existing(v, pattern, repeat, manifest['sha256'])
                if prior:
                    record.update(status='complete', directory=prior, resumed=True); save(); continue
                label = f'{v}-{pattern}-' + '-'.join(map(str, repeat))
                record.update(status='running', console_log=str(HERE / (label + '-matrix-' + datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '.log')))
                save(); print('Running', label, flush=True)
                command = [sys.executable, str(HERE / 'run_paper_case.py'), pattern, '--variant', v]
                if len(repeat) == 1: command += ['--repeat-index', str(repeat[0])]
                if pattern == 'read': command += ['--shared-read-probe']
                with Path(record['console_log']).open('w') as out:
                    result = subprocess.run(command, stdout=out, stderr=subprocess.STDOUT)
                record['exit_code'] = result.returncode
                if result.returncode: raise RuntimeError('Failed: ' + label)
                directory = existing(v, pattern, repeat, manifest['sha256'])
                assert directory, 'Completed process has no matching terminal evidence'
                record.update(status='complete', directory=directory); save()
                subprocess.run([sys.executable, str(HERE / 'summarize_results.py')],
                               stdout=subprocess.DEVNULL, check=True)
    subprocess.run([sys.executable, str(HERE / 'summarize_results.py')], check=True)
    plot_python = '/home/zheng/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/bin/python3'
    subprocess.run([plot_python, str(HERE / 'plot_measured_fig14.py')], check=True)
    subprocess.run([sys.executable, str(HERE / 'summarize_prewarmed_writes.py')], check=True)
    subprocess.run([plot_python, str(HERE / 'plot_measured_fig14.py'), '--prewarmed-writes',
                    '--summary', str(HERE / 'paper-prewarmed-summary.json')], check=True)
    subprocess.run([sys.executable, str(HERE / 'write_fig14_report.py')], check=True)
    state['status'] = 'measurements_and_plots_complete_pending_review_and_final_push'
except Exception as exc:
    state.update(status='failed', error=str(exc)); save(); raise
finally: save()
