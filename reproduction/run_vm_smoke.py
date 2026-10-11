#!/usr/bin/env python3
"""Run a disposable small FEMU namespace; this is not paper throughput.

The guest OS backing images are opened read-only through a new COW overlay.
All writes, logs and newly compiled binaries remain in this workspace.
"""
import datetime
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import time

HERE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser()
parser.add_argument('--manifest', type=Path, default=HERE / 'build-manifest.json')
args = parser.parse_args()
manifest = json.loads(args.manifest.read_text())
variant = manifest.get('variant', 'learnedftl')
assert hashlib.sha256(Path(manifest['binary']).read_bytes()).hexdigest() == manifest['sha256']
for filename, digest in manifest['sources'].items():
    assert hashlib.sha256(Path(filename).read_bytes()).hexdigest() == digest, 'Rebuild changed source before running'
SDK = Path(os.environ.get('FEMU_ROOT', '/home/zheng/FEMU-ext4'))
UP = SDK / 'reproduction/upstream/build-femu'
ENV = [str(SDK / 'femu-local-env.sh')]
stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
OUT = HERE / ('vm-smoke-' + variant + '-' + stamp)
OUT.mkdir()
port = 2233
namespace_mib = int(os.environ.get('VM_SMOKE_MIB', '768'))
assert 32 <= namespace_mib <= 2048
assert namespace_mib * 1024**2 < manifest.get('physical_bytes', 34359738368)
with socket.socket() as probe:
    probe.bind(('127.0.0.1', port))
accel = 'kvm' if os.access('/dev/kvm', os.R_OK | os.W_OK) else 'tcg'
cpu = 'host' if accel == 'kvm' else 'max'
image = OUT / 'guest.qcow2'
subprocess.run(ENV + [str(UP / 'qemu-img'), 'create', '-f', 'qcow2', '-F', 'qcow2',
                     '-b', str(SDK / 'reproduction/images/guest-overlay.qcow2'), str(image)], check=True)
ssh = ['ssh', '-q', '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=no',
       '-o', 'UserKnownHostsFile=/dev/null', '-o', 'ConnectTimeout=3',
       '-i', str(SDK / 'reproduction/images/guest_ed25519'), '-p', str(port), 'ubuntu@127.0.0.1']
qemu_args = [manifest['binary'], '-L', str(UP / 'pc-bios'),
             '-machine', 'q35,accel=' + accel, '-cpu', cpu, '-smp', '2', '-m', '2048',
             '-drive', f'file={image},if=virtio,format=qcow2',
             '-drive', f'file={SDK}/reproduction/images/seed.iso,media=cdrom,readonly=on',
             '-device', f'femu,devsz_mb={namespace_mib},femu_mode=1',
             '-netdev', f'user,id=net0,hostfwd=tcp:127.0.0.1:{port}-:22',
             '-device', 'virtio-net-pci,netdev=net0', '-display', 'none',
             '-serial', 'file:' + str(OUT / 'serial.log'), '-monitor', 'none']
method = {'purpose': 'correctness smoke; not paper performance', 'acceleration': accel,
          'variant': variant, 'namespace_bytes': namespace_mib * 1024**2,
          'physical_geometry': manifest.get('geometry', '8ch/8lun/256blk/512pg/4KiB'),
          'build': manifest, 'status': 'running'}
(OUT / 'method.json').write_text(json.dumps(method, indent=2, ensure_ascii=False))
print('Output:', OUT, 'acceleration:', accel, flush=True)
with (OUT / 'qemu.log').open('w') as log:
    qemu = subprocess.Popen(ENV + qemu_args, stdout=log, stderr=subprocess.STDOUT)
    try:
        for attempt in range(240):
            if qemu.poll() is not None: raise RuntimeError('QEMU exited during boot')
            if subprocess.run(ssh + ['true'], stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode == 0: break
            time.sleep(2)
        else: raise RuntimeError('Guest SSH boot timeout')
        print('Guest ready; six namespace writes then CRC readback', flush=True)
        commands = {
            'write-verify': 'sudo fio --name=gc-verify --filename=/dev/nvme0n1 --rw=write --bs=512k '
                            f'--size={namespace_mib}m --io_size={namespace_mib * 12}m --ioengine=psync --direct=1 '
                            '--verify=crc32c --verify_interval=4k --do_verify=1 --output-format=json',
            'random-read': 'sudo fio --name=random-read --filename=/dev/nvme0n1 --rw=randread '
                           f'--bs=4k --size={namespace_mib}m --ioengine=psync --direct=1 --randseed=20261011 '
                           '--output-format=json',
        }
        for name, command in commands.items():
            with (OUT / (name + '.json')).open('w') as out, (OUT / (name + '.err')).open('w') as err:
                subprocess.run(ssh + [command], stdout=out, stderr=err, timeout=600, check=True)
            data = json.loads((OUT / (name + '.json')).read_text())
            assert all(job['error'] == 0 for job in data['jobs'])
            if name == 'write-verify':
                # fio 3.16 includes both write and verification read in io_size.
                assert sum(job['write']['io_bytes'] for job in data['jobs']) == namespace_mib * 6 * 1024**2
                assert sum(job['read']['io_bytes'] for job in data['jobs']) == namespace_mib * 6 * 1024**2
            else:
                assert sum(job['read']['io_bytes'] for job in data['jobs']) == namespace_mib * 1024**2
            print(name, 'passed', flush=True)
        subprocess.run(ssh + ['sudo nvme admin-passthru /dev/nvme0 --opcode=0xef --cdw10=9'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        method['status'] = 'io_verified'
    except Exception as exc:
        method['status'] = 'failed'
        method['error'] = str(exc)
        raise
    finally:
        qemu.terminate()
        try: qemu.wait(timeout=15)
        except subprocess.TimeoutExpired:
            qemu.kill(); qemu.wait()
        (OUT / 'method.json').write_text(json.dumps(method, indent=2, ensure_ascii=False) + '\n')
import re
metrics = re.findall(r'METRICS (.*)', (OUT / 'qemu.log').read_text())
values = dict(re.findall(r'(\w+)=([\d.]+)', metrics[-1])) if metrics else {}
method['metrics'] = values
method['status'] = 'failed'
if values and int(values['gc']) > 0:
    if variant != 'learnedftl' or namespace_mib <= 128 or int(values['model_hit']) > 0:
        method['status'] = 'passed'
(OUT / 'method.json').write_text(json.dumps(method, indent=2, ensure_ascii=False) + '\n')
assert method['status'] == 'passed', 'Smoke must execute GC and, above 128 MiB, model reads'
