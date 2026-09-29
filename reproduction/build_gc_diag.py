#!/usr/bin/env python3
"""Build an isolated LearnedFTL binary with aggregate GC diagnostics."""
from pathlib import Path
import shutil
import os
import subprocess
import build_paper
import cross_group_patch
import difflib

BASE = Path(__file__).resolve().parent
SRC = BASE / "upstream"
C = SRC / "hw/femu/bbssd/ld-tpftl.c"
H = SRC / "hw/femu/bbssd/ld-tpftl.h"
BB = SRC / "hw/femu/bbssd/bb.c"
MESON = SRC / "hw/femu/meson.build"

COUNTERS = r'''
static struct {
    uint64_t calls, branch_wp, branch_trans, branch_low, branch_none;
    uint64_t alloc_attempt, alloc_empty, alloc_empty_with_victim;
    uint64_t freed, user_writes, gc_writes;
    uint64_t min_free, min_victim;
} gc_diag;
static bool gc_diag_force_fallback;
static bool gc_diag_skip_once;
static bool gc_diag_choice_logged;
static bool gc_diag_gc_busy_wp[512];

static uint64_t gc_diag_group_invalid_pages(struct write_pointer *wp)
{
    uint64_t invalid = 0;
    struct wp_lines *item = wp->wpl ? wp->wpl->next : NULL;
    while (item) {
        if (item->line) invalid += item->line->ipc;
        item = item->next;
    }
    return invalid;
}

static void gc_diag_donor_audit(struct ssd *ssd, const char *phase)
{
    uint64_t open_lines = 0, open_pages = 0;
    uint64_t untrained_lines = 0, untrained_pages = 0, partial_lines = 0;
    int groups = ssd->sp.tt_line_wps < 240 ? ssd->sp.tt_line_wps : 240;
    for (int i = 0; i < groups; i++) {
        struct write_pointer *wp = &ssd->gtd_wps[i];
        if (!wp->curline || wp->curline->rest <= 0) continue;
        int first = i * ssd->sp.trans_per_line;
        int last = first + ssd->sp.trans_per_line;
        if (last > ssd->sp.tt_gtd_size) last = ssd->sp.tt_gtd_size;
        if (first >= last) continue;
        int trained = 0;
        for (int j = first; j < last; j++) {
            for (int k = 0; k < MAX_INTERVALS; k++) {
                if (ssd->lr_nodes[j].brks[k].valid_cnt > 0) {
                    trained++;
                    break;
                }
            }
        }
        open_lines++;
        open_pages += wp->curline->rest;
        if (!trained) {
            untrained_lines++;
            untrained_pages += wp->curline->rest;
        } else if (trained < last-first) {
            partial_lines++;
        }
    }
    femu_log("GC_DONOR phase=%s open_lines=%" PRIu64
             " open_pages=%" PRIu64 " untrained_lines=%" PRIu64
             " untrained_pages=%" PRIu64 " partial_lines=%" PRIu64
             " free_lines=%d\n", phase, open_lines, open_pages,
             untrained_lines, untrained_pages, partial_lines,
             ssd->lm.free_line_cnt);
}

void gc_diag_reset(struct ssd *ssd)
{
    memset(&gc_diag, 0, sizeof(gc_diag));
    gc_diag.min_free = ssd->lm.free_line_cnt;
    gc_diag.min_victim = ssd->lm.victim_line_cnt;
    gc_diag_donor_audit(ssd, "before-fio");
}

static void gc_diag_before_free(struct ssd *ssd, struct line *target)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *line;
    int in_free = 0, in_victim = 0;
    QTAILQ_FOREACH(line, &lm->free_line_list, entry)
        if (line == target) in_free++;
    QTAILQ_FOREACH(line, &lm->victim_list, entry)
        if (line == target) in_victim++;
    if (in_free || in_victim != 1) {
        femu_log("GC_BAD_FREE_TARGET id=%d in_free=%d in_victim=%d"
                 " free_count=%d victim_count=%d\n", target->id,
                 in_free, in_victim, lm->free_line_cnt, lm->victim_line_cnt);
        fflush(stdout);
        abort();
    }
}

static bool gc_diag_victim_contains(struct ssd *ssd, struct line *target)
{
    struct line *line;
    QTAILQ_FOREACH(line, &ssd->lm.victim_list, entry)
        if (line == target) return true;
    return false;
}

static void gc_diag_check(struct ssd *ssd, const char *where)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *line;
    int free_seen = 0, victim_seen = 0;
    QTAILQ_FOREACH(line, &lm->free_line_list, entry) free_seen++;
    QTAILQ_FOREACH(line, &lm->victim_list, entry) victim_seen++;
    if (free_seen != lm->free_line_cnt || victim_seen != lm->victim_line_cnt) {
        femu_log("GC_INVARIANT where=%s free_count=%d free_queue=%d"
                 " victim_count=%d victim_queue=%d\n", where,
                 lm->free_line_cnt, free_seen, lm->victim_line_cnt, victim_seen);
        fflush(stdout);
        abort();
    }
}

void gc_diag_report(struct ssd *ssd)
{
    gc_diag_donor_audit(ssd, "after-fio");
    femu_log("GC_DIAG calls=%" PRIu64 " wp=%" PRIu64
             " trans=%" PRIu64 " low=%" PRIu64 " none=%" PRIu64
             " alloc=%" PRIu64 " empty=%" PRIu64
             " empty_with_victim=%" PRIu64 " freed=%" PRIu64
             " user_writes=%" PRIu64 " gc_writes=%" PRIu64
             " min_free=%" PRIu64 " min_victim=%" PRIu64
             " end_free=%d end_victim=%d end_full=%d\n",
             gc_diag.calls, gc_diag.branch_wp, gc_diag.branch_trans,
             gc_diag.branch_low, gc_diag.branch_none,
             gc_diag.alloc_attempt, gc_diag.alloc_empty,
             gc_diag.alloc_empty_with_victim, gc_diag.freed,
             gc_diag.user_writes, gc_diag.gc_writes,
             gc_diag.min_free, gc_diag.min_victim,
             ssd->lm.free_line_cnt, ssd->lm.victim_line_cnt,
             ssd->lm.full_line_cnt);
}
'''

def once(data, old, new):
    n = data.count(old)
    if n != 1:
        raise RuntimeError(f"expected one match, found {n}: {old[:90]!r}")
    return data.replace(old, new, 1)

def main():
    build_paper.verify_build_tree()
    original = {p: p.read_text() for p in (C, H, BB, MESON)}
    try:
        c = build_paper.patched_source("learnedftl", original[C])
        donor_audit = os.environ.get("GC_DIAG_DONOR_AUDIT") == "2"
        phys_audit = os.environ.get("GC_DIAG_PHYS_AUDIT") == "1"
        full_gtd_gc = os.environ.get("GC_DIAG_FULL_GTD_GC") == "1"
        cross_borrow = os.environ.get("GC_DIAG_CROSS_BORROW") == "1"
        if cross_borrow and not full_gtd_gc:
            raise RuntimeError("cross-group borrowing requires full GTD GC")
        if full_gtd_gc and not (phys_audit and os.environ.get("GC_DIAG_FIX") == "1"):
            raise RuntimeError("full GTD GC requires GC_DIAG_FIX=1 and GC_DIAG_PHYS_AUDIT=1")
        single_erase = os.environ.get("GC_DIAG_SINGLE_ERASE") == "1"
        greedy_group = os.environ.get("GC_DIAG_GREEDY_GROUP") == "1"
        guarded = os.environ.get("GC_DIAG_GUARD_REENTRY") == "1"
        if guarded and not greedy_group:
            raise RuntimeError("reentry guard diagnostic requires greedy group")
        fix = os.environ.get("GC_DIAG_FIX") == "1"
        force_fallback = os.environ.get("GC_DIAG_FORCE_FALLBACK") == "1"
        force_edge = os.environ.get("GC_DIAG_FORCE_EDGE") == "1"
        if force_edge: force_fallback = True
        if force_fallback and not fix:
            raise RuntimeError("forced fallback requires GC_DIAG_FIX=1")
        if fix:
            # The author fallback assigned a write pointer but left vl NULL,
            # so its guarded GC body never ran when all candidates had vic_cnt=1.
            c = once(c, "                write_back_wp = ssd->line2write_pointer[tvl->id];\n            }\n            if (write_back_wp->vic_cnt == 1)",
                     "                if (!tvl) return false;\n"
                     "                write_back_wp = ssd->line2write_pointer[tvl->id];\n"
                     "                vl = tvl;\n            }\n            if (write_back_wp->vic_cnt == 1)")
            c = once(c, "                    if (write_back_wp != wpp) {\n                        QTAILQ_INSERT_TAIL(&lm->victim_list, write_back_wp->curline, entry);",
                         "                    if (write_back_wp != wpp && write_back_wp->curline &&\n"
                         "                        !gc_diag_victim_contains(ssd, write_back_wp->curline)) {\n"
                         "                        QTAILQ_INSERT_TAIL(&lm->victim_list, write_back_wp->curline, entry);")
        if greedy_group:
            if not fix:
                raise RuntimeError("greedy group selection requires GC_DIAG_FIX=1")
            old = """            while (tvl) {
                struct write_pointer *tmp_wp = ssd->line2write_pointer[tvl->id];
                if (tmp_wp->vic_cnt > 1) {
                    write_back_wp = tmp_wp;
                    vl = tvl;
                    break;
                }

                tvl = tvl->entry.tqe_next;
            }
            if (!tvl) {
                tvl = QTAILQ_FIRST(&lm->victim_list);
                if (!tvl) return false;
                write_back_wp = ssd->line2write_pointer[tvl->id];
                vl = tvl;
            }
"""
            new = """            /* Choose the group with the most invalid pages. Prefer a group
             * with more than one owned line to preserve a migration target. */
            uint64_t best_invalid = 0;
            for (int pass = 0; pass < 2 && !vl; pass++) {
                struct line *scan;
                QTAILQ_FOREACH(scan, &lm->victim_list, entry) {
                    struct write_pointer *owner = ssd->line2write_pointer[scan->id];
                    if (!owner || (pass == 0 && owner->vic_cnt <= 1)) continue;
                    uint64_t invalid = gc_diag_group_invalid_pages(owner);
                    if (!vl || invalid > best_invalid ||
                        (invalid == best_invalid && scan->ipc > vl->ipc)) {
                        vl = scan;
                        write_back_wp = owner;
                        best_invalid = invalid;
                    }
                }
            }
            if (!vl) return false;
            tvl = vl;
"""
            c = once(c, old, new)
        if guarded:
            c = once(c, "                    if (!owner || (pass == 0 && owner->vic_cnt <= 1)) continue;",
                     "                    if (!owner || gc_diag_gc_busy_wp[owner->id] ||\n"
                     "                        (pass == 0 && owner->vic_cnt <= 1)) continue;")
            c = once(c, "    if (wpp && wpp->vic_cnt >= gc_threshold) {",
                     "    if (wpp && !gc_diag_gc_busy_wp[wpp->id] && wpp->vic_cnt >= gc_threshold) {")
            c = once(c, "    } else if (ssd->trans_wp.vic_cnt >= gc_threshold) {",
                     "    } else if (!gc_diag_gc_busy_wp[ssd->trans_wp.id] &&\n"
                     "               ssd->trans_wp.vic_cnt >= gc_threshold) {")
            c = once(c,
                     "static int line_do_gc(struct ssd *ssd, bool force, struct write_pointer *wpp, struct line *victim_line)\n{",
                     "static int line_do_gc(struct ssd *ssd, bool force, struct write_pointer *wpp, struct line *victim_line)\n"
                     "{\n    if (gc_diag_gc_busy_wp[wpp->id]) abort();\n"
                     "    gc_diag_gc_busy_wp[wpp->id] = true;")
            c = once(c,
                     "    mark_line_free(ssd, &ppa);\n\n    return 0;\n}\n\nstatic bool model_predict",
                     "    mark_line_free(ssd, &ppa);\n\n"
                     "    gc_diag_gc_busy_wp[wpp->id] = false;\n    return 0;\n}\n\nstatic bool model_predict")
            c = once(c,
                     "static int batch_line_do_gc(struct ssd* ssd, bool force, struct write_pointer *wpp, struct line *delete_line) {",
                     "static int batch_line_do_gc(struct ssd* ssd, bool force, struct write_pointer *wpp, struct line *delete_line) {\n"
                     "    if (gc_diag_gc_busy_wp[wpp->id]) abort();\n"
                     "    gc_diag_gc_busy_wp[wpp->id] = true;")
            c = once(c,
                     "    model_training(ssd, wpp, group_gtd_lpns, group_gtd_index, start_gtd);\n"
                     "    /* update line status */\n    \n\n    return 0;\n    \n}\n\nstatic int line_do_gc",
                     "    model_training(ssd, wpp, group_gtd_lpns, group_gtd_index, start_gtd);\n"
                     "    /* update line status */\n    \n\n    gc_diag_gc_busy_wp[wpp->id] = false;\n"
                     "    return 0;\n    \n}\n\nstatic int line_do_gc")
        if single_erase:
            if not fix:
                raise RuntimeError("single erase audit requires GC_DIAG_FIX=1")
            c = once(c, "    free_all_blocks(ssd, &ppa);\n\n    struct wp_lines *wpl = wpp->wpl;",
                     "    /* gc_read_all_valid_data already erased each block once. */\n\n"
                     "    struct wp_lines *wpl = wpp->wpl;")
        c = once(c, "static int gc_threshold = 5;", COUNTERS + "\nstatic int gc_threshold = 5;")
        if phys_audit:
            c = once(c, "void gc_diag_report(struct ssd *ssd)", "static uint64_t phys_audit_reads, phys_audit_copies, phys_audit_writes;\nvoid gc_diag_report(struct ssd *ssd)")
            phys_code = r'''
static uint64_t *phys_audit_lpn, *phys_audit_gen, *phys_audit_expected, *phys_audit_pending;
static void phys_audit_init(struct ssd *ssd)
{
    uint64_t n = ssd->sp.tt_pgs;
    phys_audit_lpn = g_malloc(sizeof(uint64_t) * n);
    phys_audit_gen = g_malloc0(sizeof(uint64_t) * n);
    phys_audit_expected = g_malloc0(sizeof(uint64_t) * n);
    phys_audit_pending = g_malloc0(sizeof(uint64_t) * n);
    for (uint64_t i = 0; i < n; i++) phys_audit_lpn[i] = INVALID_LPN;
}
static void phys_audit_check(struct ssd *ssd, uint64_t lpn, struct ppa *ppa, const char *stage)
{
    uint64_t idx = ppa2pgidx(ssd, ppa);
    if (phys_audit_lpn[idx] != lpn || !phys_audit_gen[idx] ||
        phys_audit_gen[idx] != phys_audit_expected[lpn]) {
        femu_log("PHYS_ID_MISMATCH stage=%s lpn=%" PRIu64 " ppa=%" PRIu64
                 " stored_lpn=%" PRIu64 " physical_gen=%" PRIu64
                 " expected_gen=%" PRIu64 "\n", stage, lpn, ppa->ppa,
                 phys_audit_lpn[idx], phys_audit_gen[idx], phys_audit_expected[lpn]);
        fflush(stdout); abort();
    }
}
static void phys_audit_host(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    uint64_t idx = ppa2pgidx(ssd, ppa);
    phys_audit_lpn[idx] = lpn;
    phys_audit_gen[idx] = ++phys_audit_expected[lpn];
    phys_audit_writes++;
}
static void phys_audit_capture(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    phys_audit_check(ssd, lpn, ppa, "gc-source");
    phys_audit_pending[lpn] = phys_audit_gen[ppa2pgidx(ssd, ppa)];
}
static void phys_audit_move(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    uint64_t idx = ppa2pgidx(ssd, ppa);
    if (!phys_audit_pending[lpn]) { femu_log("PHYS_ID_NO_SOURCE lpn=%" PRIu64 "\n", lpn); abort(); }
    phys_audit_lpn[idx] = lpn;
    phys_audit_gen[idx] = phys_audit_pending[lpn];
    phys_audit_pending[lpn] = 0;
    phys_audit_copies++;
}
'''
            c = once(c, "static inline int victim_line_cmp_pri", phys_code + "\nstatic inline int victim_line_cmp_pri")
            c = once(c, "        ssd->rmap[i] = INVALID_LPN;\n    }\n}", "        ssd->rmap[i] = INVALID_LPN;\n    }\n    phys_audit_init(ssd);\n}")
            c = once(c, "                    tmp_lpn = get_rmap_ent(ssd, &ppa);", "                    tmp_lpn = get_rmap_ent(ssd, &ppa);\n                    phys_audit_capture(ssd, tmp_lpn, &ppa);")
            c = once(c, "            mark_block_free(ssd, &ppa);\n            \n            if (spp->enable_gc_delay)", "            mark_block_free(ssd, &ppa);\n            for (int ep = 0; ep < spp->pgs_per_blk; ep++) {\n                ppa.g.pg = ep;\n                uint64_t idx = ppa2pgidx(ssd, &ppa);\n                phys_audit_lpn[idx] = INVALID_LPN;\n                phys_audit_gen[idx] = 0;\n            }\n            \n            if (spp->enable_gc_delay)")
            c = once(c, "    mark_page_valid(ssd, new_ppa);\n\n    /* need to advance", "    mark_page_valid(ssd, new_ppa);\n    phys_audit_move(ssd, lpn, new_ppa);\n\n    /* need to advance")
            c = once(c, "        mark_page_valid(ssd, &ppa);\n\n        struct nand_cmd swr;", "        mark_page_valid(ssd, &ppa);\n        phys_audit_host(ssd, lpn, &ppa);\n\n        struct nand_cmd swr;")
            c = once(c, "    ssd_read_latency:\n", "    ssd_read_latency:\n        if (mapped_ppa(&ppa) && valid_ppa(ssd, &ppa)) {\n            phys_audit_check(ssd, lpn, &ppa, \"host-read\");\n            phys_audit_reads++;\n        }\n")
            c = once(c, "void gc_diag_report(struct ssd *ssd)\n{", "void gc_diag_report(struct ssd *ssd)\n{")
            c = once(c, "    gc_diag_donor_audit(ssd, \"after-fio\");", "    gc_diag_donor_audit(ssd, \"after-fio\");\n    femu_log(\"PHYS_ID_SUMMARY writes=%\" PRIu64 \" copies=%\" PRIu64 \" reads=%\" PRIu64 \"\\n\", phys_audit_writes, phys_audit_copies, phys_audit_reads);")

        if force_fallback:
            c = once(c, "                if (tmp_wp->vic_cnt > 1) {",
                     "                if (tmp_wp->vic_cnt > 1 && !gc_diag_force_fallback) {")
            c = once(c, "    memset(&gc_diag, 0, sizeof(gc_diag));",
                     "    memset(&gc_diag, 0, sizeof(gc_diag));\n    gc_diag_force_fallback = true;")
        c = once(c, "    struct line *curline = NULL;\n    \n    if (gc_flag)",
                 "    struct line *curline = NULL;\n    gc_diag.alloc_attempt++;\n"
                 "    if ((uint64_t)lm->free_line_cnt < gc_diag.min_free) gc_diag.min_free = lm->free_line_cnt;\n"
                 "    if ((uint64_t)lm->victim_line_cnt < gc_diag.min_victim) gc_diag.min_victim = lm->victim_line_cnt;\n    \n    if (gc_flag)")
        c = once(c, "    if (!curline) {\n        ftl_err(\"GC_EXHAUST",
                 "    if (!curline) {\n        gc_diag.alloc_empty++;\n"
                 "        if (lm->victim_line_cnt) gc_diag.alloc_empty_with_victim++;\n"
                 "        ftl_err(\"GC_EXHAUST")
        c = once(c, '    printf("GC happens?\\n");',
                 '    gc_diag.calls++;\n    printf("GC happens?\\n");')
        if force_edge:
            c = once(c, "    gc_diag.calls++;\n    printf(\"GC happens?\\n\");",
                     "    gc_diag.calls++;\n"
                     "    if (gc_diag_skip_once && lm->free_line_cnt == 2) {\n"
                     "        gc_diag_skip_once = false;\n"
                     "        femu_log(\"GC_FORCE_SKIP free=%d victim=%d\\n\", lm->free_line_cnt, lm->victim_line_cnt);\n"
                     "        return false;\n    }\n"
                     "    printf(\"GC happens?\\n\");")
            c = once(c, "                tvl = QTAILQ_FIRST(&lm->victim_list);\n                if (!tvl) return false;",
                     "                tvl = QTAILQ_FIRST(&lm->victim_list);\n"
                     "                if (gc_diag_force_fallback) {\n"
                     "                    struct line *scan = tvl;\n"
                     "                    while (scan) {\n"
                     "                        struct write_pointer *owner = ssd->line2write_pointer[scan->id];\n"
                     "                        if (owner && owner->vic_cnt == 1) { tvl = scan; break; }\n"
                     "                        scan = scan->entry.tqe_next;\n"
                     "                    }\n"
                     "                }\n"
                     "                if (!tvl) return false;\n"
                     "                if (!gc_diag_choice_logged) {\n"
                     "                    gc_diag_choice_logged = true;\n"
                     "                    femu_log(\"GC_FORCE_CHOICE id=%d vic=%d free=%d\\n\",\n"
                     "                             tvl->id, ssd->line2write_pointer[tvl->id]->vic_cnt, lm->free_line_cnt);\n"
                     "                }")
            c = once(c, "    gc_diag_force_fallback = true;",
                     "    gc_diag_force_fallback = true;\n"
                     "    gc_diag_skip_once = true;\n"
                     "    gc_diag_choice_logged = false;")
        wp_branch = ("    if (wpp && !gc_diag_gc_busy_wp[wpp->id] && wpp->vic_cnt >= gc_threshold) {"
                     if guarded else "    if (wpp && wpp->vic_cnt >= gc_threshold) {")
        trans_branch = ("    } else if (!gc_diag_gc_busy_wp[ssd->trans_wp.id] &&\n"
                        "               ssd->trans_wp.vic_cnt >= gc_threshold) {"
                        if guarded else "    } else if (ssd->trans_wp.vic_cnt >= gc_threshold) {")
        c = once(c, wp_branch, wp_branch + "\n        gc_diag.branch_wp++;")
        c = once(c, trans_branch, trans_branch + "\n        gc_diag.branch_trans++;")
        c = once(c, "    } else if (lm->free_line_cnt < 10) {",
                 "    } else if (lm->free_line_cnt < 10) {\n        gc_diag.branch_low++;")
        c = once(c, "    return false;\n}\n\n\nstatic void clear_one_write_pointer_victim_lines",
                 "    gc_diag.branch_none++;\n    return false;\n}\n\n\nstatic void clear_one_write_pointer_victim_lines")
        c = once(c, "    /* move this line to free line list */\n    QTAILQ_REMOVE",
                 "    /* move this line to free line list */\n    gc_diag_check(ssd, \"before-free\");\n    gc_diag_before_free(ssd, line);\n    QTAILQ_REMOVE")
        c = once(c, "    lm->free_line_cnt++;\n    ssd->line2write_pointer[line->id] = NULL;",
                 "    lm->free_line_cnt++;\n    gc_diag.freed++;\n    ssd->line2write_pointer[line->id] = NULL;")
        c = once(c, "    lm->free_line_cnt--;\n\n    /* wpp->curline", 
                 "    lm->free_line_cnt--;\n    gc_diag_check(ssd, \"allocate\");\n\n    /* wpp->curline")
        c = once(c, "                lm->victim_line_cnt++;\n\n                // TODO:",
                 "                lm->victim_line_cnt++;\n                gc_diag_check(ssd, \"advance-victim\");\n\n                // TODO:")
        c = once(c, "        lm->victim_line_cnt++;\n\n        init_line_write_pointer(ssd, &ssd->trans_wp, false);",
                 "        lm->victim_line_cnt++;\n        gc_diag_check(ssd, \"trans-victim\");\n\n        init_line_write_pointer(ssd, &ssd->trans_wp, false);")
        c = once(c, "                        lm->victim_line_cnt++;\n                    }",
                 "                        lm->victim_line_cnt++;\n                        gc_diag_check(ssd, \"gc-victim\");\n                    }")
        c = once(c, "    lm->free_line_cnt++;\n    gc_diag.freed++;",
                 "    lm->free_line_cnt++;\n    gc_diag_check(ssd, \"free\");\n    gc_diag.freed++;")
        c = once(c, "    case NAND_WRITE:\n        ssd->stat.write_num++;",
                 "    case NAND_WRITE:\n        ssd->stat.write_num++;\n"
                 "        if (ncmd->type == USER_IO) gc_diag.user_writes++;\n"
                 "        else gc_diag.gc_writes++;")
        if full_gtd_gc:
            c = cross_group_patch.apply(c)
            if cross_borrow:
                c = cross_group_patch.enable_borrow(c)
            patch = BASE / ("learnedftl-cross-borrow.patch" if cross_borrow else
                            "learnedftl-full-gtd-gc.patch")
            patch.write_text("".join(difflib.unified_diff(
                original[C].splitlines(keepends=True), c.splitlines(keepends=True),
                fromfile="a/hw/femu/bbssd/ld-tpftl.c",
                tofile="b/hw/femu/bbssd/ld-tpftl.c")))
        C.write_text(c)
        H.write_text(original[H] + "\nvoid gc_diag_reset(struct ssd *ssd);\nvoid gc_diag_report(struct ssd *ssd);\n")
        clean_bb = subprocess.check_output(["git","show","HEAD:hw/femu/bbssd/bb.c"],cwd=SRC,text=True)
        first = clean_bb.index("static void reset_stat(")
        last = clean_bb.index("static void bb_flip(", first)
        metric = build_paper.metric_code("learnedftl")
        metric = once(metric, "    memset(&ssd->stat, 0, sizeof(ssd->stat));",
                      "    memset(&ssd->stat, 0, sizeof(ssd->stat));\n    gc_diag_reset(ssd);")
        metric = once(metric, 'st->erase_joule);', 'st->erase_joule);\n    gc_diag_report(ssd);')
        BB.write_text(clean_bb[:first] + metric + clean_bb[last:])
        log = BASE / "bin/build-learnedftl-gc-diag.log"
        with log.open("w") as out:
            subprocess.run(["make","-C",str(SRC/"build-femu"),"-j4"],
                           stdout=out,stderr=subprocess.STDOUT,check=True)
        target = BASE / ("bin/qemu-learnedftl-cross-borrow-greedy" if cross_borrow and greedy_group else
                         "bin/qemu-learnedftl-cross-borrow" if cross_borrow else
                         "bin/qemu-learnedftl-gc-force-edge" if force_edge else
                         "bin/qemu-learnedftl-gc-force-fallback" if force_fallback else
                         "bin/qemu-learnedftl-gc-greedy-guarded" if guarded and fix else
                         "bin/qemu-learnedftl-gc-greedy-group" if greedy_group and fix else
                         "bin/qemu-learnedftl-gc-single-erase" if single_erase and fix else
                         "bin/qemu-learnedftl-full-gtd-gc" if full_gtd_gc else
                         "bin/qemu-learnedftl-gc-phys-audit" if phys_audit and fix else
                         "bin/qemu-learnedftl-gc-donor-audit-v2" if donor_audit and fix else
                         "bin/qemu-learnedftl-gc-fixed" if fix else "bin/qemu-learnedftl-gc-diag")
        staged = target.with_name(target.name + ".new")
        shutil.copy2(SRC / "build-femu/x86_64-softmmu/qemu-system-x86_64", staged)
        os.replace(staged, target)
        print(target)
    finally:
        for path, data in original.items():
            path.write_text(data)

if __name__ == "__main__":
    main()
