"""Isolated full-GTD GC collection transform for the LearnedFTL prototype.

This keeps the old allocation policy while making data GC capable of collecting
pages from more than one GTD entry group.  Borrowing is enabled separately.
"""


def replace_once(source: str, old: str, new: str) -> str:
    count = source.count(old)
    if count != 1:
        raise RuntimeError(f"expected one occurrence ({count}): {old[:100]!r}")
    return source.replace(old, new, 1)


BUFFER = r'''
struct gc_gtd_buffer {
    uint64_t **lpns;
    int *counts;
};

static struct gc_gtd_buffer *gc_gtd_buffer_new(struct ssd *ssd)
{
    struct gc_gtd_buffer *b = g_malloc0(sizeof(*b));
    b->lpns = g_malloc0(sizeof(*b->lpns) * ssd->sp.tt_gtd_size);
    b->counts = g_malloc0(sizeof(*b->counts) * ssd->sp.tt_gtd_size);
    return b;
}

static void gc_gtd_buffer_add(struct ssd *ssd, struct gc_gtd_buffer *b,
                              uint64_t lpn)
{
    int gtd = lpn / ssd->sp.ents_per_pg;
    if (gtd < 0 || gtd >= ssd->sp.tt_gtd_size ||
        b->counts[gtd] >= ssd->sp.ents_per_pg) {
        femu_log("GC_GTD_OVERFLOW gtd=%d lpn=%" PRIu64 " count=%d\n",
                 gtd, lpn, gtd >= 0 && gtd < ssd->sp.tt_gtd_size ? b->counts[gtd] : -1);
        abort();
    }
    if (!b->lpns[gtd])
        b->lpns[gtd] = g_malloc(sizeof(uint64_t) * ssd->sp.ents_per_pg);
    b->lpns[gtd][b->counts[gtd]++] = lpn;
}

static void gc_gtd_buffer_free(struct ssd *ssd, struct gc_gtd_buffer *b)
{
    for (int i = 0; i < ssd->sp.tt_gtd_size; i++) g_free(b->lpns[i]);
    g_free(b->lpns);
    g_free(b->counts);
    g_free(b);
}
'''

TRAIN = r'''
static void model_training_groups(struct ssd *ssd, struct gc_gtd_buffer *b)
{
    const int n = ssd->sp.trans_per_line;
    const int entries = ssd->sp.ents_per_pg;
    uint64_t (*group_lpns)[512] = g_malloc0(sizeof(uint64_t) * n * entries);
    int *group_counts = g_malloc0(sizeof(int) * n);
    for (int start = 0; start < ssd->sp.tt_gtd_size; start += n) {
        bool present = false;
        memset(group_counts, 0, sizeof(int) * n);
        for (int i = 0; i < n && start + i < ssd->sp.tt_gtd_size; i++) {
            int count = b->counts[start + i];
            if (!count) continue;
            memcpy(group_lpns[i], b->lpns[start + i], count * sizeof(uint64_t));
            group_counts[i] = count;
            present = true;
        }
        if (present) {
            struct write_pointer *target = &ssd->gtd_wps[start / n];
            if (!target->curline || target->curline->rest <= 0) {
                femu_log("GC_GROUP_NO_TARGET group=%d rest=%d\n", start / n,
                         target->curline ? target->curline->rest : -1);
                abort();
            }
            model_training(ssd, target, group_lpns, group_counts, start);
        }
    }
    g_free(group_counts);
    g_free(group_lpns);
}
'''


def apply(source: str) -> str:
    source = replace_once(source, 'static void gc_read_all_valid_data(struct ssd *ssd, struct ppa *tppa, uint64_t group_gtd_lpns[][512], int *group_gtd_index, int *start_gtd) {',
                          BUFFER + '\nstatic void gc_read_all_valid_data(struct ssd *ssd, struct ppa *tppa, struct gc_gtd_buffer *buffer) {')
    source = replace_once(source, '''                    int gtd_index = tmp_lpn/spp->ents_per_pg;
                    *start_gtd = gtd_index - (gtd_index % parallel);      // ! FIXME: 
                    int gtd_index_loc = gtd_index % spp->trans_per_line;    // gtd_index%64''',
                          '''                    int gtd_index = tmp_lpn/spp->ents_per_pg;
                    (void)gtd_index;''')
    source = replace_once(source,
                          '                    group_gtd_lpns[gtd_index_loc][group_gtd_index[gtd_index_loc]++] = tmp_lpn;',
                          '                    gc_gtd_buffer_add(ssd, buffer, tmp_lpn);')
    source = replace_once(source, 'static int batch_line_do_gc(struct ssd* ssd, bool force, struct write_pointer *wpp, struct line *delete_line) {',
                          TRAIN + '\nstatic int batch_line_do_gc(struct ssd* ssd, bool force, struct write_pointer *wpp, struct line *delete_line) {')
    old = '''    uint64_t group_gtd_lpns[parallel][trans_ent];
    int group_gtd_index[parallel];
    memset(group_gtd_index, 0, sizeof(group_gtd_index));
    int start_gtd = 0;'''
    if source.count(old) != 2:
        raise RuntimeError('expected batch and line GC scratch arrays')
    source = source.replace(old, '    struct gc_gtd_buffer *buffer = gc_gtd_buffer_new(ssd);', 2)
    old = 'gc_read_all_valid_data(ssd, &ppa, group_gtd_lpns, group_gtd_index, &start_gtd);'
    if source.count(old) != 2:
        raise RuntimeError('expected batch and line collection calls')
    source = source.replace(old, 'gc_read_all_valid_data(ssd, &ppa, buffer);', 2)
    old = 'model_training(ssd, wpp, group_gtd_lpns, group_gtd_index, start_gtd);'
    if source.count(old) != 2:
        raise RuntimeError('expected batch and line training calls')
    source = source.replace(old, 'model_training_groups(ssd, buffer);\n    gc_gtd_buffer_free(ssd, buffer);', 2)
    # gc_read_all_valid_data has already cleared and timed each NAND block.
    source = replace_once(source, '    free_all_blocks(ssd, &ppa);\n\n    struct wp_lines *wpl = wpp->wpl;',
                          '    struct wp_lines *wpl = wpp->wpl;')
    return source

BORROW = r'''
/* Selection and audit use the same candidate predicate. The free-line gate is
 * counted separately so a rejected attempt never means there was no donor. */
static void cg_relation_link(struct ssd *ssd, int group, int line)
{
    if (group < 0 || group >= ssd->sp.tt_line_wps ||
        line < 0 || line >= ssd->sp.tt_lines ||
        ssd->sp.tt_line_wps > 256 || ssd->sp.tt_lines > 256)
        abort();
    cg_line_groups[line][group / 64] |= 1ULL << (group % 64);
    cg_group_lines[group][line / 64] |= 1ULL << (line % 64);
}

static void cg_relation_unlink_line(struct ssd *ssd, int line)
{
    if (line < 0 || line >= ssd->sp.tt_lines) abort();
    for (int group = 0; group < ssd->sp.tt_line_wps; group++)
        cg_group_lines[group][line / 64] &= ~(1ULL << (line % 64));
    memset(cg_line_groups[line], 0, sizeof(cg_line_groups[0]));
}

static void cg_relation_check(struct ssd *ssd, const char *phase)
{
    uint64_t links = 0, shared = 0;
    for (int line = 0; line < ssd->sp.tt_lines; line++) {
        int members = 0;
        for (int group = 0; group < ssd->sp.tt_line_wps; group++) {
            bool forward = (cg_line_groups[line][group / 64] >> (group % 64)) & 1;
            bool reverse = (cg_group_lines[group][line / 64] >> (line % 64)) & 1;
            if (forward != reverse) {
                femu_log("CG_RELATION_MISMATCH phase=%s line=%d group=%d\n",
                         phase, line, group);
                abort();
            }
            members += forward;
        }
        links += members;
        shared += members > 1;
    }
    femu_log("CG_RELATION phase=%s links=%" PRIu64 " shared_lines=%" PRIu64
             "\n", phase, links, shared);
}

static bool cg_donor_candidate(struct write_pointer *candidate,
                               struct write_pointer *hot)
{
    return candidate != hot && candidate->curline &&
           candidate->curline->rest >= cg_borrow_min_free &&
           !cg_group_trained[candidate->id] &&
           cg_donor_hot[candidate->id] < 0 && cg_hot_donor[candidate->id] < 0;
}

static struct write_pointer *cg_select_donor(struct ssd *ssd, struct write_pointer *hot)
{
    struct write_pointer *best = NULL;
    bool open = false, room = false, untrained = false;
    if (cg_donor_hot[hot->id] >= 0) {
        femu_log("CG_ACTIVE_DONOR_WRITER group=%d borrower=%d\n",
                 hot->id, cg_donor_hot[hot->id]);
        abort();
    }
    if (cg_hot_donor[hot->id] >= 0) {
        struct write_pointer *pinned = &ssd->gtd_wps[cg_hot_donor[hot->id]];
        if (!pinned->curline || pinned->curline->rest <= 0 ||
            cg_donor_hot[pinned->id] != hot->id) abort();
        cg_reason_selected++;
        return pinned;
    }
    for (int i = 0; i < ssd->sp.tt_line_wps; i++) {
        struct write_pointer *candidate = &ssd->gtd_wps[i];
        if (candidate == hot || !candidate->curline) continue;
        open = true;
        if (candidate->curline->rest < cg_borrow_min_free) continue;
        room = true;
        if (cg_group_trained[i]) continue;
        untrained = true;
        if (cg_donor_candidate(candidate, hot) &&
            (!best || candidate->curline->rest > best->curline->rest))
            best = candidate;
    }
    if (ssd->lm.free_line_cnt > 128) {
        cg_reason_high++;
        if (best) cg_gate_with_candidate++;
        return NULL;
    }
    if (ssd->lm.free_line_cnt <= 32) {
        cg_reason_low++;
        if (best) cg_gate_with_candidate++;
        return NULL;
    }
    if (!open) cg_reason_no_open++;
    else if (!room) cg_reason_no_room++;
    else if (!untrained) cg_reason_trained++;
    else if (!best) cg_reason_busy++;
    else cg_reason_selected++;
    return best;
}

static void cg_donor_snapshot(struct ssd *ssd)
{
    uint64_t open = 0, room = 0, eligible = 0, pages = 0;
    for (int i = 0; i < ssd->sp.tt_line_wps; i++) {
        struct write_pointer *candidate = &ssd->gtd_wps[i];
        if (!candidate->curline) continue;
        open++;
        if (candidate->curline->rest < cg_borrow_min_free) continue;
        room++;
        if (!cg_donor_candidate(candidate, NULL)) continue;
        eligible++;
        pages += candidate->curline->rest;
    }
    femu_log("CG_DONOR_POLICY free=%d open=%" PRIu64 " room=%" PRIu64
             " eligible=%" PRIu64 " eligible_pages=%" PRIu64
             " gate_open=%d\n", ssd->lm.free_line_cnt, open, room,
             eligible, pages, ssd->lm.free_line_cnt > 32 &&
             ssd->lm.free_line_cnt <= 128);
}

static void cg_component_check(struct ssd *ssd, int hot, int donor)
{
    bool groups[256] = {0}, lines[256] = {0};
    groups[hot] = groups[donor] = true;
    bool changed;
    do {
        changed = false;
        for (int group = 0; group < ssd->sp.tt_line_wps; group++) {
            if (!groups[group]) continue;
            for (int line = 0; line < ssd->sp.tt_lines; line++) {
                if (!(cg_group_lines[group][line / 64] & (1ULL << (line % 64))) ||
                    lines[line]) continue;
                lines[line] = true;
                changed = true;
            }
        }
        for (int line = 0; line < ssd->sp.tt_lines; line++) {
            if (!lines[line]) continue;
            for (int group = 0; group < ssd->sp.tt_line_wps; group++) {
                if (!(cg_line_groups[line][group / 64] & (1ULL << (group % 64))) ||
                    groups[group]) continue;
                groups[group] = true;
                changed = true;
            }
        }
    } while (changed);
    int group_count = 0, line_count = 0;
    for (int group = 0; group < ssd->sp.tt_line_wps; group++)
        group_count += groups[group];
    for (int line = 0; line < ssd->sp.tt_lines; line++)
        line_count += lines[line];
    femu_log("CG_COMPONENT hot=%d donor=%d groups=%d lines=%d\n",
             hot, donor, group_count, line_count);
    /* The current pair collector handles only two groups. Stop before it
     * mutates queues if the true connected component is wider. */
    if (group_count > 2) {
        femu_log("CG_COMPONENT_UNSUPPORTED hot=%d donor=%d groups=%d lines=%d\n",
                 hot, donor, group_count, line_count);
        abort();
    }
}

static void cg_close_hot_line(struct ssd *ssd, struct write_pointer *hot)
{
    if (cg_hot_closed[hot->id]) return;
    if (!hot->curline || hot->curline->rest != 0) abort();
    QTAILQ_INSERT_TAIL(&ssd->lm.victim_list, hot->curline, entry);
    ssd->lm.victim_line_cnt++;
    cg_hot_closed[hot->id] = 1;
}

static void cg_reclaim_pair(struct ssd *ssd, struct write_pointer *hot,
                            struct write_pointer *donor)
{
    struct line *hot_line = hot->curline;
    struct line *donor_line = donor->curline;
    cg_component_check(ssd, hot->id, donor->id);
    if (ssd->lm.free_line_cnt < 2 || !hot_line || !donor_line ||
        !cg_hot_closed[hot->id]) {
        femu_log("CG_PAIR_NO_SPACE free=%d hot=%d donor=%d\n",
                 ssd->lm.free_line_cnt, hot->id, donor->id);
        abort();
    }
    QTAILQ_INSERT_TAIL(&ssd->lm.victim_list, donor_line, entry);
    ssd->lm.victim_line_cnt++;
    init_line_write_pointer(ssd, hot, false);
    init_line_write_pointer(ssd, donor, false);
    if (!hot->curline || !donor->curline) abort();
    /* Collect all owned lines for both GTD groups.  The shared donor line
     * can contain LPNs from the hot group and any additional borrowers;
     * model_training_groups routes each LPN by its complete GTD index. */
    batch_line_do_gc(ssd, true, hot, NULL);
    batch_line_do_gc(ssd, true, donor, NULL);
    cg_hot_donor[hot->id] = -1;
    cg_donor_hot[donor->id] = -1;
    cg_paired_gc++;
    cg_relation_check(ssd, "after-pair");
    femu_log("CG_PAIR_GC hot=%d donor=%d free=%d\n",
             hot->id, donor->id, ssd->lm.free_line_cnt);
}
'''


def enable_borrow(source: str) -> str:
    """Experimental donor selection with paired group GC."""
    source = replace_once(source, 'static int free_line_threshold = 3;',
                          'static int free_line_threshold = 32;')
    source = replace_once(source, '    } else if (lm->free_line_cnt < 10) {',
                          '    } else if (lm->free_line_cnt < 64) {')
    source = replace_once(source, 'void gc_diag_reset(struct ssd *ssd)',
        "static uint8_t *cg_group_trained, *cg_hot_closed;\n"
        "static int *cg_hot_donor, *cg_donor_hot;\n"
        "static uint64_t (*cg_line_groups)[4], (*cg_group_lines)[4];\n"
        "static uint64_t cg_borrow_attempts, cg_borrow_success, cg_borrow_pages;\n"
        "static int cg_borrow_min_free = 8192;\n"
        "static int cg_pair_trigger = 8192;\n"
        "static uint64_t cg_paired_gc;\n"
        "static uint64_t cg_reason_high, cg_reason_low, cg_reason_no_open;\n"
        "static uint64_t cg_reason_no_room, cg_reason_trained, cg_reason_busy, cg_reason_selected;\n"
        "static uint64_t cg_gate_with_candidate;\n"
        "static uint32_t *cg_line_borrow_count;\n"
        "static void cg_donor_snapshot(struct ssd *ssd);\n"
        "static void cg_relation_link(struct ssd *ssd, int group, int line);\n"
        "static void cg_relation_unlink_line(struct ssd *ssd, int line);\n"
        "static void cg_relation_check(struct ssd *ssd, const char *phase);\n"
        "void gc_diag_reset(struct ssd *ssd)")
    source = replace_once(source,
        '    memset(&gc_diag, 0, sizeof(gc_diag));',
        '    memset(&gc_diag, 0, sizeof(gc_diag));\n'
        '    cg_borrow_attempts = cg_borrow_success = cg_borrow_pages = 0;\n'
        '    cg_paired_gc = 0;\n'
        '    cg_reason_high = cg_reason_low = cg_reason_no_open = 0;\n'
        '    cg_reason_no_room = cg_reason_trained = cg_reason_busy = cg_reason_selected = 0;\n'
        '    cg_gate_with_candidate = 0;')
    source = replace_once(source,
        '    ssd->gtd_wps = g_malloc0(sizeof(struct write_pointer) * ssd->sp.tt_line_wps);',
        '''    ssd->gtd_wps = g_malloc0(sizeof(struct write_pointer) * ssd->sp.tt_line_wps);
    cg_group_trained = g_malloc0(ssd->sp.tt_line_wps);
    cg_hot_donor = g_malloc(sizeof(int) * ssd->sp.tt_line_wps);
    cg_donor_hot = g_malloc(sizeof(int) * ssd->sp.tt_line_wps);
    for (int i = 0; i < ssd->sp.tt_line_wps; i++)
        cg_hot_donor[i] = cg_donor_hot[i] = -1;
    cg_hot_closed = g_malloc0(ssd->sp.tt_line_wps);
    cg_line_groups = g_malloc0(sizeof(*cg_line_groups) * ssd->sp.tt_lines);
    cg_group_lines = g_malloc0(sizeof(*cg_group_lines) * ssd->sp.tt_line_wps);
    cg_line_borrow_count = g_malloc0(sizeof(*cg_line_borrow_count) * ssd->sp.tt_lines);
    const char *trigger_text = getenv("CG_BORROW_PAIR_TRIGGER");
    if (trigger_text) {
        char *end = NULL;
        long value = strtol(trigger_text, &end, 10);
        if (*end || value < 1 || value > 32768) {
            femu_log("CG_BAD_PAIR_TRIGGER value=%s\\n", trigger_text);
            abort();
        }
        cg_pair_trigger = value;
    }''')
    source = replace_once(source,
        '    ssd->line2write_pointer[wpp->curline->id] = wpp;',
        '''    ssd->line2write_pointer[wpp->curline->id] = wpp;
    memset(cg_line_groups[wpp->curline->id], 0, sizeof(cg_line_groups[0]));
    cg_line_borrow_count[wpp->curline->id] = 0;
    if (wpp != &ssd->trans_wp) {
        cg_relation_link(ssd, wpp->id, wpp->curline->id);
        cg_hot_closed[wpp->id] = 0;
    }''')
    source = replace_once(source,
        '    ssd->line2write_pointer[line->id] = NULL;',
        '''    ssd->line2write_pointer[line->id] = NULL;
    cg_relation_unlink_line(ssd, line->id);''')
    source = source.replace(
        '                QTAILQ_INSERT_TAIL(&lm->victim_list, wpp->curline, entry);',
        '''                if (!cg_hot_closed[wpp->id]) {
                    QTAILQ_INSERT_TAIL(&lm->victim_list, wpp->curline, entry);
                    lm->victim_line_cnt++;
                } else {
                    cg_hot_closed[wpp->id] = 0;
                }''', 1)
    # The original increment is now inside the conditional above.
    source = replace_once(source,
        '''                lm->victim_line_cnt++;
                gc_diag_check(ssd, "advance-victim");''',
        '''                gc_diag_check(ssd, "advance-victim");''')
    source = replace_once(source,
        '        if (group_gtd_index[i] > TRAIN_THRESHOLD) {',
        '''        if (group_gtd_index[i] > TRAIN_THRESHOLD) {
            cg_group_trained[(start_gtd + i) / ssd->sp.trans_per_line] = 1;''')
    source = replace_once(source,
        '                ftl_assert(pg_iter->status != PG_FREE);',
        '                if (pg_iter->status == PG_FREE) continue;')
    source = replace_once(source,
        'static uint64_t ssd_write(struct ssd *ssd, NvmeRequest *req)',
        BORROW + '\nstatic uint64_t ssd_write(struct ssd *ssd, NvmeRequest *req)')
    source = replace_once(source,
        '''        struct write_pointer *lwp= &ssd->gtd_wps[wp_index];
        if (!lwp->curline) {
            init_line_write_pointer(ssd, lwp, true);
        } else {
            advance_line_write_pointer(ssd, lwp);
        }

        ppa = get_new_line_page(ssd, lwp);''',
        '''        struct write_pointer *lwp= &ssd->gtd_wps[wp_index];
        struct write_pointer *alloc_wp = lwp;
        if (lwp->curline && lwp->curline->rest == 0) {
            cg_borrow_attempts++;
            struct write_pointer *donor = cg_select_donor(ssd, lwp);
            if (donor) {
                if (cg_hot_donor[lwp->id] < 0) {
                    cg_hot_donor[lwp->id] = donor->id;
                    cg_donor_hot[donor->id] = lwp->id;
                }
                cg_close_hot_line(ssd, lwp);
                alloc_wp = donor;
            }
        }
        if (!alloc_wp->curline) {
            init_line_write_pointer(ssd, alloc_wp, true);
        } else {
            advance_line_write_pointer(ssd, alloc_wp);
        }
        if (alloc_wp != lwp) {
            cg_borrow_success++;
            cg_borrow_pages++;
            cg_line_borrow_count[alloc_wp->curline->id]++;
            cg_relation_link(ssd, lwp->id, alloc_wp->curline->id);
        }
        ppa = get_new_line_page(ssd, alloc_wp);''')
    source = replace_once(source,
        '        maxlat = (curlat > maxlat) ? curlat : maxlat;\n        // clock_gettime(CLOCK_MONOTONIC, &time2);',
        '        maxlat = (curlat > maxlat) ? curlat : maxlat;\n        if (alloc_wp != lwp && cg_line_borrow_count[ppa.g.blk] >= cg_pair_trigger)\n            cg_reclaim_pair(ssd, lwp, alloc_wp);\n        // clock_gettime(CLOCK_MONOTONIC, &time2);')
    # The physical victim is available immediately after its valid LPNs have
    # been staged.  Release it before allocating targets for mixed groups.
    source = replace_once(source,
        '''    model_training_groups(ssd, buffer);
    gc_gtd_buffer_free(ssd, buffer);

    struct wp_lines *wpl = wpp->wpl;
    // TODO: evict this line out of the wp_lines of wpp;
    clear_one_write_pointer_victim_lines(wpl, victim_line);

    /* update line status */
    mark_line_free(ssd, &ppa);''',
        '''    struct wp_lines *wpl = wpp->wpl;
    clear_one_write_pointer_victim_lines(wpl, victim_line);
    mark_line_free(ssd, &ppa);
    model_training_groups(ssd, buffer);
    gc_gtd_buffer_free(ssd, buffer);''')
    source = replace_once(source,
        '''            if (!target->curline || target->curline->rest <= 0) {
                femu_log("GC_GROUP_NO_TARGET group=%d rest=%d\\n", start / n,
                         target->curline ? target->curline->rest : -1);
                abort();
            }
            model_training''',
        '''            if (!target->curline || target->curline->rest <= 0)
                init_line_write_pointer(ssd, target, false);
            if (!target->curline || target->curline->rest <= 0) {
                femu_log("GC_GROUP_NO_TARGET group=%d rest=%d\\n", start / n,
                         target->curline ? target->curline->rest : -1);
                abort();
            }
            model_training''')
    # Summarize allocation state only on a fatal migration failure.
    begin = source.index('static void model_training_groups(')
    fail = source.index('GC_GROUP_NO_TARGET', begin)
    abort_at = source.index('                abort();', fail)
    diag = ("                int max_invalid = 0;\n"
            "                struct line *candidate;\n"
            "                QTAILQ_FOREACH(candidate, &ssd->lm.victim_list, entry)\n"
            "                    if (candidate->ipc > max_invalid) max_invalid = candidate->ipc;\n"
            "                femu_log(\"CG_FAILURE free=%d victim=%d max_invalid=%d borrowed=%\" PRIu64 \"\\n\",\n"
            "                         ssd->lm.free_line_cnt, ssd->lm.victim_line_cnt,\n"
            "                         max_invalid, cg_borrow_success);\n")
    source = source[:abort_at] + diag + source[abort_at:]
    source = replace_once(source,
        '    gc_diag_donor_audit(ssd, "after-fio");',
        '''    gc_diag_donor_audit(ssd, "after-fio");
    femu_log("CG_BORROW attempts=%" PRIu64 " success=%" PRIu64
             " pages=%" PRIu64 " paired_gc=%" PRIu64 " min_donor_free=%d pair_trigger=%d\\n",
             cg_borrow_attempts, cg_borrow_success,
             cg_borrow_pages, cg_paired_gc, cg_borrow_min_free, cg_pair_trigger);
    femu_log("CG_BORROW_REASONS attempts=%" PRIu64 " high=%" PRIu64
             " low=%" PRIu64 " no_open=%" PRIu64 " no_room=%" PRIu64
             " trained=%" PRIu64 " busy=%" PRIu64 " selected=%" PRIu64
             " gated_with_candidate=%" PRIu64 "\\n",
             cg_borrow_attempts, cg_reason_high, cg_reason_low,
             cg_reason_no_open, cg_reason_no_room, cg_reason_trained,
             cg_reason_busy, cg_reason_selected, cg_gate_with_candidate);
    cg_donor_snapshot(ssd);
    cg_relation_check(ssd, "after-fio");''')
    return source
