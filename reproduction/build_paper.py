#!/usr/bin/env python3
"""Build paper-parameter FEMU binaries with measurement-only counters.

Rewrites author source only during each build and restores it afterward.
Existing running QEMU processes use copied binaries and are unaffected.
"""
import argparse
import shutil
import subprocess
from pathlib import Path

BASE = Path(__file__).resolve().parent
SRC = BASE / 'upstream'
BB = SRC / 'hw/femu/bbssd/bb.c'
MESON = SRC / 'hw/femu/meson.build'
FTL = SRC / 'hw/femu/bbssd'
VARIANTS = {'learnedftl': 'ld-tpftl', 'dftl': 'dftl', 'tpftl': 'tpftl',
            'leaftl': 'leaftl', 'ideal': 'ftl'}


def verify_build_tree():
    """Reject build caches copied from another checkout before changing source."""
    build = SRC / 'build-femu'
    config = build / 'config-host.mak'
    ninja = build / 'build.ninja'
    if not config.is_file() or not ninja.is_file():
        raise RuntimeError('FEMU build is not configured; see reproduction/README.zh-CN.md')
    settings = config.read_text()
    expected = f'SRC_PATH={SRC.resolve()}'
    if expected not in settings.splitlines():
        raise RuntimeError(f'Wrong FEMU build source in {config}; expected {expected}. Reconfigure on ext4.')
    old_mount = '/run/media/zheng/D14DA81B6A18BBFA/FEMU'
    if old_mount in settings or old_mount in ninja.read_text():
        raise RuntimeError(f'Build cache still points to read-only NTFS: {build}. Reconfigure on ext4.')


def metric_code(variant):
    st = 'ssd->stat'
    if variant == 'learnedftl':
        vals = ('st->access_cnt, st->cmt_hit_cnt, st->cmt_miss_cnt, '
                'st->model_hit_num, st->model_use_num, st->gc_times, '
                'st->write_num, st->read_joule, st->write_joule, st->erase_joule')
    elif variant in ('dftl', 'tpftl'):
        vals = ('st->access_cnt, st->cmt_hit_cnt, st->cmt_miss_cnt, '
                '(uint64_t)0, (uint64_t)0, st->nand_erases, '
                'st->nand_writes, st->read_joule, st->write_joule, st->erase_joule')
    elif variant == 'leaftl':
        vals = ('st->access_cnt, st->cmt_hit_cnt, st->cmt_miss_cnt, '
                'st->model_hit, st->model_hit, (uint64_t)st->gc_cnt, '
                '(uint64_t)st->wa_cnt, st->read_joule, st->write_joule, st->erase_joule')
    else:
        vals = ('(uint64_t)0, (uint64_t)0, (uint64_t)0, '
                '(uint64_t)0, (uint64_t)0, st->nand_erases, '
                'st->nand_writes, st->read_joule, st->write_joule, st->erase_joule')
    return '''static void reset_stat(struct ssd *ssd)
{
    memset(&ssd->stat, 0, sizeof(ssd->stat));
}

static void print_stat(struct ssd *ssd)
{
    struct statistics *st = &ssd->stat;
    femu_log("METRICS access=%" PRIu64 " cmt_hit=%" PRIu64
             " cmt_miss=%" PRIu64 " model_hit=%" PRIu64
             " model_use=%" PRIu64 " gc=%" PRIu64
             " writes=%" PRIu64 " read_energy=%.3Lf write_energy=%.3Lf"
             " erase_energy=%.3Lf\\n",
             ''' + vals + ''');
}

'''


def patched_source(variant, content):
    if variant in ('dftl', 'tpftl'):
        content = content.replace('spp->pgs_per_blk = 256;', 'spp->pgs_per_blk = 512;')
        content = content.replace('spp->tt_cmt_size = spp->tt_blks / 2;',
                                  'spp->tt_cmt_size = spp->tt_pgs * 3 / 100; /* paper 3% */')
        for case, updates in [('READ', 'nand_reads++;\n        ssd->stat.read_joule += 3.5;'),
                              ('WRITE', 'nand_writes++;\n        ssd->stat.write_joule += 16.7;'),
                              ('ERASE', 'nand_erases++;\n        ssd->stat.erase_joule += 132;')]:
            old = f'case NAND_{case}:\n'
            assert content.count(old) == 1
            content = content.replace(old, old + '        ssd->stat.' + updates + '\n')
    elif variant == 'learnedftl':
        # Record the allocator state only when free lines are exhausted.
        # This does not alter the FTL path or print per I/O.
        old_exhaust = '        ftl_err("No free lines left in [%s]31232131231321 !!!!\\n", ssd->ssdname);'
        new_exhaust = ('        ftl_err("GC_EXHAUST free=%d victim=%d full=%d wp=%d wp_vic=%d '
                       'trans_vic=%d\\n", lm->free_line_cnt, lm->victim_line_cnt, '
                       'lm->full_line_cnt, wpp->id, wpp->vic_cnt, ssd->trans_wp.vic_cnt);\n'
                       + old_exhaust)
        assert content.count(old_exhaust) == 1
        content = content.replace(old_exhaust, new_exhaust)
        content = content.replace('spp->tt_cmt_size = 8192;', 
                                  'spp->tt_cmt_size = spp->tt_pgs * 15 / 1000; /* paper 1.5% */')
    elif variant == 'leaftl':
        # Optional author GC trace points to a private absolute path. fopen()
        # fails here; keep its fprintf from dereferencing NULL at first GC.
        old_gc_log = '        if (flag) {\n                fprintf(gc_fp, "%ld\\n",lun->next_lun_avail_time);'
        assert content.count(old_gc_log) == 1
        content = content.replace(old_gc_log, old_gc_log.replace('if (flag)', 'if (flag && gc_fp)'))
        # quick_sort uses an inclusive high index; the author passed the
        # element count in both flush paths, reading one element past the end.
        gc_sort = 'quick_sort(buffer_lpns, 0, pointer);'
        buffer_sort = 'quick_sort(ssd->buffer_lpns, 0, sp->buffer_size);'
        assert content.count(gc_sort) == 1 and content.count(buffer_sort) == 1
        content = content.replace(gc_sort, 'if (pointer > 1) quick_sort(buffer_lpns, 0, pointer - 1);')
        content = content.replace(buffer_sort, 'quick_sort(ssd->buffer_lpns, 0, sp->buffer_size - 1);')
        # sizeof(dftl.cmt_entry)=48, sizeof(leaftl.cmt_entry)=80 on x86-64.
        # This matches allocated cache-struct bytes, excluding dynamic segments.
        content = content.replace('spp->tt_cmt_size = 8192;',
                                  'spp->tt_cmt_size = (spp->tt_pgs * 3 / 100) * 48 / 80;')
    elif variant == 'ideal':
        for case, field in [('READ', 'nand_reads'), ('WRITE', 'nand_writes'),
                            ('ERASE', 'nand_erases')]:
            old = f'case NAND_{case}:\n'
            assert content.count(old) == 1
            content = content.replace(old, old + f'        ssd->stat.{field}++;\n')
    return content


def patched_header(variant, content):
    if variant in ('dftl', 'tpftl'):
        old = '    uint64_t access_cnt;\n};'
        assert old in content
        content = content.replace(old, '''    uint64_t access_cnt;
    uint64_t nand_reads, nand_writes, nand_erases;
    long double read_joule, write_joule, erase_joule;
};''')
    elif variant == 'ideal':
        old = 'struct statistics {\n'
        assert old in content
        content = content.replace(old, old + '    uint64_t nand_reads, nand_writes, nand_erases;\n')
    return content


def build(variant):
    verify_build_tree()
    stem = VARIANTS[variant]
    cfile = FTL / f'{stem}.c'
    hfile = FTL / f'{stem}.h'
    original = {BB: BB.read_text(), MESON: MESON.read_text(),
                cfile: cfile.read_text(), hfile: hfile.read_text()}
    try:
        clean_bb = subprocess.check_output(['git', 'show', 'HEAD:hw/femu/bbssd/bb.c'],
                                           cwd=SRC, text=True)
        clean_bb = clean_bb.replace('"./ld-tpftl.h"', f'"./{stem}.h"')
        first = clean_bb.index('static void reset_stat(')
        last = clean_bb.index('static void bb_flip(', first)
        BB.write_text(clean_bb[:first] + metric_code(variant) + clean_bb[last:])
        MESON.write_text(original[MESON].replace("'bbssd/ld-tpftl.c'", f"'bbssd/{stem}.c'"))
        cfile.write_text(patched_source(variant, original[cfile]))
        hfile.write_text(patched_header(variant, original[hfile]))
        logpath = BASE / 'bin' / f'build-{variant}-paper-full.log'
        with logpath.open('w') as log:
            subprocess.run(['make', '-C', str(SRC / 'build-femu'), '-j4'],
                           stdout=log, stderr=subprocess.STDOUT, check=True)
        target = BASE / 'bin' / f'qemu-{variant}-paper-full'
        shutil.copy2(SRC / 'build-femu/x86_64-softmmu/qemu-system-x86_64', target)
        print(f'{variant}: {target}', flush=True)
    finally:
        for path, content in original.items():
            path.write_text(content)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('variants', nargs='*', choices=list(VARIANTS),
                        default=list(VARIANTS))
    args = parser.parse_args()
    (BASE / 'bin').mkdir(exist_ok=True)
    for variant in args.variants:
        build(variant)


if __name__ == '__main__':
    main()
