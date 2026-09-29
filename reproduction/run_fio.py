#!/usr/bin/env python3
"""Run paper-style FIO measurements against the already booted FEMU guest."""
import argparse
import csv
import hashlib
import json
import re
import shlex
import subprocess
import time
from pathlib import Path

BASE = Path(__file__).resolve().parent
SSH = ["ssh", "-q", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no",
       "-o", "UserKnownHostsFile=/dev/null", "-i", str(BASE / "images/guest_ed25519"),
       "-p", "2222", "ubuntu@127.0.0.1"]
METRIC = re.compile(r"\bMETRICS\s+(.+)$")
ADDRESS_LAYOUT_VERSION = "explicit-per-job-end-v1"


def remote(args, input_text=None):
    cmd = shlex.join(args)
    p = subprocess.run(SSH + [cmd], input=input_text, text=True, capture_output=True, check=False)
    if p.returncode:
        raise RuntimeError(f"guest command failed ({p.returncode}): {cmd}\n{p.stderr[-1000:]}")
    return p.stdout


def metric_line(log_path, start):
    for _ in range(30):
        with log_path.open("r", errors="replace") as f:
            f.seek(start)
            lines = f.readlines()
        found = [METRIC.search(line) for line in lines]
        found = [m for m in found if m]
        if found:
            return {k: float(v) if "." in v else int(v)
                    for k, v in (token.split("=", 1) for token in found[-1].group(1).split())}
        time.sleep(0.1)
    raise RuntimeError("FEMU did not emit metrics after admin-passthru")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("variant")
    ap.add_argument("pattern", choices=["randread", "read", "randwrite", "write"])
    ap.add_argument("--run", type=int, default=1)
    ap.add_argument("--duration", type=int, default=15)
    ap.add_argument("--capacity-mib", type=int, default=7680)
    ap.add_argument("--qemu-log", type=Path, default=BASE / "images/qemu-learnedftl-8g.log")
    ap.add_argument("--no-metrics", action="store_true", help="baseline binary has no metric hook")
    ap.add_argument("--offset-increment-kib", type=int,
                    help="per-job start spacing in KiB; defaults to one region, plus 4 KiB for sequential reads")
    args = ap.parse_args()
    if args.capacity_mib < 64:
        raise SystemExit("capacity-mib must be at least 64")
    dest = BASE / "results" / args.variant / args.pattern
    dest.mkdir(parents=True, exist_ok=True)
    if not args.no_metrics:
        remote(["sudo", "nvme", "admin-passthru", "/dev/nvme0", "--opcode=0xef", "--cdw10=8"])
    region_kib = (args.capacity_mib // 64) * 1024
    offset_increment_kib = args.offset_increment_kib or (region_kib + (4 if args.pattern == "read" else 0))
    if offset_increment_kib < region_kib or 63 * offset_increment_kib + region_kib > args.capacity_mib * 1024:
        raise SystemExit("offset increment must keep 64 non-overlapping regions within the device")
    # fio's filesize overrides size as the I/O region. One global filesize
    # makes later jobs run beyond the intended per-job region. Give each job
    # its own end boundary; a read-only iolog probe verified this on fio 3.16.
    lines = ["[global]", "filename=/dev/nvme0n1", f"rw={args.pattern}",
             "bs=4k", "ioengine=psync", "direct=1", "group_reporting=1",
             "time_based=1", f"runtime={args.duration}", "randrepeat=0"]
    for job_no in range(64):
        start_kib = job_no * offset_increment_kib
        end_kib = start_kib + region_kib
        lines += [f"[job{job_no:02d}]", f"offset={start_kib}k",
                  f"filesize={end_kib}k", f"size={region_kib}k"]
    jobfile = "\n".join(lines) + "\n"
    jobfile_path = dest / f"run{args.run}-jobfile.fio"
    jobfile_path.write_text(jobfile)
    jobfile_sha256 = hashlib.sha256(jobfile.encode()).hexdigest()
    remote_path = f"/tmp/codex-fig14-{args.pattern}-{args.run}.fio"
    remote(["sudo", "tee", remote_path], input_text=jobfile)
    fio = ["sudo", "fio", remote_path, "--output-format=json"]
    stdout = remote(fio)
    (dest / f"run{args.run}.json").write_text(stdout)
    result = json.loads(stdout)
    job = result["jobs"][0]
    if job["error"]:
        raise RuntimeError(f"FIO reported error {job['error']}")
    metrics = {}
    if not args.no_metrics:
        with args.qemu_log.open("r", errors="replace") as f:
            f.seek(0, 2)
            pos = f.tell()
        remote(["sudo", "nvme", "admin-passthru", "/dev/nvme0", "--opcode=0xef", "--cdw10=9"])
        metrics = metric_line(args.qemu_log, pos)
        (dest / f"run{args.run}-metrics.json").write_text(json.dumps(metrics, indent=2) + "\n")
    operation = "read" if "read" in args.pattern else "write"
    io = job[operation]
    row = {"variant": args.variant, "pattern": args.pattern, "run": args.run,
           "address_layout_version": ADDRESS_LAYOUT_VERSION,
           "jobfile_sha256": jobfile_sha256,
           "throughput_MiB_s": io["bw_bytes"] / 1048576,
           "iops": io["iops"], "io_bytes": io["io_bytes"],
           "runtime_ms": io["runtime"], "p99_us": None,
           **metrics}
    clat = io.get("clat_ns") or io.get("clat_us") or {}
    p99 = clat.get("percentile", {}).get("99.000000")
    if p99 is not None:
        row["p99_us"] = p99 / (1000 if "clat_ns" in io else 1)
    csv_path = BASE / "results" / "fio_runs.csv"
    existing = []
    if csv_path.exists():
        with csv_path.open(newline="") as f:
            existing = list(csv.DictReader(f))
    existing = [old for old in existing if not (old.get("variant") == row["variant"] and old.get("pattern") == row["pattern"] and old.get("run") == str(row["run"]))]
    existing.append(row)
    fields = list(dict.fromkeys(k for item in existing for k in item))
    with csv_path.open("w", newline="") as f:
        w = csv.DictWriter(f, fields)
        w.writeheader()
        w.writerows(existing)
    print(json.dumps(row, indent=2))


if __name__ == "__main__":
    main()
