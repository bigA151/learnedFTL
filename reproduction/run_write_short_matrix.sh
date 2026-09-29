#!/usr/bin/env bash
# Diagnostic only: three independent fresh-FEMU trials per random-write policy.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
[[ "$(findmnt -T "$PWD" -no FSTYPE)" == ext4 ]] || { echo 'ext4 required' >&2; exit 2; }
[[ -x reproduction/bin/qemu-learnedftl-cross-borrow-greedy ]] || exit 2
run_id="$(date -u +%Y%m%dT%H%M%SZ)-$$"
index="reproduction/results/diagnostics/write-matrix-$run_id"
mkdir -p "$index"
sha256sum reproduction/bin/qemu-learnedftl-cross-borrow-greedy > "$index/binary.sha256"
printf 'policy,repeat,result,exit_code,fio_mean_mib_s,last10_mib_s,borrow_pages,paired_gc,gc_copies\n' > "$index/matrix.csv"
for policy in default low_sticky; do
  for repeat in 1 2 3; do
    log="$index/${policy}-${repeat}.out"
    echo "Starting $policy repeat $repeat (fresh FEMU disk)"
    rc=0
    if [[ "$policy" == default ]]; then
      env -u CG_ALLOW_LOW_FREE -u CG_BORROW_STICKY -u CG_BORROW_ALLOW_MULTI \
        GC_DIAG_BINARY=qemu-learnedftl-cross-borrow-greedy \
        GC_EXPLICIT_RANGES=1 GC_FIO_JOBS=64 GC_MEASURE_SIZE_MIB=476 GC_FIO_PATTERN=randwrite \
        WARMUP_PASSES=0 RUNTIME_SECONDS=40 GC_BW_LOG=1 GC_POST_READ=0 \
        bash reproduction/run_gc_diag.sh > "$log" 2>&1 || rc=$?
    else
      CG_ALLOW_LOW_FREE=1 CG_BORROW_ALLOW_MULTI=1 CG_BORROW_STICKY=1 \
        GC_DIAG_BINARY=qemu-learnedftl-cross-borrow-greedy \
        GC_EXPLICIT_RANGES=1 GC_FIO_JOBS=64 GC_MEASURE_SIZE_MIB=476 GC_FIO_PATTERN=randwrite \
        WARMUP_PASSES=0 RUNTIME_SECONDS=40 GC_BW_LOG=1 GC_POST_READ=0 \
        bash reproduction/run_gc_diag.sh > "$log" 2>&1 || rc=$?
    fi
    result="$(sed -n 's/^Output: //p' "$log" | tail -1)"
    if [[ -n "$result" && -f "$result/fio.json" && "$rc" -eq 0 ]]; then
      python3 reproduction/summarize_gc_bw.py "$result" > "$index/${policy}-${repeat}-bw.json"
      if [[ "$policy" == low_sticky ]]; then
        python3 reproduction/validate_cross_group.py "$result" --require-borrow --require-pair --min-component-groups 2 > "$index/${policy}-${repeat}-validation.txt"
      else
        python3 reproduction/validate_cross_group.py "$result" > "$index/${policy}-${repeat}-validation.txt"
      fi
    fi
    python3 - "$index/matrix.csv" "$policy" "$repeat" "$result" "$rc" <<'PY'
import csv,json,re,sys
from pathlib import Path
index,policy,repeat,result,rc=sys.argv[1:]
row=[policy,repeat,result,rc,'','','','','']
if result:
 p=Path(result)
 try:
  bw=json.loads((p/'fio-bw-summary.json').read_text())
  log=(p/'qemu.log').read_text(errors='replace')
  borrow=re.search(r'CG_BORROW attempts=\d+ success=(\d+) pages=(\d+) paired_gc=(\d+)',log)
  ident=re.search(r'PHYS_ID_SUMMARY writes=\d+ copies=(\d+) reads=\d+',log)
  row[4:]=[f"{bw['fio_mean_mib_per_s']:.3f}",f"{bw['last_window_mib_per_s']:.3f}",borrow.group(2) if borrow else '',borrow.group(3) if borrow else '',ident.group(1) if ident else '']
 except (OSError,KeyError,ValueError): pass
with open(index,'a',newline='') as f: csv.writer(f).writerow(row)
print('Finished',policy,repeat,'exit',rc,'result',result)
PY
  done
done
sha256sum -c "$index/binary.sha256"
echo "Matrix: $index/matrix.csv"
