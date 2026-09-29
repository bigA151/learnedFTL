#!/usr/bin/env bash
# Isolated LearnedFTL GC experiment; writes only a new guest COW image.
set -euo pipefail
base="$(cd "$(dirname "$0")" && pwd)"
cd "$base"
fs_type="$(findmnt -T "$base" -n -o FSTYPE)"
case "$fs_type" in ntfs|ntfs3|fuseblk) echo "Use the ext4 checkout." >&2; exit 2;; esac
runtime="${RUNTIME_SECONDS:-60}"
passes="${WARMUP_PASSES:-6}"
warmup_io_mib="${GC_WARMUP_IO_MIB:-$((30518*passes))}"
jobs="${GC_FIO_JOBS:-64}"
prepare_seconds="${GC_PREPARE_SECONDS:-0}"
sparse_groups="${GC_PREPARE_SPARSE_GROUPS:-0}"
measure_size_mib="${GC_MEASURE_SIZE_MIB:-476}"
pattern="${GC_FIO_PATTERN:-write}"
warmup_pattern="${GC_WARMUP_PATTERN:-write}"
[[ "$warmup_pattern" == write || "$warmup_pattern" == randwrite ]] || exit 2
bwlog_opts=""
if [[ "${GC_BW_LOG:-0}" == 1 ]]; then bwlog_opts="--write_bw_log=/tmp/femu-bw --log_avg_msec=1000 --per_job_logs=0"; fi
warmup_extra=""
[[ "$warmup_pattern" == randwrite ]] && warmup_extra="--norandommap=1 --randrepeat=0"
[[ "$pattern" == write || "$pattern" == randwrite ]] || exit 2
[[ "$runtime" =~ ^[1-9][0-9]*$ && "$passes" =~ ^(0|[1-9][0-9]*)$ && "$warmup_io_mib" =~ ^(0|[1-9][0-9]*)$ && "$jobs" =~ ^[1-9][0-9]*$ && "$prepare_seconds" =~ ^(0|[1-9][0-9]*)$ && "$sparse_groups" =~ ^(0|[1-9][0-9]*)$ && "$measure_size_mib" =~ ^[1-9][0-9]*$ ]] || exit 2
(( measure_size_mib <= 476 && sparse_groups <= 235 )) || exit 2
(( jobs <= 64 )) || exit 2
binary="${GC_DIAG_BINARY:-qemu-learnedftl-gc-diag}"
offset_increment_kib="${GC_OFFSET_INCREMENT_KIB:-487428}"
[[ "$offset_increment_kib" == 487424 || "$offset_increment_kib" == 487428 ]] || exit 2
[[ "$binary" == qemu-learnedftl-paper-full || "$binary" == qemu-dftl-paper-full || "$binary" == qemu-tpftl-paper-full || "$binary" == qemu-ideal-paper-full || "$binary" == qemu-leaftl-paper-full || "$binary" == qemu-learnedftl-cross-borrow-greedy || "$binary" == qemu-learnedftl-cross-borrow || "$binary" == qemu-learnedftl-full-gtd-gc || "$binary" == qemu-learnedftl-gc-phys-audit || "$binary" == qemu-learnedftl-gc-donor-audit-v2 || "$binary" == qemu-learnedftl-gc-greedy-guarded || "$binary" == qemu-learnedftl-gc-single-erase || "$binary" == qemu-learnedftl-gc-diag || "$binary" == qemu-learnedftl-gc-fixed || "$binary" == qemu-learnedftl-gc-force-fallback || "$binary" == qemu-learnedftl-gc-force-edge || "$binary" == qemu-learnedftl-paper-gcfix || "$binary" == qemu-learnedftl-paper-gcfix-320lines ]] || exit 2
[[ -x "bin/$binary" ]] || { echo "Build the GC diagnostic binary first." >&2; exit 2; }
if (echo >/dev/tcp/127.0.0.1/2222) >/dev/null 2>&1; then
  echo "FEMU port 2222 is occupied; wait for the current experiment." >&2; exit 2
fi
id="$(date -u +%Y%m%dT%H%M%SZ)-$$"
out="results/diagnostics/gc-$id"
mkdir -p "$out"
python3 - "$out" "$base" "$binary" "$fs_type" <<'PY_MANIFEST'
import hashlib, json, sys
from pathlib import Path
out, base, binary, fs_type = sys.argv[1:]
base = Path(base).resolve()
files = {
    'binary': base / 'bin' / binary,
    'build_script': base / 'build_gc_diag.py',
    'cross_group_patch': base / 'cross_group_patch.py',
    'validator': base / 'validate_cross_group.py',
    'runner': base / 'run_gc_diag.sh',
    'upstream_ftl': base / 'upstream/hw/femu/bbssd/ld-tpftl.c',
}
manifest = {'experiment_root': str(base.parent), 'filesystem': fs_type,
            'files': {name: {'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
                      for name, path in files.items()}}
(out_path := Path(out) / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
PY_MANIFEST
image="images/guest-gc-$id.qcow2"
upstream/build-femu/qemu-img create -f qcow2 -F qcow2 -b "$PWD/images/guest-overlay.qcow2" "$image" > "$out/image-create.log"
qemu_log="$out/qemu.log"
"$base/../femu-local-env.sh" "./bin/$binary" \
  -L upstream/build-femu/pc-bios -machine q35,accel=kvm -cpu host -smp 4 -m 2048 \
  -drive file="$image",if=virtio,format=qcow2 \
  -drive file=images/seed.iso,media=cdrom,readonly=on \
  -device femu,devsz_mb=30518,femu_mode=1 \
  -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22 \
  -device virtio-net-pci,netdev=net0 -display none \
  -serial "file:$out/guest-serial.log" -monitor none > "$qemu_log" 2>&1 &
qemu_pid=$!
stop_guest() { kill "$qemu_pid" 2>/dev/null || true; wait "$qemu_pid" 2>/dev/null || true; }
trap stop_guest EXIT
ssh_opts=(-q -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=3 -i images/guest_ed25519 -p 2222)
ready=0
for _ in $(seq 1 120); do
  if ssh "${ssh_opts[@]}" ubuntu@127.0.0.1 true >/dev/null 2>&1; then ready=1; break; fi
  kill -0 "$qemu_pid" 2>/dev/null || break
  sleep 2
done
(( ready )) || { echo "Guest boot failed: $qemu_log" >&2; exit 1; }
echo "Output: $base/$out"
if (( warmup_io_mib > 0 )); then
echo "Prewarm: $warmup_io_mib MiB $warmup_pattern (nominal passes=$passes)"
ssh "${ssh_opts[@]}" ubuntu@127.0.0.1 \
  "sudo fio --name=warmup-gc-diag --filename=/dev/nvme0n1 --rw=$warmup_pattern --bs=512k --ioengine=psync --direct=1 --size=30518m --io_size=${warmup_io_mib}m --numjobs=1 --group_reporting --output-format=json $warmup_extra" \
  > "$out/warmup.json" 2> "$out/warmup.err"
python3 - "$out/warmup.json" "$warmup_io_mib" <<'PY'
import json,sys
j=json.load(open(sys.argv[1]))['jobs'][0]
assert j['error']==0 and j['write']['io_bytes']==int(sys.argv[2])*1048576
PY
else
  printf '{"skipped":true,"reason":"fresh FEMU device; no prewarm"}\n' > "$out/warmup.json"
fi
if (( sparse_groups > 0 )); then
  echo "Preparing $sparse_groups sparse groups with one 4 KiB write each (diagnostic only)"
  ssh "${ssh_opts[@]}" ubuntu@127.0.0.1 \
    "sudo fio --name=sparse-groups --filename=/dev/nvme0n1 --rw=write --bs=4k --ioengine=psync --direct=1 --numjobs=$sparse_groups --group_reporting --filesize=30518m --size=4k --offset_increment=131072k --output-format=json" \
    > "$out/sparse-prepare.json" 2> "$out/sparse-prepare.err"
  python3 - "$out/sparse-prepare.json" "$sparse_groups" <<'PY_SPARSE'
import json,sys
entry=json.load(open(sys.argv[1]))['jobs'][0]
assert entry['error']==0 and entry['write']['io_bytes']==int(sys.argv[2])*4096
PY_SPARSE
fi
if (( prepare_seconds > 0 )); then
  echo "Preparing 64-job 4 KiB sequential state for $prepare_seconds s (diagnostic only)"
  ssh "${ssh_opts[@]}" ubuntu@127.0.0.1 \
    "sudo fio --name=prepare-gc-diag --filename=/dev/nvme0n1 --rw=write --bs=4k --ioengine=psync --direct=1 --numjobs=64 --group_reporting --filesize=30518m --size=476m --offset_increment=487428k --time_based=1 --runtime=$prepare_seconds --output-format=json" \
    > "$out/prepare.json" 2> "$out/prepare.err"
  python3 - "$out/prepare.json" <<'PY_PREPARE'
import json,sys
entry=json.load(open(sys.argv[1]))['jobs'][0]
assert entry['error']==0 and entry['write']['io_bytes']>0
PY_PREPARE
fi
ssh "${ssh_opts[@]}" ubuntu@127.0.0.1 \
  'sudo nvme admin-passthru /dev/nvme0 --opcode=0xef --cdw10=8' > "$out/reset.out" 2> "$out/reset.err"
echo "Measuring ${jobs:-64}-job 4 KiB $pattern for $runtime s"
if [[ "${GC_EXPLICIT_RANGES:-0}" == 1 ]]; then
  python3 - "$jobs" "$pattern" "$runtime" "$measure_size_mib" "$offset_increment_kib" "${GC_BW_LOG:-0}" > "$out/measure.fio" <<'PY_FIO_JOB'
import sys
jobs,pattern,runtime,region_mib,stride_kib,bw=map(str,sys.argv[1:])
jobs,runtime,region_mib,stride_kib=map(int,(jobs,runtime,region_mib,stride_kib))
print('[global]\nfilename=/dev/nvme0n1\nrw='+pattern+'\nbs=4k\nioengine=psync\ndirect=1\ngroup_reporting=1\ntime_based=1\nruntime='+str(runtime)+'\nrandrepeat=0')
if bw=='1': print('write_bw_log=/tmp/femu-bw\nlog_avg_msec=1000\nper_job_logs=0')
for job in range(jobs):
 start=job*stride_kib
 end=start+region_mib*1024
 assert end<=30518*1024
 print(f'[job{job:02d}]\noffset={start}k\nfilesize={end}k\nsize={region_mib*1024}k')
PY_FIO_JOB
  ssh "${ssh_opts[@]}" ubuntu@127.0.0.1 'cat > /tmp/gc-explicit-regions.fio' < "$out/measure.fio"
fi
set +e
if [[ "${GC_EXPLICIT_RANGES:-0}" == 1 ]]; then
  ssh "${ssh_opts[@]}" ubuntu@127.0.0.1 \
    'sudo fio /tmp/gc-explicit-regions.fio --output-format=json' > "$out/fio.json" 2> "$out/fio.err"
else
  ssh "${ssh_opts[@]}" ubuntu@127.0.0.1 \
    "sudo fio --name=gc-diag --filename=/dev/nvme0n1 --rw=$pattern --bs=4k --ioengine=psync --direct=1 --numjobs=${jobs:-64} --group_reporting --filesize=30518m --size=${measure_size_mib}m --offset_increment=${offset_increment_kib}k --time_based=1 --runtime=$runtime --randrepeat=0 --output-format=json $bwlog_opts" \
    > "$out/fio.json" 2> "$out/fio.err"
fi
fio_rc=$?
if [[ "${GC_BW_LOG:-0}" == 1 ]]; then
  ssh "${ssh_opts[@]}" ubuntu@127.0.0.1 'cd /tmp && cat femu-bw_bw*.log' \
    > "$out/fio-bw.log" 2> "$out/fio-bw.err" || true
fi
if [[ "${GC_POST_READ:-0}" == 1 && "$fio_rc" == 0 ]]; then
  ssh "${ssh_opts[@]}" ubuntu@127.0.0.1 \
    'sudo fio --name=hot-read --filename=/dev/nvme0n1 --rw=read --bs=4k --ioengine=psync --direct=1 --size=128m --offset=0 --output-format=json' \
    > "$out/post-hot-read.json" 2> "$out/post-hot-read.err" || fio_rc=1
  ssh "${ssh_opts[@]}" ubuntu@127.0.0.1 \
    'sudo fio --name=donor-read --filename=/dev/nvme0n1 --rw=read --bs=4k --ioengine=psync --direct=1 --size=32m --offset=16384m --output-format=json' \
    > "$out/post-donor-read.json" 2> "$out/post-donor-read.err" || fio_rc=1
  if (( jobs >= 2 )); then
    ssh "${ssh_opts[@]}" ubuntu@127.0.0.1 \
      'sudo fio --name=second-hot-read --filename=/dev/nvme0n1 --rw=read --bs=4k --ioengine=psync --direct=1 --size=16m --offset=487428k --output-format=json' \
      > "$out/post-second-hot-read.json" 2> "$out/post-second-hot-read.err" || fio_rc=1
  fi
  python3 - "$out" "$jobs" <<'PY_POST_READ' || fio_rc=1
import json, sys
from pathlib import Path
folder, jobs = Path(sys.argv[1]), int(sys.argv[2])
expected = {'post-hot-read.json': 128 << 20,
            'post-donor-read.json': 32 << 20}
if jobs >= 2:
    expected['post-second-hot-read.json'] = 16 << 20
for filename, nbytes in expected.items():
    entry = json.loads((folder / filename).read_text())['jobs'][0]
    assert entry['error'] == 0 and entry['read']['io_bytes'] == nbytes, filename
PY_POST_READ
fi
ssh "${ssh_opts[@]}" ubuntu@127.0.0.1 \
  'sudo nvme admin-passthru /dev/nvme0 --opcode=0xef --cdw10=9' > "$out/report.out" 2> "$out/report.err"
report_rc=$?
set -e
grep -E 'GC_DIAG|GC_DONOR|GC_EXHAUST' "$qemu_log" > "$out/gc-summary.log" || true
python3 - "$out" "$passes" "$warmup_io_mib" "$runtime" "$fio_rc" "$report_rc" "$image" "$binary" "${jobs:-64}" "$pattern" "$warmup_pattern" "$offset_increment_kib" <<'PY'
import json,sys
p,passes,warmup_io_mib,runtime,fio_rc,report_rc,image,binary,jobs,pattern,warmup_pattern,offset_increment_kib=sys.argv[1:]
with open(p+'/method.json','w') as f:
 json.dump({'binary':binary,'warmup_passes':int(passes),'warmup_pattern':warmup_pattern,
            'warmup_bytes':int(warmup_io_mib)*1048576,'fio_bs':'4k',
            'fio_engine':'psync','fio_pattern':pattern,'fio_jobs':int(jobs),'fio_seconds':int(runtime),
            'fio_filesize_mib':None if __import__('os').environ.get('GC_EXPLICIT_RANGES')=='1' else 30518,'fio_size_mib':int(__import__('os').environ.get('GC_MEASURE_SIZE_MIB','476')),'prepare_seconds':int(__import__('os').environ.get('GC_PREPARE_SECONDS','0')),'sparse_groups':int(__import__('os').environ.get('GC_PREPARE_SPARSE_GROUPS','0')),'offset_increment':offset_increment_kib+'k',
            'fio_returncode':int(fio_rc),'report_returncode':int(report_rc),
            'address_layout':'explicit-per-job-end-v1' if __import__('os').environ.get('GC_EXPLICIT_RANGES')=='1' else 'legacy-global-filesize',
            'jobfile_sha256':__import__('hashlib').sha256(open(p+'/measure.fio','rb').read()).hexdigest() if __import__('os').path.exists(p+'/measure.fio') else None,
            'guest_image':image,'pair_trigger':int(__import__('os').environ.get('CG_BORROW_PAIR_TRIGGER','8192')),'allow_multi':bool(__import__('os').environ.get('CG_BORROW_ALLOW_MULTI')),'allow_low_free':bool(__import__('os').environ.get('CG_ALLOW_LOW_FREE')),'borrow_sticky':bool(__import__('os').environ.get('CG_BORROW_STICKY')),'donor_limit':int(__import__('os').environ.get('CG_BORROW_DONOR_LIMIT','256')),'post_read':bool(int(__import__('os').environ.get('GC_POST_READ','0'))),'assert_model_bitmap':bool(__import__('os').environ.get('CG_ASSERT_MODEL_BITMAP')),'test_budget_free':__import__('os').environ.get('CG_TEST_BUDGET_FREE'),'test_fail_target_at':__import__('os').environ.get('CG_TEST_FAIL_TARGET_AT')},f,indent=2)
 f.write('\n')
PY
echo "Done: $base/$out (fio=$fio_rc, report=$report_rc)"
(( fio_rc == 0 && report_rc == 0 )) || exit 1
python3 - "$out/fio.json" <<'PY_VALIDATE'
import json,sys
j=json.load(open(sys.argv[1]))['jobs'][0]
assert j['error']==0 and j['write']['io_bytes']>0
print(f"Validated fio: {j['write']['bw_bytes']/1048576:.4f} MiB/s, {j['write']['iops']:.1f} IOPS")
PY_VALIDATE
if grep -Eq 'GC_EXHAUST|No free lines left|buduijin|GC_INVARIANT|GC_BAD_FREE_TARGET' "$qemu_log"; then
  echo "GC/allocator fault in $qemu_log" >&2
  exit 1
fi
if [[ "$binary" != *-paper-full && "$binary" != qemu-learnedftl-paper-gcfix && "$binary" != qemu-learnedftl-paper-gcfix-320lines ]] && ! grep -q 'GC_DIAG' "$out/gc-summary.log"; then
  echo "Missing GC_DIAG completion report in $out/gc-summary.log" >&2
  exit 1
fi
