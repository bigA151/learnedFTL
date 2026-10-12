#!/usr/bin/env python3
"""Run one LearnedFTL FIO case at the paper's SSD geometry and cache sizes.

Unpublished choices: 2 GiB guest RAM, 60 s windows, disjoint job regions,
deterministic seeds. Six prewarm passes are required for reads; optional write
prewarming is an explicitly separate experimental choice.
Each invocation creates an independent COW guest; existing FEMU runs are untouched.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import time

HERE = Path(__file__).resolve().parent
SDK = Path(os.environ.get('FEMU_ROOT', '/home/zheng/FEMU-ext4'))
UP = SDK / 'reproduction/upstream/build-femu'
ENV = [str(SDK / 'femu-local-env.sh')]
parser = argparse.ArgumentParser()
parser.add_argument('pattern', choices=('read', 'randread', 'write', 'randwrite'))
parser.add_argument('--variant', choices=('learnedftl', 'dftl', 'tpftl', 'ideal', 'leaftl'),
                    default='learnedftl')
parser.add_argument('--shared-read-probe', action='store_true',
                    help='After formal sequential-read runs, record a separate 10s shared-range diagnostic')
parser.add_argument('--steady-write-probe', action=argparse.BooleanOptionalAction, default=True,
                    help='After read evaluation, record three separate matching prewarmed write windows')
parser.add_argument('--repeat-index', type=int, choices=(1, 2, 3),
                    help='One independent fresh-VM measurement, indexed for aggregation')
parser.add_argument('--write-prewarm-passes', type=int, choices=(0, 1, 6), default=0,
                    help='One pass is allowed only for a separately marked crash diagnostic')
parser.add_argument('--diagnostic-guest-log', action='store_true',
                    help='Capture guest kernel logs while polling; excludes this case from formal results')
parser.add_argument('--diagnostic-io-timeout-seconds', type=int,
                    help='Change guest NVMe timeout only in an explicitly marked diagnostic')
args = parser.parse_args()
if args.shared_read_probe: assert args.pattern == 'read'
if args.write_prewarm_passes == 1:
    assert args.diagnostic_guest_log and args.pattern.endswith('write')
if args.diagnostic_io_timeout_seconds is not None:
    assert args.diagnostic_guest_log and args.diagnostic_io_timeout_seconds > 0
repeat_indices = [args.repeat_index] if args.repeat_index else [1, 2, 3]
prewarm_passes = 6 if args.pattern.endswith('read') else args.write_prewarm_passes
steady_probe = args.steady_write_probe and args.pattern.endswith('read')
prewarm_time_limit = int(os.environ.get('PREWARM_TIMEOUT_SECONDS', '86400'))
assert prewarm_time_limit > 0
manifest_path = (HERE / 'build-manifest.json' if args.variant == 'learnedftl' else
                 HERE / 'variants' / args.variant / 'build-manifest.json')
build = json.loads(manifest_path.read_text())
if args.variant != 'learnedftl':
    validation = json.loads((manifest_path.parent / 'validation.json').read_text())
    assert validation['status'] == 'passed'
    assert validation['paper_binary_sha256'] == build['sha256']
assert hashlib.sha256(Path(build['binary']).read_bytes()).hexdigest() == build['sha256']
for filename, digest in build['sources'].items():
    assert hashlib.sha256(Path(filename).read_bytes()).hexdigest() == digest
assert os.access('/dev/kvm', os.R_OK | os.W_OK), 'Paper runs require KVM'
mem = dict(re.findall(r'^(\w+):\s+(\d+)', Path('/proc/meminfo').read_text(), re.M))
assert int(mem['MemAvailable']) > 35 * 1024**2, 'Need 35 GiB available RAM before starting'
with socket.socket() as probe:
    probe.bind(('127.0.0.1', 2233))
stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
OUT = HERE / ('paper-' + args.variant + '-' + args.pattern + '-' + stamp)
OUT.mkdir()
if args.diagnostic_guest_log:
    (OUT / 'DIAGNOSTIC-ONLY.txt').write_text('Guest kernel-log capture enabled for crash investigation; not a formal performance sample.\n')
shutil.copy2(__file__, OUT / 'runner.py')
snapshots = OUT / 'source-snapshot'
snapshots.mkdir()
source_index = {}
for number, (filename, digest) in enumerate(build['sources'].items()):
    target = snapshots / (str(number) + '-' + Path(filename).name)
    shutil.copy2(filename, target)
    assert hashlib.sha256(target.read_bytes()).hexdigest() == digest
    source_index[filename] = {'snapshot': target.name, 'sha256': digest}
(snapshots / 'index.json').write_text(json.dumps(source_index, indent=2) + '\n')
shutil.copy2(build['binary'], OUT / 'qemu-binary')
image = OUT / 'guest.qcow2'
subprocess.run(ENV + [str(UP / 'qemu-img'), 'create', '-f', 'qcow2', '-F', 'qcow2',
                     '-b', str(SDK / 'reproduction/images/guest-overlay.qcow2'), str(image)], check=True)
ssh = ['ssh', '-q', '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=no',
       '-o', 'UserKnownHostsFile=/dev/null', '-o', 'ConnectTimeout=5',
       '-i', str(SDK / 'reproduction/images/guest_ed25519'), '-p', '2233', 'ubuntu@127.0.0.1']
method = {'status': 'running', 'variant': args.variant, 'pattern': args.pattern,
          'runner_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
          'build': build, 'physical_bytes': 34359738368, 'logical_bytes': 32000442368,
          'overprovision_bytes': 2359296000, 'prewarm_passes': prewarm_passes, 'prewarm_bs': 524288,
          'prewarm_time_limit_seconds': prewarm_time_limit,
          'prewarm_randommap': 'fio bitmap; each 512KiB block once per pass, seed 20261011',
          'measurement': {'jobs': 64, 'bs': 4096, 'engine': 'psync', 'direct': True,
                          'runtime_seconds': 60, 'repeats': len(repeat_indices),
                          'repeat_indices': repeat_indices, 'randseed': 20261011, 'norandommap': 0},
          'guest': {'accelerator': 'kvm', 'vcpus': 4, 'ram_mib': 2048},
          'unpublished_choices': ['job regions/strides', 'seeds/randommap', '60s windows',
                                  'guest resources', 'write prewarm passes=' + str(prewarm_passes)
                                  if args.pattern.endswith('write') else 'six-pass read prewarm as paper'],
          'completed_stages': [],
          'repeat_state': 'Independent fresh VM' if args.repeat_index else 'Three windows share one VM',
          'address_layout': '64 explicit disjoint 476MiB regions; SeqRead stride additionally shifted 4KiB per job',
          'shared_read_probe_requested': args.shared_read_probe,
          'diagnostic_guest_log': args.diagnostic_guest_log,
          'steady_write_probe_requested': steady_probe}
def save_method():
    (OUT / 'method.json').write_text(json.dumps(method, indent=2, ensure_ascii=False) + '\n')
save_method()

def get_io_timeout():
    result = subprocess.run(ssh + ['cat /sys/block/nvme0n1/queue/io_timeout'],
                            capture_output=True, text=True, timeout=10, check=True)
    milliseconds = int(result.stdout.strip())
    assert milliseconds % 1000 == 0
    return milliseconds // 1000

def set_io_timeout(seconds):
    subprocess.run(ssh + ['sudo tee /sys/block/nvme0n1/queue/io_timeout'],
                   input=str(seconds*1000)+'\n', text=True, stdout=subprocess.DEVNULL,
                   timeout=10, check=True)
    assert get_io_timeout() == seconds

def run_guest(name, command, timeout=14400):
    """Preserve raw fio JSON; export host memory and guest sector progress separately."""
    print('Stage:', name, flush=True)
    with (OUT / (name + '.json')).open('w') as out, (OUT / (name + '.err')).open('w') as err:
        child = subprocess.Popen(ssh + [command], stdout=out, stderr=err)
        start = last_poll = last_progress = time.monotonic()
        old_sectors = None
        try:
            while child.poll() is None:
                time.sleep(5)
                now = time.monotonic()
                if qemu.poll() is not None:
                    raise RuntimeError(f'QEMU exited with return code {qemu.returncode}')
                if now - start > timeout: raise RuntimeError('Stage time limit exceeded')
                if now - last_poll >= 30:
                    status = subprocess.run(ssh + ['cat /sys/block/nvme0n1/stat'],
                                            capture_output=True, text=True, timeout=10, check=True)
                    fields = [int(x) for x in status.stdout.split()]
                    sectors = fields[2] + fields[6]
                    if sectors != old_sectors:
                        old_sectors = sectors; last_progress = now
                    host = dict(re.findall(r'^(VmRSS|VmSwap):\s+(\d+)',
                                           Path(f'/proc/{qemu.pid}/status').read_text(), re.M))
                    row = {'stage': name, 'elapsed_s': round(now - start, 1), 'block_stat': fields,
                           'qemu_rss_kib': int(host.get('VmRSS', 0)),
                           'qemu_swap_kib': int(host.get('VmSwap', 0))}
                    with (OUT / 'progress.jsonl').open('a') as progress:
                        progress.write(json.dumps(row) + '\n')
                    if args.diagnostic_guest_log:
                        kernel = subprocess.run(ssh + ['sudo dmesg --color=never'],
                                                capture_output=True, text=True, timeout=10)
                        with (OUT / 'guest-kernel.jsonl').open('a') as output:
                            output.write(json.dumps({'stage': name, 'elapsed_s': row['elapsed_s'],
                                                     'returncode': kernel.returncode,
                                                     'stdout': kernel.stdout, 'stderr': kernel.stderr}) + '\n')
                    print(name, row['elapsed_s'], 's, completed sectors:', sectors, flush=True)
                    if now - last_progress > 900: raise RuntimeError('No completed guest I/O for 900s')
                    last_poll = now
            if child.returncode: raise RuntimeError(f'{name}: fio/SSH exit {child.returncode}')
        finally:
            if child.poll() is None: child.terminate(); child.wait(timeout=10)
    data = json.loads((OUT / (name + '.json')).read_text())
    assert all(job['error'] == 0 for job in data['jobs'])
    method['completed_stages'].append(name); save_method()
    return data

qemu_args = [build['binary'], '-L', str(UP / 'pc-bios'), '-machine', 'q35,accel=kvm',
             '-cpu', 'host', '-smp', '4', '-m', '2048',
             '-drive', f'file={image},if=virtio,format=qcow2',
             '-drive', f'file={SDK}/reproduction/images/seed.iso,media=cdrom,readonly=on',
             '-device', 'femu,devsz_mb=30518,femu_mode=1',
             '-netdev', 'user,id=net0,hostfwd=tcp:127.0.0.1:2233-:22',
             '-device', 'virtio-net-pci,netdev=net0', '-display', 'none',
             '-serial', 'file:' + str(OUT / 'serial.log'), '-monitor', 'none']
print('Output:', OUT, flush=True)
with (OUT / 'qemu.log').open('w') as log:
    qemu = subprocess.Popen(ENV + qemu_args, stdout=log, stderr=subprocess.STDOUT)
    try:
        for _ in range(180):
            if qemu.poll() is not None: raise RuntimeError('QEMU boot failed')
            if subprocess.run(ssh + ['true'], stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode == 0: break
            time.sleep(2)
        else: raise RuntimeError('SSH boot timeout')
        method['guest']['initial_nvme_io_timeout_seconds'] = get_io_timeout()
        method['guest']['timeout_interface'] = '/sys/block/nvme0n1/queue/io_timeout (milliseconds; converted to seconds)'
        if args.diagnostic_io_timeout_seconds is not None:
            set_io_timeout(args.diagnostic_io_timeout_seconds)
        method['guest']['formal_nvme_io_timeout_seconds'] = get_io_timeout()
        save_method()
        subprocess.run(ssh + ['sudo nvme admin-passthru /dev/nvme0 --opcode=0xef --cdw10=8'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        prewarm_pattern = 'randwrite' if args.pattern.startswith('rand') else 'write'
        command = ('sudo fio --name=prewarm --filename=/dev/nvme0n1 '
                   f'--rw={prewarm_pattern} --bs=512k --size=30518m --io_size={30518*prewarm_passes}m '
                   '--ioengine=psync --direct=1 --numjobs=1 --randseed=20261011 '
                   '--randrepeat=1 --norandommap=0 --group_reporting --output-format=json')
        if prewarm_passes:
            warmup = run_guest('prewarm', command, prewarm_time_limit)
            assert sum(j['write']['io_bytes'] for j in warmup['jobs']) == prewarm_passes * method['logical_bytes']
            subprocess.run(ssh + ['sudo nvme admin-passthru /dev/nvme0 --opcode=0xef --cdw10=9'],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        region = 476 * 1024**2
        stride = region + (4096 if args.pattern == 'read' else 0)
        lines = ['[global]', 'filename=/dev/nvme0n1', 'rw=' + args.pattern, 'bs=4k',
                 'ioengine=psync', 'direct=1', 'time_based=1', 'runtime=60',
                 'group_reporting=1', 'randrepeat=1', 'randseed=20261011', 'norandommap=0']
        for job in range(64):
            start = job * stride; end = start + region
            assert end <= method['logical_bytes']
            lines += [f'[job{job:02d}]', f'offset={start}', f'filesize={end}', f'size={region}']
        fio_job = '\n'.join(lines) + '\n'
        (OUT / 'measure.fio').write_text(fio_job)
        subprocess.run(ssh + ['cat > /tmp/learnedftl-paper.fio'], input=fio_job, text=True, check=True)
        for repeat in repeat_indices:
            subprocess.run(ssh + ['sudo nvme admin-passthru /dev/nvme0 --opcode=0xef --cdw10=8'],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
            data = run_guest(f'run{repeat}', 'sudo fio /tmp/learnedftl-paper.fio --output-format=json', 300)
            kind = 'write' if args.pattern.endswith('write') else 'read'
            assert sum(j[kind]['io_bytes'] for j in data['jobs']) > 0
            subprocess.run(ssh + ['sudo nvme admin-passthru /dev/nvme0 --opcode=0xef --cdw10=9'],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        if args.shared_read_probe:
            # This changes only the address layout after all formal windows.
            # The paper does not disclose job regions; retain it as sensitivity
            # evidence rather than silently mixing it into Figure 14 samples.
            probe_lines = ['[global]', 'filename=/dev/nvme0n1', 'rw=read', 'bs=4k',
                           'ioengine=psync', 'direct=1', 'time_based=1', 'runtime=10',
                           'group_reporting=1']
            for job in range(64):
                probe_lines += [f'[shared{job:02d}]', 'offset=0',
                                'filesize=' + str(method['logical_bytes']),
                                'size=' + str(method['logical_bytes'])]
            probe_job = '\n'.join(probe_lines) + '\n'
            (OUT / 'shared-read-probe.fio').write_text(probe_job)
            subprocess.run(ssh + ['cat > /tmp/learnedftl-shared-read.fio'],
                           input=probe_job, text=True, check=True)
            subprocess.run(ssh + ['sudo nvme admin-passthru /dev/nvme0 --opcode=0xef --cdw10=8'],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
            probe = run_guest('shared-read-probe',
                              'sudo fio /tmp/learnedftl-shared-read.fio --output-format=json', 120)
            assert sum(j['read']['io_bytes'] for j in probe['jobs']) > 0
            subprocess.run(ssh + ['sudo nvme admin-passthru /dev/nvme0 --opcode=0xef --cdw10=9'],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        if steady_probe:
            # Use the expensive matching six-pass prewarm after all read
            # measurements. Consecutive writes evolve the device; these are
            # separately labeled observations, never fresh-write samples.
            write_pattern = 'randwrite' if args.pattern.startswith('rand') else 'write'
            # These are separate observations after the formal read samples.
            # Keep the same effective queue timeout as formal windows.
            # GC read-completion snapshots fix artificial cross-LUN chains;
            # extended timeouts are retained only as explicit diagnostics.
            method['steady_write_probe_environment'] = {
                'prior_nvme_io_timeout_seconds': get_io_timeout(),
                'nvme_io_timeout_seconds': get_io_timeout(),
                'stage_time_limit_seconds': 900,
                'reason': 'Keep the effective queue timeout unchanged; preserve actual FIO runtime including request drain.'}
            save_method()
            steady_lines = ['[global]', 'filename=/dev/nvme0n1', 'rw=' + write_pattern,
                            'bs=4k', 'ioengine=psync', 'direct=1', 'time_based=1',
                            'runtime=60', 'group_reporting=1', 'randrepeat=1',
                            'randseed=20261011', 'norandommap=0']
            for job in range(64):
                start = job * region; end = start + region
                steady_lines += [f'[job{job:02d}]', f'offset={start}',
                                 f'filesize={end}', f'size={region}']
            steady_job = '\n'.join(steady_lines) + '\n'
            (OUT / 'steady-write.fio').write_text(steady_job)
            subprocess.run(ssh + ['cat > /tmp/learnedftl-steady-write.fio'],
                           input=steady_job, text=True, check=True)
            for repeat in (1, 2, 3):
                subprocess.run(ssh + ['sudo nvme admin-passthru /dev/nvme0 --opcode=0xef --cdw10=8'],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
                data = run_guest(f'steady-write{repeat}',
                                 'sudo fio /tmp/learnedftl-steady-write.fio --output-format=json', 900)
                assert sum(j['write']['io_bytes'] for j in data['jobs']) > 0
                subprocess.run(ssh + ['sudo nvme admin-passthru /dev/nvme0 --opcode=0xef --cdw10=9'],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        method['status'] = 'fio_finished'
    except Exception as exc:
        method['status'] = 'failed'; method['error'] = str(exc)
        raise
    finally:
        method['qemu_exit_before_cleanup'] = qemu.poll()
        if qemu.poll() is None: qemu.terminate()
        try: qemu.wait(timeout=15)
        except subprocess.TimeoutExpired: qemu.kill(); qemu.wait()
        method['qemu_final_returncode'] = qemu.returncode
        save_method()
metrics = re.findall(r'METRICS (.*)', (OUT / 'qemu.log').read_text())
assert len(metrics) == len(repeat_indices) + bool(prewarm_passes) + bool(args.shared_read_probe) + 3*steady_probe, 'Need all completed measurement snapshots'
parsed = [dict(re.findall(r'(\w+)=([\d.]+)', row)) for row in metrics]
method['prewarm_metrics'] = parsed[0] if prewarm_passes else None
first_metric = int(bool(prewarm_passes))
method['metrics'] = parsed[first_metric:first_metric + len(repeat_indices)]
if args.shared_read_probe:
    shared_index = first_metric + len(repeat_indices)
    method['shared_read_probe'] = {'diagnostic_only': True, 'runtime_seconds': 10,
                                   'address_layout': 'All 64 jobs independently traverse full namespace from offset 0',
                                   'metrics': parsed[shared_index]}
if steady_probe:
    method['steady_write_probe'] = {'diagnostic_only': True,
        'pattern': 'randwrite' if args.pattern.startswith('rand') else 'write',
        'runtime_seconds': 60, 'repeat_indices': [1,2,3], 'prewarm_passes': 6,
        'address_layout': 'Same 64 disjoint 476MiB regions as fresh-write measurements',
        'repeat_state': 'Three consecutive windows evolve one prewarmed device after read measurements',
        'metrics': parsed[-3:]}
method['status'] = 'complete'
save_method()
