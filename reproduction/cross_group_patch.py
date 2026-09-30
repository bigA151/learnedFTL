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
    source = replace_once(source,
                          '            gc_write_page_through_line_wp(ssd, group_gtd_lpns[i][pgi], &tmp_ppa, wpp);',
                          '            gc_write_page_through_line_wp(ssd, group_gtd_lpns[i][pgi], &tmp_ppa, wpp);\n            ssd->bitmaps[group_gtd_lpns[i][pgi]] = 0;')
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
    int other_borrowers = 0;
    for (int group = 0; group < cg_group_count; group++)
        if ((!hot || group != hot->id) && cg_hot_donor[group] == candidate->id)
            other_borrowers++;
    return candidate != hot && candidate->curline &&
           candidate->curline->rest >= cg_borrow_min_free &&
           !cg_group_trained[candidate->id] &&
           candidate->id < cg_donor_limit &&
           (cg_max_active_borrowers == 256 ||
            (cg_hot_donor[candidate->id] < 0 &&
             other_borrowers < cg_max_active_borrowers)) &&
           (cg_allow_multi ||
            (cg_donor_hot[candidate->id] < 0 && cg_hot_donor[candidate->id] < 0));
}

static struct write_pointer *cg_select_donor(struct ssd *ssd, struct write_pointer *hot)
{
    struct write_pointer *best = NULL;
    bool open = false, room = false, untrained = false;
    if (!cg_allow_multi && cg_donor_hot[hot->id] >= 0) {
        femu_log("CG_ACTIVE_DONOR_WRITER group=%d borrower=%d\n",
                 hot->id, cg_donor_hot[hot->id]);
        abort();
    }
    /* A leased donor may keep writing, but may not borrow onward when
     * this diagnostic policy caps connected borrower components. */
    if (cg_max_active_borrowers < 256 && cg_donor_hot[hot->id] >= 0)
        return NULL;
    if (cg_allow_multi && cg_borrow_sticky && cg_hot_donor[hot->id] >= 0) {
        struct write_pointer *pinned = &ssd->gtd_wps[cg_hot_donor[hot->id]];
        if (cg_donor_candidate(pinned, hot)) {
            cg_reason_selected++;
            return pinned;
        }
        cg_hot_donor[hot->id] = -1;
    }
    if (!cg_allow_multi && cg_hot_donor[hot->id] >= 0) {
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
    if (ssd->lm.free_line_cnt <= 32 && !cg_allow_low_free) {
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
             eligible, pages, (ssd->lm.free_line_cnt > 32 || cg_allow_low_free) &&
             ssd->lm.free_line_cnt <= 128);
}

static void cg_component_check(struct ssd *ssd, int hot, int donor,
                               bool groups[256], bool lines[256],
                               int *group_count_out, int *line_count_out)
{
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
    uint64_t group_live[256] = {0};
    uint8_t *gtd_seen = g_malloc0(ssd->sp.tt_gtd_size);
    uint64_t live_pages = 0, estimate = 0;
    uint64_t staged_gtds = 0;
    /* A shared line may contain data from several groups. Count each valid
     * physical page by its reverse-mapped LPN, then round capacity separately
     * for every target write pointer. Rounding only the aggregate can reserve
     * too few lines when one group crosses a line boundary. */
    for (int line = 0; line < ssd->sp.tt_lines; line++) {
        if (!lines[line]) continue;
        uint64_t line_live = 0;
        struct ppa ppa = { .ppa = 0 };
        ppa.g.blk = line;
        for (int ch = 0; ch < ssd->sp.nchs; ch++)
            for (int lun = 0; lun < ssd->sp.luns_per_ch; lun++)
                for (int pg = 0; pg < ssd->sp.pgs_per_blk; pg++) {
                    ppa.g.ch = ch;
                    ppa.g.lun = lun;
                    ppa.g.pg = pg;
                    ppa.g.pl = 0;
                    if (get_pg(ssd, &ppa)->status != PG_VALID) continue;
                    uint64_t lpn = get_rmap_ent(ssd, &ppa);
                    int group = (lpn / ssd->sp.ents_per_pg) /
                                ssd->sp.trans_per_line;
                    if (lpn == INVALID_LPN || group >= ssd->sp.tt_line_wps ||
                        !groups[group]) {
                        femu_log("CG_BUDGET_BAD_PAGE line=%d lpn=%" PRIu64
                                 " group=%d\n", line, lpn, group);
                        abort();
                    }
                    int gtd = lpn / ssd->sp.ents_per_pg;
                    if (!gtd_seen[gtd]) {
                        gtd_seen[gtd] = 1;
                        staged_gtds++;
                    }
                    group_live[group]++;
                    line_live++;
                }
        if (line_live != ssd->lm.lines[line].vpc) {
            femu_log("CG_BUDGET_VPC line=%d counted=%" PRIu64
                     " vpc=%d\n", line, line_live, ssd->lm.lines[line].vpc);
            abort();
        }
        live_pages += line_live;
    }
    uint64_t staging_bytes = sizeof(struct gc_gtd_buffer) +
        (uint64_t)ssd->sp.tt_gtd_size * (sizeof(uint64_t *) + sizeof(int)) +
        staged_gtds * ssd->sp.ents_per_pg * sizeof(uint64_t);
    g_free(gtd_seen);
    /* The previous 16 MiB diagnostic cap rejected a valid 8,769-GTD
     * component before any migration. At this geometry, the complete GTD
     * can require about 64 MiB of LPN staging plus index arrays. Keep a
     * fail-closed cap above that bound; this is diagnostic host memory. */
    femu_log("CG_STAGE_BUDGET gtds=%" PRIu64 " bytes=%" PRIu64
             " limit=%d\n", staged_gtds, staging_bytes, 128 << 20);
    if (staging_bytes > (128ULL << 20)) {
        femu_log("CG_STAGE_LIMIT bytes=%" PRIu64 "\n", staging_bytes);
        abort();
    }
    int reserve_lines = 0;
    for (int group = 0; group < ssd->sp.tt_line_wps; group++) {
        if (!groups[group]) continue;
        uint64_t group_estimate = group_live[group] +
            (uint64_t)line_count * ssd->sp.trans_per_line;
        estimate += group_estimate;
        int needed = (group_estimate + ssd->sp.pgs_per_line - 1) /
                     ssd->sp.pgs_per_line;
        if (needed < 1) needed = 1;
        reserve_lines += needed;
    }
    femu_log("CG_COMPONENT hot=%d donor=%d groups=%d lines=%d\n",
             hot, donor, group_count, line_count);
    for (int line = 0; line < ssd->sp.tt_lines; line++) {
        if (!lines[line]) continue;
        int members = 0;
        for (int word = 0; word < 4; word++)
            members += __builtin_popcountll(cg_line_groups[line][word]);
        if (members >= 3)
            femu_log("CG_SHARED_LINE line=%d owner=%d members=%d\n",
                     line, ssd->line2write_pointer[line] ?
                     ssd->line2write_pointer[line]->id : -1, members);
    }
    int screen_free = cg_test_budget_free >= 0 ? cg_test_budget_free :
                      ssd->lm.free_line_cnt;
    femu_log("CG_BUDGET free=%d live_pages=%" PRIu64
             " estimate_pages=%" PRIu64 " reserve_lines=%d actual_free=%d\n",
             screen_free, live_pages, estimate, reserve_lines,
             ssd->lm.free_line_cnt);
    if (screen_free < reserve_lines) {
        femu_log("CG_BUDGET_REJECT free=%d actual_free=%d need=%d\n",
                 screen_free, ssd->lm.free_line_cnt, reserve_lines);
        abort();
    }
    *group_count_out = group_count;
    *line_count_out = line_count;
}

static void cg_close_hot_line(struct ssd *ssd, struct write_pointer *hot)
{
    if (!hot->curline || hot->curline->rest != 0) abort();
    if (!gc_diag_victim_contains(ssd, hot->curline)) {
        QTAILQ_INSERT_TAIL(&ssd->lm.victim_list, hot->curline, entry);
        ssd->lm.victim_line_cnt++;
    }
    cg_hot_closed[hot->id] = 1;
}

static void cg_stage_line_valid_data(struct ssd *ssd, int line_id,
                                     struct gc_gtd_buffer *buffer)
{
    struct ppa ppa = { .ppa = 0 };
    ppa.g.blk = line_id;
    uint64_t found = 0;
    for (int ch = 0; ch < ssd->sp.nchs; ch++)
        for (int lun = 0; lun < ssd->sp.luns_per_ch; lun++)
            for (int pg = 0; pg < ssd->sp.pgs_per_blk; pg++) {
                ppa.g.ch = ch;
                ppa.g.lun = lun;
                ppa.g.pg = pg;
                ppa.g.pl = 0;
                if (get_pg(ssd, &ppa)->status != PG_VALID) continue;
                uint64_t lpn = get_rmap_ent(ssd, &ppa);
                if (!valid_lpn(ssd, lpn)) {
                    femu_log("CG_STAGE_BAD_RMAP line=%d lpn=%" PRIu64 "\n",
                             line_id, lpn);
                    abort();
                }
                phys_audit_capture(ssd, lpn, &ppa);
                gc_gtd_buffer_add(ssd, buffer, lpn);
                gc_read_page(ssd, &ppa);
                found++;
            }
    if (found != ssd->lm.lines[line_id].vpc) {
        femu_log("CG_STAGE_VPC line=%d found=%" PRIu64 " vpc=%d\n",
                 line_id, found, ssd->lm.lines[line_id].vpc);
        abort();
    }
}

static void cg_reclaim_pair(struct ssd *ssd, struct write_pointer *hot,
                            struct write_pointer *donor)
{
    bool groups[256] = {0}, lines[256] = {0};
    int group_count = 0, line_count = 0;
    if (!hot->curline || !donor->curline || !cg_hot_closed[hot->id]) abort();
    cg_component_check(ssd, hot->id, donor->id, groups, lines,
                       &group_count, &line_count);
    /* Keep source pages mapped until every destination is programmed. The
     * FTL thread is serialized, and nested GC is disabled during this plan. */
    cg_in_migration = true;
    struct gc_gtd_buffer *buffer = gc_gtd_buffer_new(ssd);
    int allocated_targets = 0;
    for (int group = 0; group < ssd->sp.tt_line_wps; group++) {
        if (!groups[group]) continue;
        struct write_pointer *wp = &ssd->gtd_wps[group];
        /* A void allocator must not leave the old source pointer looking like
         * a successful destination when no free line is available. */
        wp->curline = NULL;
        if (cg_test_fail_target_at == allocated_targets) {
            femu_log("CG_TARGET_ALLOC_INJECT group=%d allocated=%d\n",
                     group, allocated_targets);
            abort();
        }
        init_line_write_pointer(ssd, wp, false);
        if (!wp->curline || wp->curline->rest <= 0) {
            femu_log("CG_TARGET_ALLOC_FAIL group=%d allocated=%d\n",
                     group, allocated_targets);
            abort();
        }
        allocated_targets++;
    }
    for (int line_id = 0; line_id < ssd->sp.tt_lines; line_id++) {
        if (!lines[line_id]) continue;
        struct write_pointer *owner = ssd->line2write_pointer[line_id];
        if (!owner || !groups[owner->id]) {
            femu_log("CG_COMPONENT_BAD_OWNER line=%d\n", line_id);
            abort();
        }
        cg_stage_line_valid_data(ssd, line_id, buffer);
    }
    model_training_groups(ssd, buffer);
    for (int gtd = 0; gtd < ssd->sp.tt_gtd_size; gtd++)
        for (int i = 0; i < buffer->counts[gtd]; i++)
            if (phys_audit_pending[buffer->lpns[gtd][i]]) {
                femu_log("CG_PENDING_COPY gtd=%d lpn=%" PRIu64 "\n",
                         gtd, buffer->lpns[gtd][i]);
                abort();
            }
    gc_gtd_buffer_free(ssd, buffer);
    femu_log("CG_COPY_DONE groups=%d lines=%d copied_total=%" PRIu64 "\n",
             group_count, line_count, phys_audit_copies);
    /* Source erase is the final step, after mapping/model update and a
     * zero-valid-page check for every source line. */
    for (int line_id = 0; line_id < ssd->sp.tt_lines; line_id++) {
        if (!lines[line_id]) continue;
        struct line *source = &ssd->lm.lines[line_id];
        struct write_pointer *owner = ssd->line2write_pointer[line_id];
        struct ppa ppa = { .ppa = 0 };
        ppa.g.blk = line_id;
        if (source->vpc != 0) {
            femu_log("CG_SOURCE_STILL_VALID line=%d vpc=%d\n",
                     line_id, source->vpc);
            abort();
        }
        if (!gc_diag_victim_contains(ssd, source)) {
            QTAILQ_INSERT_TAIL(&ssd->lm.victim_list, source, entry);
            ssd->lm.victim_line_cnt++;
        }
        free_all_blocks(ssd, &ppa);
        clear_one_write_pointer_victim_lines(owner->wpl, source);
        owner->vic_cnt--;
        ssd->stat.gc_times++;
        ssd->stat.line_gc_times[line_id]++;
        ssd->stat.wp_victims[owner->id]++;
        ssd->stat.line_wp_gc_times++;
        mark_line_free(ssd, &ppa);
    }
    cg_in_migration = false;
    femu_log("CG_ERASE_DONE lines=%d\n", line_count);
    for (int group = 0; group < ssd->sp.tt_line_wps; group++) {
        if (!groups[group]) continue;
        cg_hot_donor[group] = -1;
        cg_donor_hot[group] = -1;
        cg_hot_closed[group] = 0; /* targets replace the closed source line */
    }
    cg_paired_gc++;
    cg_relation_check(ssd, "after-component");
    femu_log("CG_COMPONENT_GC hot=%d donor=%d groups=%d lines=%d free=%d\n",
             hot->id, donor->id, group_count, line_count,
             ssd->lm.free_line_cnt);
}
'''


def enable_borrow(source: str) -> str:
    """Experimental donor selection with paired group GC."""
    source = replace_once(source, 'static int free_line_threshold = 3;',
                          'static int free_line_threshold = 32;')
    source = replace_once(source,
        'static bool should_do_gc_v3(struct ssd *ssd, struct write_pointer *wpp) {',
        'static bool should_do_gc_v3(struct ssd *ssd, struct write_pointer *wpp) {\n    if (cg_in_migration) return false;')
    source = replace_once(source,
        '    /* update maptbl */\n    set_maptbl_ent(ssd, lpn, new_ppa);',
        '    struct ppa old_ppa = get_maptbl_ent(ssd, lpn);\n'
        '    if (mapped_ppa(&old_ppa) && valid_ppa(ssd, &old_ppa) &&\n'
        '        get_pg(ssd, &old_ppa)->status == PG_VALID) {\n'
        '        mark_page_invalid(ssd, &old_ppa);\n'
        '        set_rmap_ent(ssd, INVALID_LPN, &old_ppa);\n'
        '    }\n    /* update maptbl */\n    set_maptbl_ent(ssd, lpn, new_ppa);')
    source = replace_once(source, '    } else if (lm->free_line_cnt < 10) {',
                          '    } else if (lm->free_line_cnt < 64) {')
    source = replace_once(source, 'void gc_diag_reset(struct ssd *ssd)',
        "static uint8_t *cg_group_trained, *cg_hot_closed;\n"
        "static int *cg_hot_donor, *cg_donor_hot;\n"
        "static uint64_t (*cg_line_groups)[4], (*cg_group_lines)[4];\n"
        "static uint64_t cg_borrow_attempts, cg_borrow_success, cg_borrow_pages;\n"
        "static int cg_borrow_min_free = 8192;\n"
        "static int cg_pair_trigger = 8192;\n"
        "static int cg_donor_limit = 256;\n"
        "static int cg_max_active_borrowers = 256, cg_group_count;\n"
        "static int cg_test_budget_free = -1, cg_test_fail_target_at = -1;\n"
        "static bool cg_allow_multi, cg_allow_low_free, cg_borrow_sticky, cg_in_migration;\n"
        "static uint64_t cg_paired_gc, cg_seq_model_inits;\n"
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
        '    cg_in_migration = false;\n'
        '    cg_reason_high = cg_reason_low = cg_reason_no_open = 0;\n'
        '    cg_reason_no_room = cg_reason_trained = cg_reason_busy = cg_reason_selected = 0;\n'
        '    cg_gate_with_candidate = 0;')
    source = replace_once(source,
        '    ssd->gtd_wps = g_malloc0(sizeof(struct write_pointer) * ssd->sp.tt_line_wps);',
        '''    ssd->gtd_wps = g_malloc0(sizeof(struct write_pointer) * ssd->sp.tt_line_wps);
    cg_group_count = ssd->sp.tt_line_wps;
    const char *early_gc_text = getenv("CG_EARLY_GC_FREE");
    if (early_gc_text) {
        char *end = NULL;
        long value = strtol(early_gc_text, &end, 10);
        if (*end || value < 3 || value > ssd->sp.tt_lines / 2) abort();
        free_line_threshold = value;
        femu_log("CG_EARLY_GC_FREE threshold=%d\\n", free_line_threshold);
    }
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
    }
    cg_allow_multi = getenv("CG_BORROW_ALLOW_MULTI") != NULL;
    cg_allow_low_free = getenv("CG_ALLOW_LOW_FREE") != NULL;
    cg_borrow_sticky = getenv("CG_BORROW_STICKY") != NULL;
    const char *limit_text = getenv("CG_BORROW_DONOR_LIMIT");
    if (limit_text) {
        char *end = NULL;
        long value = strtol(limit_text, &end, 10);
        if (*end || value < 1 || value > ssd->sp.tt_line_wps) abort();
        cg_donor_limit = value;
    }
    const char *max_borrowers = getenv("CG_BORROW_MAX_ACTIVE");
    if (max_borrowers) {
        char *end = NULL;
        long value = strtol(max_borrowers, &end, 10);
        if (*end || value < 1 || value > ssd->sp.tt_line_wps) abort();
        cg_max_active_borrowers = value;
    }
    const char *budget_text = getenv("CG_TEST_BUDGET_FREE");
    if (budget_text) {
        char *end = NULL;
        long value = strtol(budget_text, &end, 10);
        if (*end || value < 0 || value > ssd->sp.tt_lines) abort();
        cg_test_budget_free = value;
    }
    const char *fail_target_text = getenv("CG_TEST_FAIL_TARGET_AT");
    if (fail_target_text) {
        char *end = NULL;
        long value = strtol(fail_target_text, &end, 10);
        if (*end || value < 0 || value >= ssd->sp.tt_line_wps) abort();
        cg_test_fail_target_at = value;
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
        '''                if (!gc_diag_victim_contains(ssd, wpp->curline)) {
                    QTAILQ_INSERT_TAIL(&lm->victim_list, wpp->curline, entry);
                    lm->victim_line_cnt++;
                }
                cg_hot_closed[wpp->id] = 0;''', 1)
    # The original increment is now inside the conditional above.
    source = replace_once(source,
        '''                lm->victim_line_cnt++;
                gc_diag_check(ssd, "advance-victim");''',
        '''                gc_diag_check(ssd, "advance-victim");''')
    source = replace_once(source,
        '    wpp->curline->rest--;\n    if (wpp->curline->rest < 0) {',
        """    if (!wpp->curline || wpp->curline->rest <= 0) {
        femu_log("CG_NO_REST group=%d line=%d rest=%d ch=%d lun=%d pg=%d\\n",
                 wpp->id, wpp->curline ? wpp->curline->id : -1,
                 wpp->curline ? wpp->curline->rest : -1,
                 wpp->ch, wpp->lun, wpp->pg);
        femu_log("CG_FATAL_SNAPSHOT attempts=%" PRIu64 " success=%" PRIu64
                 " borrowed_pages=%" PRIu64 " paired_gc=%" PRIu64
                 " high=%" PRIu64 " low=%" PRIu64 " no_open=%" PRIu64
                 " no_room=%" PRIu64 " trained=%" PRIu64
                 " busy=%" PRIu64 " selected=%" PRIu64 "\\n",
                 cg_borrow_attempts, cg_borrow_success, cg_borrow_pages,
                 cg_paired_gc, cg_reason_high, cg_reason_low,
                 cg_reason_no_open, cg_reason_no_room, cg_reason_trained,
                 cg_reason_busy, cg_reason_selected);
        cg_donor_snapshot(ssd);
        abort();
    }
    wpp->curline->rest--;
    if (wpp->curline->rest < 0) {""")
    source = replace_once(source,
        '''        // * maintain the consistency of bitmap
        // if (ssd->bitmaps[lpn] == 1) {
            // ssd->bitmaps[lpn] = 0;
        // }''',
        '        /* An overwrite invalidates any prediction for the old PPA. */\n'
        '        ssd->bitmaps[lpn] = 0;')
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
    # The upstream post-write block used the loop-ended LPN to select a GTD,
    # then modified a copy of lr_node. Only publish a verified linear run for
    # an unused model; replacement of trained segments needs a separate proof.
    source = replace_once(source,
        '    int sequence_cnt = 0;',
        '    uint64_t seq_first_vppn = 0, seq_prev_vppn = 0;\n'
        '    bool seq_linear = true;')
    source = replace_once(source,
        '        mark_page_valid(ssd, &ppa);\n        phys_audit_host(ssd, lpn, &ppa);',
        '        mark_page_valid(ssd, &ppa);\n        phys_audit_host(ssd, lpn, &ppa);\n'
        '        uint64_t seq_vppn = ppa2vppn(ssd, &ppa);\n'
        '        if (lpn == start_lpn) seq_first_vppn = seq_vppn;\n'
        '        else if (seq_vppn != seq_prev_vppn + 1) seq_linear = false;\n'
        '        seq_prev_vppn = seq_vppn;')
    begin = source.index('    // * simulate the model sequential initalization')
    end = source.index('    // ssd->stat.write_time', begin)
    source = source[:begin] + r'''    /* A fresh, untrained model can represent a verified contiguous run. */
    if (ssd->model_used && end_lpn > start_lpn && seq_linear &&
        start_lpn / spp->ents_per_pg == end_lpn / spp->ents_per_pg) {
        int gtd = start_lpn / spp->ents_per_pg;
        lr_node *model = &ssd->lr_nodes[gtd];
        bool unused = true;
        for (int j = 0; j < MAX_INTERVALS; j++)
            if (model->brks[j].valid_cnt) unused = false;
        if (unused) {
            for (uint64_t page = start_lpn; page <= end_lpn; page++) {
                struct ppa actual = get_maptbl_ent(ssd, page);
                if (!mapped_ppa(&actual) || !valid_ppa(ssd, &actual) ||
                    ppa2vppn(ssd, &actual) != seq_first_vppn + page - start_lpn) {
                    unused = false;
                    break;
                }
            }
        }
        if (unused) {
            int count = end_lpn - start_lpn + 1;
            uint64_t gtd_first = (uint64_t)gtd * spp->ents_per_pg;
            for (uint64_t page = gtd_first;
                 page < gtd_first + spp->ents_per_pg; page++)
                ssd->bitmaps[page] = 0;
            model->start_lpn = start_lpn;
            model->start_ppa = seq_first_vppn;
            for (int j = 0; j < MAX_INTERVALS; j++) {
                model->brks[j].w = 1;
                model->brks[j].b = 0;
                model->brks[j].key = count - 1;
                model->brks[j].valid_cnt = j == 0 ? count : 0;
            }
            model->u = 1;
            model->less = 0;
            model->success_ratio = 1.0;
            for (uint64_t page = start_lpn; page <= end_lpn; page++)
                ssd->bitmaps[page] = 1;
            cg_group_trained[gtd / spp->trans_per_line] = 1;
            cg_seq_model_inits++;
        }
    }

''' + source[end:]

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
    cg_relation_check(ssd, "after-fio");
    femu_log("CG_SEQ_MODEL_INIT count=%" PRIu64 "\\n", cg_seq_model_inits);''')
    return source
