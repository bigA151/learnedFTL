#!/usr/bin/env bash
# Two isolated VMs: dense sequential/random writes and full sampled CMT contents.
set -euo pipefail
base="$(cd "$(dirname "$0")" && pwd)"
cd "$base/.."
case "$(findmnt -T "$base" -n -o FSTYPE)" in
  ntfs|ntfs3|fuseblk) echo '请从 ext4 目录运行。' >&2; exit 2;;
esac
seconds="${CMT_SECONDS:-20}"
[[ "$seconds" =~ ^[1-9][0-9]*$ ]] || exit 2
if [[ "${1:-}" != --skip-build ]]; then
  GC_DIAG_FIX=1 GC_DIAG_CMT_FREE=1 GC_DIAG_DONOR_AUDIT=0 \
  GC_DIAG_PHYS_AUDIT=0 GC_DIAG_FULL_GTD_GC=0 GC_DIAG_CROSS_BORROW=0 \
  GC_DIAG_SINGLE_ERASE=0 GC_DIAG_GREEDY_GROUP=0 GC_DIAG_GUARD_REENTRY=0 \
  GC_DIAG_FORCE_FALLBACK=0 GC_DIAG_FORCE_EDGE=0 bash ./femu-local-env.sh python3 reproduction/build_gc_diag.py
fi
batch="$base/results/diagnostics/cmt-free-$(date -u +%Y%m%dT%H%M%SZ)-$$"
mkdir -p "$batch"
echo "$batch" > "$base/results/diagnostics/cmt-free-latest.txt"
printf 'RUNNING\n' > "$batch/STATUS.txt"
trap 'rc=$?; if ((rc != 0)); then printf "FAILED exit=%s\n" "$rc" > "$batch/STATUS.txt"; fi' EXIT
for pattern in write randwrite; do
  echo "开始 $pattern：64 作业、4 KiB、${seconds} 秒，每作业 128 MiB，全新 FEMU。"
  # All settings are explicit; no inherited six-pass prewarm or preparation.
  WARMUP_PASSES=0 GC_WARMUP_IO_MIB=0 GC_PREPARE_SECONDS=0 GC_PREPARE_SPARSE_GROUPS=0 \
  RUNTIME_SECONDS="$seconds" GC_FIO_PATTERN="$pattern" GC_FIO_JOBS=64 \
  GC_MEASURE_SIZE_MIB=128 GC_OFFSET_INCREMENT_KIB=487428 GC_EXPLICIT_RANGES=1 \
  GC_POST_READ=0 GC_BW_LOG=1 GC_DIAG_BINARY=qemu-learnedftl-cmt-free-diag \
    bash reproduction/run_gc_diag.sh 2>&1 | tee "$batch/$pattern-console.log"
  run="$(sed -n 's/^Output: //p' "$batch/$pattern-console.log")"
  [[ -d "$run" ]] || { echo '找不到本轮结果目录。' >&2; exit 1; }
  python3 reproduction/summarize_cmt_free.py "$run" | tee "$batch/$pattern-summary.json"
done
printf 'COMPLETE\n' > "$batch/STATUS.txt"
echo "两轮完成且数据验证通过：$batch"
