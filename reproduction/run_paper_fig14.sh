#!/usr/bin/env bash
# Reproduce Figure 14's data collection with the closest disclosed parameters.
# Run one variant at a time: VARIANTS='learnedftl dftl tpftl leaftl ideal'.
set -uo pipefail
usage() {
  cat <<'HELP'
用法：bash reproduction/run_paper_fig14.sh [--help|--status|--resume]
在 FEMU 仓库根目录运行；正式运行会清空实验盘上的数据，请不要在盘上保存文件。
默认依次运行 5 种 FTL × 4 种负载，每项预热 6 盘、测量 3 次，每次 60 秒。
--status  只列出已完成/失败/待运行项目，不启动虚拟机。
--resume  跳过标记 complete 且原始数据完整的项目，继续其他项目。
可设 VARIANTS、PATTERNS、WARMUP_PASSES、REPEATS、RUNTIME_SECONDS；
修改后三个参数产生诊断实验，不能用于论文严格复现图。
HELP
}
mode=run
case "${1:-}" in
  --help|-h) usage; exit 0;;
  --status) mode=status;;
  --resume) mode=resume;;
  '') ;;
  *) usage >&2; exit 2;;
esac
base="$(cd "$(dirname "$0")" && pwd)"
cd "$base"
fs_type="$(findmnt -T "$base" -n -o FSTYPE)"
case "$fs_type" in ntfs|ntfs3|fuseblk) echo "Refusing to run FEMU on $fs_type; use an ext4 checkout." >&2; exit 2;; esac
capacity_mib=30518                 # 32,000,442,368 bytes ≈ 32.00 decimal GB
passes="${WARMUP_PASSES:-6}"
repeats="${REPEATS:-3}"
runtime="${RUNTIME_SECONDS:-60}"  # paper leaves the benchmark duration unspecified
variants="${VARIANTS:-learnedftl dftl tpftl leaftl ideal}"
patterns="${PATTERNS:-randread read randwrite write}"
ssh_opts=(-q -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -i images/guest_ed25519 -p 2222)
qemu_pid=''
stop_guest() {
  if [[ -n "$qemu_pid" ]]; then
    ssh "${ssh_opts[@]}" ubuntu@127.0.0.1 'sudo poweroff' >/dev/null 2>&1 || true
    for _ in $(seq 1 20); do
      kill -0 "$qemu_pid" 2>/dev/null || break
      sleep 1
    done
    kill "$qemu_pid" 2>/dev/null || true
    wait "$qemu_pid" 2>/dev/null || true
    qemu_pid=''
  fi
}
trap stop_guest EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
complete_item() {
  python3 - "$1" "$repeats" "$passes" "$runtime" "$capacity_mib" <<'PY2'
import hashlib,json,sys
from pathlib import Path
folder=Path(sys.argv[1]); repeats,passes,runtime,capacity=map(int,sys.argv[2:])
try:
 d=json.loads((folder/'method.json').read_text())
 if d.get('status')!='complete' or d.get('warmup_passes')!=passes or d.get('fio_duration_seconds')!=runtime or d.get('repeats')!=repeats or d.get('logical_bytes')!=capacity*1048576 or d.get('fio_address_layout_version')!='explicit-per-job-end-v1': raise ValueError()
 w=json.loads((folder/'warmup.json').read_text())['jobs'][0]
 if w['error'] or w['write']['io_bytes']!=passes*capacity*1048576: raise ValueError()
 for n in range(1,repeats+1):
  r=json.loads((folder/f'run{n}-console.json').read_text())
  if not (folder/f'run{n}.err').exists() or r.get('variant')!=d['variant']+'-paper-full': raise ValueError()
  raw=folder.parents[2]/(d['variant']+'-paper-full')/folder.name/f'run{n}.json'
  fio=json.loads(raw.read_text())['jobs'][0]
  jobfile=folder.parents[2]/(d['variant']+'-paper-full')/folder.name/f'run{n}-jobfile.fio'
  if r.get('address_layout_version')!='explicit-per-job-end-v1' or hashlib.sha256(jobfile.read_bytes()).hexdigest()!=r.get('jobfile_sha256'): raise ValueError()
  if fio['error'] or jobfile.read_text().count('[job')!=64: raise ValueError()
except (OSError,ValueError,KeyError,IndexError,json.JSONDecodeError):
 sys.exit(1)
PY2
}

[[ "$passes" =~ ^[1-9][0-9]*$ && "$repeats" =~ ^[1-9][0-9]*$ && "$runtime" =~ ^[1-9][0-9]*$ ]] || {
  echo 'WARMUP_PASSES, REPEATS and RUNTIME_SECONDS must be positive integers' >&2; exit 2;
}
if [[ "$mode" == status ]]; then
  for variant in $variants; do
    for pattern in $patterns; do
      out="results/paper_fig14/$variant/$pattern"
      if complete_item "$out"; then state=完成
      elif [[ -f "$out/FAILED.txt" ]]; then state=失败
      elif [[ -f "$out/method.json" ]]; then state=未完成
      else state=未运行; fi
      printf '%-12s %-10s %s\n' "$variant" "$pattern" "$state"
    done
  done
  exit 0
fi
if (echo >/dev/tcp/127.0.0.1/2222) >/dev/null 2>&1; then
  echo 'localhost:2222 已被占用；请等待正在运行的 FEMU 实验结束。' >&2; exit 2
fi
[[ -e /dev/kvm ]] || { echo '/dev/kvm is required' >&2; exit 2; }
[[ -f images/guest-overlay.qcow2 && -f images/seed.iso && -f images/guest_ed25519 ]] || {
  echo 'Guest image, cloud-init seed, or SSH key is missing' >&2; exit 2;
}
failures=0
for variant in $variants; do
  case "$variant" in learnedftl|dftl|tpftl|leaftl|ideal) ;; *) echo "Bad variant: $variant" >&2; exit 2;; esac
  binary="bin/qemu-$variant-paper-full"
  [[ -x "$binary" ]] || { echo "Missing $binary; run build_paper.py" >&2; exit 2; }
  for pattern in $patterns; do
    case "$pattern" in randread|read|randwrite|write) ;; *) echo "Bad pattern: $pattern" >&2; exit 2;; esac
    out="results/paper_fig14/$variant/$pattern"
    mkdir -p "$out"
    if [[ "$mode" == resume ]] && complete_item "$out"; then
      echo "[$(date -Iseconds)] $variant $pattern: 已完成，跳过"
      continue
    fi
    if [[ -n "$(find "$out" -mindepth 1 -maxdepth 1 -print -quit)" ]]; then
      archive="results/paper_fig14-archive/$(date -u +%Y%m%dT%H%M%SZ)-$$/$variant/$pattern"
      mkdir -p "$(dirname "$archive")"
      mv "$out" "$archive"
      mkdir -p "$out"
      echo "[$(date -Iseconds)] $variant $pattern: 旧结果已归档到 $archive"
    fi
    rm -f "$out/FAILED.txt"
    qemu_log="images/qemu-$variant-$pattern-paper-full.log"
    echo "[$(date -Iseconds)] $variant $pattern: starting clean FEMU"
    "$base/../femu-local-env.sh" "./$binary" \
      -L upstream/build-femu/pc-bios -machine q35,accel=kvm -cpu host -smp 4 -m 2048 \
      -drive file=images/guest-overlay.qcow2,if=virtio,format=qcow2 \
      -drive file=images/seed.iso,media=cdrom,readonly=on \
      -device "femu,devsz_mb=$capacity_mib,femu_mode=1" \
      -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22 \
      -device virtio-net-pci,netdev=net0 -display none \
      -serial "file:images/guest-serial-$variant-$pattern.log" -monitor none \
      > "$qemu_log" 2>&1 &
    qemu_pid=$!
    ready=0
    for _ in $(seq 1 60); do
      if ssh "${ssh_opts[@]}" ubuntu@127.0.0.1 true >/dev/null 2>&1; then ready=1; break; fi
      kill -0 "$qemu_pid" 2>/dev/null || break
      sleep 2
    done
    if (( !ready )); then
      echo "Boot failed: $variant $pattern; inspect $qemu_log" | tee "$out/FAILED.txt" >&2
      failures=$((failures+1)); stop_guest; continue
    fi
    warmup_rw=write
    warmup_extra=''
    [[ "$pattern" == rand* ]] && { warmup_rw=randwrite; warmup_extra='--norandommap=1 --randrepeat=0'; }
    io_size=$((capacity_mib * passes))
    printf '{"variant":"%s","pattern":"%s","logical_bytes":%s,"physical_bytes":34359738368,"cmt_ratio":"1.5%% LearnedFTL; 3%% DFTL/TPFTL; LeaFTL equal struct-byte budget","warmup_rw":"%s","warmup_bs":"512k","warmup_passes":%s,"randommap":"disabled for random warmup (paper unspecified)","fio_bs":"4k","fio_engine":"psync","fio_jobs":64,"fio_region_mib":476,"fio_address_layout_version":"explicit-per-job-end-v1","fio_duration_seconds":%s,"repeats":%s,"status":"started"}\n' \
      "$variant" "$pattern" "$((capacity_mib * 1048576))" "$warmup_rw" "$passes" "$runtime" "$repeats" \
      > "$out/method.json"
    python3 - "$out/method.json" "$capacity_mib" "$pattern" <<'PY'
import json,sys
p,capacity,pattern=sys.argv[1],int(sys.argv[2]),sys.argv[3]
d=json.load(open(p));d["fio_offset_increment_kib"]=(capacity//64)*1024+(4 if pattern=="read" else 0)
d["offset_note"]="Each of 64 jobs has an explicit offset and a per-job filesize equal to offset plus 476 MiB; read starts add 4 KiB per job. fio size alone is not a region limit when a global filesize is set. This address layout is a disclosed implementation choice; the paper does not specify job starts."
open(p,"w").write(json.dumps(d,indent=2)+"\n")
PY
    echo "[$(date -Iseconds)] $variant $pattern: $passes-pass $warmup_rw prewarm"
    # shellcheck disable=SC2086
    timeout 86400 ssh "${ssh_opts[@]}" ubuntu@127.0.0.1 \
      "sudo fio --name=warmup-$variant-$pattern --filename=/dev/nvme0n1 --rw=$warmup_rw --bs=512k --ioengine=psync --direct=1 --size=${capacity_mib}m --io_size=${io_size}m --numjobs=1 --group_reporting --output-format=json $warmup_extra" \
      > "$out/warmup.json" 2> "$out/warmup.err"
    warmup_rc=$?
    if (( warmup_rc )); then
      echo "Warmup failed with exit $warmup_rc" | tee "$out/FAILED.txt" >&2
      failures=$((failures+1)); stop_guest; continue
    fi
    if ! python3 - "$out/warmup.json" "$((io_size * 1048576))" <<'PY'
import json,sys
job=json.load(open(sys.argv[1]))['jobs'][0]
assert job['error']==0 and job['write']['io_bytes']==int(sys.argv[2]), (job['error'],job['write']['io_bytes'])
PY
    then
      echo 'Warmup byte count or FIO error mismatches' | tee "$out/FAILED.txt" >&2
      failures=$((failures+1)); stop_guest; continue
    fi
    echo "[$(date -Iseconds)] $variant $pattern: $repeats FIO repetitions"
    ok=1
    for run in $(seq 1 "$repeats"); do
      if ! timeout 900 python3 run_fio.py "$variant-paper-full" "$pattern" \
          --capacity-mib "$capacity_mib" --duration "$runtime" --run "$run" \
          --qemu-log "$qemu_log" > "$out/run$run-console.json" 2> "$out/run$run.err"; then
        echo "FIO repetition $run failed" | tee "$out/FAILED.txt" >&2
        failures=$((failures+1)); ok=0; break
      fi
    done
    if (( ok )); then
      python3 - "$out/method.json" <<'PY'
import json,sys
p=sys.argv[1];d=json.load(open(p));d['status']='complete';open(p,'w').write(json.dumps(d,indent=2)+'\n')
PY
    fi
    stop_guest
  done
done
if (( failures )); then
  echo "$failures experiment(s) failed; inspect results/paper_fig14/*/*/FAILED.txt" >&2
  exit 1
fi
echo 'All Figure 14 workloads completed; run plot_fig14.py --variant-suffix=-paper-full --require-complete.'
