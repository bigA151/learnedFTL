#!/usr/bin/env bash
# Read-only guest fio address-generation check; uses a new ext4-backed COW guest image.
set -euo pipefail
base="$(cd "$(dirname "$0")" && pwd)"
cd "$base"
fs_type="$(findmnt -T "$base" -n -o FSTYPE)"
case "$fs_type" in ntfs|ntfs3|fuseblk) echo 'Refusing NTFS: use ext4.' >&2; exit 2;; esac
if (echo >/dev/tcp/127.0.0.1/2222) >/dev/null 2>&1; then echo 'Port 2222 occupied' >&2; exit 2; fi
id="$(date -u +%Y%m%dT%H%M%SZ)-$$"
out="results/diagnostics/fio-address-check-$id"
mkdir -p "$out"
image="images/guest-fio-address-check-$id.qcow2"
upstream/build-femu/qemu-img create -f qcow2 -F qcow2 -b "$PWD/images/guest-overlay.qcow2" "$image" > "$out/image-create.log"
"$base/../femu-local-env.sh" ./bin/qemu-ideal-paper-full \
  -L upstream/build-femu/pc-bios -machine q35,accel=kvm -cpu host -smp 4 -m 2048 \
  -drive file="$image",if=virtio,format=qcow2 -drive file=images/seed.iso,media=cdrom,readonly=on \
  -device femu,devsz_mb=30518,femu_mode=1 \
  -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22 \
  -device virtio-net-pci,netdev=net0 -display none \
  -serial "file:$out/serial.log" -monitor none > "$out/qemu.log" 2>&1 &
pid=$!
trap 'kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true' EXIT
ssh_opts=(-q -p 2222 -i images/guest_ed25519 -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=3)
ready=0
for _ in $(seq 1 120); do
  if ssh "${ssh_opts[@]}" ubuntu@127.0.0.1 true >/dev/null 2>&1; then ready=1; break; fi
  kill -0 "$pid" 2>/dev/null || break
  sleep 2
done
(( ready )) || { echo "Guest failed; see $out/qemu.log" >&2; exit 1; }
run_case() {
  local label="$1" jobs="$2" rate="$3" filesize="$4" size="${5:-476m}"
  local opts=''
  [[ "$filesize" == yes ]] && opts='--filesize=30518m'
  ssh "${ssh_opts[@]}" ubuntu@127.0.0.1 \
    "sudo fio --name=$label --filename=/dev/nvme0n1 --rw=read --bs=4k --ioengine=psync --direct=1 --numjobs=$jobs --group_reporting --size=$size --offset_increment=487428k --time_based=1 --runtime=1 --rate_iops=$rate --write_iolog=/tmp/$label.iolog --output-format=json $opts" > "$out/$label.fio.json" 2> "$out/$label.err"
  ssh "${ssh_opts[@]}" ubuntu@127.0.0.1 "cat /tmp/$label.iolog" > "$out/$label.iolog"
  python3 - "$out/$label.iolog" "$label" <<'PY'
import collections,json,re,sys
s=open(sys.argv[1]).read();addresses=[int(x) for x in re.findall(r'read ([0-9]+) 4096',s)]
c=collections.Counter(addresses)
assert addresses
print(json.dumps({'case':sys.argv[2],'reads':len(addresses),'unique_addresses':len(c),
 'max_repetitions':max(c.values()),'min_offset':min(addresses),'max_offset':max(addresses),'first_offsets':addresses[:8]}))
PY
}
run_case without_filesize_2jobs 2 1000 no
run_case with_filesize_2jobs 2 1000 yes
run_case with_filesize_64jobs 64 100 yes
run_case with_filesize_1job_128m 1 70000 yes 128m
run_case without_filesize_1job_128m 1 70000 no 128m
run_case with_filesize_64jobs_128m 64 100 yes 128m
cat <<'FIO_JOB' | ssh "${ssh_opts[@]}" ubuntu@127.0.0.1 'cat > /tmp/explicit-regions.fio'
[global]
filename=/dev/nvme0n1
rw=read
bs=4k
ioengine=psync
direct=1
time_based=1
runtime=1
rate_iops=70000
[job0]
offset=0
size=128m
filesize=128m
write_iolog=/tmp/explicit-j0.iolog
[job1]
offset=487428k
size=128m
filesize=618500k
write_iolog=/tmp/explicit-j1.iolog
FIO_JOB
ssh "${ssh_opts[@]}" ubuntu@127.0.0.1 'sudo fio /tmp/explicit-regions.fio --output-format=json' > "$out/explicit-regions.fio.json" 2> "$out/explicit-regions.err"
for job in j0 j1; do
  ssh "${ssh_opts[@]}" ubuntu@127.0.0.1 "cat /tmp/explicit-$job.iolog" > "$out/explicit-$job.iolog"
done
python3 - "$out" <<'PY_RANGE'
import json,re,sys
from pathlib import Path
p=Path(sys.argv[1]); expected=[0,487428*1024]
for idx,job in enumerate(('j0','j1')):
 addresses=[int(x) for x in re.findall(r'read ([0-9]+) 4096',(p/f'explicit-{job}.iolog').read_text())]
 assert addresses,job
 lo,hi=min(addresses),max(addresses)
 assert expected[idx]<=lo<=hi<expected[idx]+128*1048576,(job,lo,hi)
 assert len(set(addresses))>1000 and hi-lo>4*1048576,(job,'not advancing')
 assert len(set(addresses))<len(addresses),(job,'no wrap observed')
 print(json.dumps({'case':job,'reads':len(addresses),'unique':len(set(addresses)),'min_offset':lo,'max_offset':hi}))
PY_RANGE
python3 "$base/run_fio.py" ideal-address-probe read --duration 1 --capacity-mib 30518 --no-metrics > "$out/formal-run-fio-probe.json"
python3 - "$base/results/ideal-address-probe/read/run1.json" <<'PY_FORMAL'
import json,sys
j=json.load(open(sys.argv[1]))["jobs"][0]
assert j["error"]==0 and j["read"]["io_bytes"]>0
print(json.dumps({"case":"formal-run-fio-probe","bytes":j["read"]["io_bytes"],"job_options":j["job options"]}))
PY_FORMAL
echo "Saved: $base/$out"
