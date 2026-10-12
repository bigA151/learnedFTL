/* Execute the production FTL in-process with real QEMU types and GLib queues.
 * No VM, namespace payload or performance result is simulated by this test.
 */
#define FEMU_DEBUG_FTL
#ifndef LEARNED_FTL_SOURCE
#define LEARNED_FTL_SOURCE "../learnedFTL-codex-learnedftl-chinese-comments/bbssd/ld-tpftl.c"
#endif
#include LEARNED_FTL_SOURCE

static int64_t test_clock = 1000000000;
int64_t qemu_clock_get_ns(QEMUClockType type)
{
    (void)type;
    return test_clock;
}

static void init_test(struct ssd *s)
{
    struct ssdparams *p = &s->sp;
    ssd_init_params(p);
#ifndef NATIVE_TEST_CHANNELS
#define NATIVE_TEST_CHANNELS 4
#endif
#ifndef NATIVE_TEST_LUNS
#define NATIVE_TEST_LUNS 4
#endif
#ifndef NATIVE_TEST_PAGES
#define NATIVE_TEST_PAGES 64
#endif
    p->nchs = NATIVE_TEST_CHANNELS;
    p->luns_per_ch = NATIVE_TEST_LUNS;
    p->pgs_per_blk = NATIVE_TEST_PAGES;
#ifndef NATIVE_TEST_BLOCKS
#define NATIVE_TEST_BLOCKS 32
#endif
    p->blks_per_pl = NATIVE_TEST_BLOCKS;
    p->secs_per_blk = p->secs_per_pg * p->pgs_per_blk;
    p->secs_per_pl = p->secs_per_blk * p->blks_per_pl;
    p->secs_per_lun = p->secs_per_pl;
    p->secs_per_ch = p->secs_per_lun * p->luns_per_ch;
    p->tt_secs = p->secs_per_ch * p->nchs;
    p->pgs_per_pl = p->pgs_per_blk * p->blks_per_pl;
    p->pgs_per_lun = p->pgs_per_pl;
    p->pgs_per_ch = p->pgs_per_lun * p->luns_per_ch;
    p->tt_pgs = p->pgs_per_ch * p->nchs;
    p->blks_per_lun = p->blks_per_pl;
    p->blks_per_ch = p->blks_per_lun * p->luns_per_ch;
    p->tt_blks = p->blks_per_ch * p->nchs;
    p->pls_per_ch = p->luns_per_ch; p->tt_pls = p->pls_per_ch * p->nchs;
    p->tt_luns = p->nchs * p->luns_per_ch;
    p->blks_per_line = p->tt_luns;
    p->pgs_per_line = p->blks_per_line * p->pgs_per_blk;
    p->secs_per_line = p->pgs_per_line * p->secs_per_pg;
    p->tt_lines = p->blks_per_pl;
    p->tt_trans_pgs = p->tt_pgs / p->ents_per_pg;
    p->tt_gtd_size = p->tt_trans_pgs;
    p->trans_per_line = p->pgs_per_line / p->ents_per_pg;
    p->tt_line_wps = p->tt_trans_pgs / p->trans_per_line;
#ifdef NATIVE_PAPER_CACHE
    p->tt_cmt_size = p->tt_pgs * 0.015;
#else
    p->tt_cmt_size = 64;
#endif
    p->chn_per_lun = p->nchs;
    p->chn_per_pl = p->nchs * p->luns_per_ch;
    p->chn_per_pg = p->chn_per_pl;
    p->chn_per_blk = p->chn_per_pg * p->pgs_per_blk;
    s->ssdname = g_strdup("native-gc-test");
    s->ch = g_new0(struct ssd_channel, p->nchs);
    for (int ch = 0; ch < p->nchs; ch++) ssd_init_ch(&s->ch[ch], p);
    ssd_init_maptbl(s); ssd_init_rmap(s); ssd_init_bitmap(s);
    ssd_init_lines(s); ssd_init_cmt(s); ssd_init_write_pointer(s);
    ssd_init_statistics(s); ssd_init_all_models(s);
    s->model_used = true;
}

static void check_state(struct ssd *s)
{
    int *owned = g_new0(int, s->sp.tt_lines);
    int free_count = 0, victim_count = 0;
    struct line *line;
    QTAILQ_FOREACH(line, &s->lm.free_line_list, entry) {
        assert(++owned[line->id] == 1);
        assert(line->vpc == 0 && line->ipc == 0);
        free_count++;
    }
    QTAILQ_FOREACH(line, &s->lm.victim_list, entry) victim_count++;
    assert(free_count == s->lm.free_line_cnt);
    assert(victim_count == s->lm.victim_line_cnt);
    for (int g = 0; g <= s->sp.tt_line_wps; g++) {
        struct write_pointer *wp = g == s->sp.tt_line_wps ? &s->trans_wp : &s->gtd_wps[g];
        int count = 0;
        for (struct wp_lines *l = wp->wpl->next; l; l = l->next) {
            assert(++owned[l->line->id] == 1);
            assert(s->line2write_pointer[l->line->id] == wp);
            count++;
        }
        assert(count == wp->vic_cnt);
    }
    for (int i = 0; i < s->sp.tt_lines; i++) assert(owned[i] == 1);
    uint64_t erases = 0;
    uint64_t *invalid_by_group = g_new0(uint64_t, s->sp.tt_line_wps);
    for (int i = 0; i < s->sp.tt_lines; i++) {
        int valid = 0, invalid = 0, free_pages = 0;
        struct ppa ppa = { .ppa = 0 };
        ppa.g.blk = i;
        for (int ch = 0; ch < s->sp.nchs; ch++) {
            for (int lun = 0; lun < s->sp.luns_per_ch; lun++) {
                ppa.g.ch = ch; ppa.g.lun = lun;
                int bv = 0, bi = 0;
                for (int pg = 0; pg < s->sp.pgs_per_blk; pg++) {
                    ppa.g.pg = pg;
                    int status = get_pg(s, &ppa)->status;
                    bv += status == PG_VALID; bi += status == PG_INVALID;
                    free_pages += status == PG_FREE;
                    if (status == PG_INVALID && s->lm.lines[i].type == DATA) {
                        uint64_t lpn = get_rmap_ent(s, &ppa);
                        assert(valid_lpn(s, lpn));
                        invalid_by_group[lpn / s->sp.ents_per_pg / s->sp.trans_per_line]++;
                    }
                    if (status == PG_VALID) {
                        uint64_t index = get_rmap_ent(s, &ppa);
                        if (s->lm.lines[i].type == GTD) {
                            assert(index < (uint64_t)s->sp.tt_gtd_size);
                            assert(s->gtd[index].ppa == ppa.ppa);
                        } else {
                            assert(index < (uint64_t)s->sp.tt_pgs);
                            assert(s->maptbl[index].ppa == ppa.ppa);
                        }
                    }
                }
                assert(get_blk(s, &ppa)->vpc == bv && get_blk(s, &ppa)->ipc == bi);
                erases += get_blk(s, &ppa)->erase_cnt;
                valid += bv; invalid += bi;
            }
        }
        assert(s->lm.lines[i].vpc == valid && s->lm.lines[i].ipc == invalid);
        assert(s->lm.lines[i].rest == free_pages);
    }
    assert(s->stat.erase_joule == (long double)erases * 132);
    for (int g = 0; g < s->sp.tt_line_wps; g++) {
        assert(s->gtd_wps[g].invalid_pages == invalid_by_group[g]);
    }
    g_free(invalid_by_group);
    for (int g = 0; g < s->sp.tt_line_wps; g++) {
        struct write_pointer *wp = &s->gtd_wps[g];
        if (wp->curline) assert(wp->free_pages == (uint64_t)wp->curline->rest);
        if (wp->borrow_peer) assert(wp->borrow_peer->borrow_peer == wp);
    }
    for (int lpn = 0; lpn < s->sp.tt_pgs; lpn++) {
        struct ppa ppa = s->maptbl[lpn];
        if (!mapped_ppa(&ppa)) { assert(!model_bitmap_get(s, lpn)); continue; }
        assert(valid_ppa(s, &ppa));
        assert(get_pg(s, &ppa)->status == PG_VALID);
        assert(get_rmap_ent(s, &ppa) == (uint64_t)lpn);
        struct cmt_entry *e = find_hash_entry(&s->cm.ht, lpn);
        if (e) assert(e->ppn == ppa2pgidx(s, &ppa));
        if (model_bitmap_get(s, lpn)) {
            struct ppa predicted;
            assert(model_predict(s, lpn, &predicted));
            assert(ppa2pgidx(s, &predicted) == ppa2pgidx(s, &ppa));
        }
    }
    int used = 0, nodes = 0;
    struct TPnode *tp;
    QTAILQ_FOREACH(tp, &s->cm.TPnode_list, entry) {
        assert(find_hash_tpnode(&s->cm.ht, tp->tvpn) == tp);
        int count = 0;
        struct cmt_entry *e;
        QTAILQ_FOREACH(e, &tp->cmt_entry_list, entry) {
            assert(e->lpn / s->sp.ents_per_pg == tp->tvpn);
            assert(find_hash_entry(&s->cm.ht, e->lpn) == e);
            count++;
        }
        assert(count == tp->cmt_entry_cnt);
        used += count; nodes++;
    }
    assert(used == s->cm.used_cmt_entry_cnt);
    assert(nodes == s->cm.tt_TPnodes);
    g_free(owned);
}

int main(void)
{
    setbuf(stdout, NULL);
    struct ssd *s = g_new0(struct ssd, 1);
    init_test(s);
    /* Sparse, untrained cold group supplies its tail to a hot group. */
    NvmeRequest write = { .slba = 1024 * s->sp.secs_per_pg,
                         .nlb = s->sp.secs_per_pg, .stime = test_clock };
    test_clock += ssd_write(s, &write) + 1000;
    for (int i = 0; i < 1025; i++) {
        write.slba = (i % 1024) * s->sp.secs_per_pg;
        write.stime = test_clock;
        test_clock += ssd_write(s, &write) + 1000;
    }
    assert(s->gtd_wps[0].is_borrower);
    assert(s->gtd_wps[0].borrow_peer == &s->gtd_wps[1]);
    check_state(s);
    for (int i = 0; i < 257; i++) {
        write.slba = i * s->sp.secs_per_pg;
        write.stime = test_clock;
        test_clock += ssd_write(s, &write) + 1000;
    }
    assert(!s->gtd_wps[0].borrow_peer && !s->gtd_wps[1].borrow_peer);
    assert(s->gtd_wps[0].trained && s->gtd_wps[1].trained);
    check_state(s);
    /* Prove model prediction does not consult the full mapping-table oracle. */
    bool predicted_once = false;
    for (uint64_t lpn = 0; lpn < 1024; lpn++) {
        if (!model_bitmap_get(s, lpn)) continue;
        struct ppa actual = s->maptbl[lpn], predicted;
        s->maptbl[lpn].ppa = UNMAPPED_PPA;
        assert(model_predict(s, lpn, &predicted));
        assert(predicted.ppa == actual.ppa);
        s->maptbl[lpn] = actual;
        predicted_once = true;
        break;
    }
    assert(predicted_once);
    puts("PASS cold-tail borrowing and paired GC");
    uint32_t seed = 20261011;
    for (int i = 0; i < 50000; i++) {
        seed = seed * 1664525U + 1013904223U;
        uint64_t lpn = (seed >> 8) % 8192;
        NvmeRequest req = { .slba = lpn * s->sp.secs_per_pg,
                            .nlb = s->sp.secs_per_pg, .stime = test_clock };
        uint64_t lat = ssd_write(s, &req);
        test_clock += lat + 1000;
        if (i % 31 == 0) {
            req.stime = test_clock;
            test_clock += ssd_read(s, &req) + 1000;
        }
        if (i % 257 == 0) check_state(s);
    }
    check_state(s);
    int saved_free = s->lm.free_line_cnt;
    struct statistics saved_stat = s->stat;
    struct line *saved_current = s->gtd_wps[0].curline;
    s->lm.free_line_cnt = 0;
    assert(batch_line_do_gc(s, true, &s->gtd_wps[0], NULL) == -1);
    assert(s->gtd_wps[0].curline == saved_current);
    assert(memcmp(&s->stat, &saved_stat, sizeof(saved_stat)) == 0);
    s->lm.free_line_cnt = saved_free;
    check_state(s);
    puts("PASS insufficient GC budget leaves state and NAND counters unchanged");
    assert(s->stat.line_wp_gc_times > 0);
    printf("PASS native random overwrite: 50000 writes, GC lines=%" PRIu64 "\n",
           s->stat.line_wp_gc_times);
    /* 512 KiB writes cross TP and line boundaries with a deliberately tiny CMT. */
    for (int i = 0; i < 1024; i++) {
        NvmeRequest req = { .slba = (uint64_t)(i % 64) * 128 * s->sp.secs_per_pg,
                            .nlb = 128 * s->sp.secs_per_pg, .stime = test_clock };
        test_clock += ssd_write(s, &req) + 1000;
        req.stime = test_clock;
        test_clock += ssd_read(s, &req) + 1000;
        if (i % 31 == 0) check_state(s);
    }
    check_state(s);
    puts("PASS 1024 sequential 512 KiB write/read requests across TP/line boundaries");
    /* GC includes partially filled current lines, including an empty group model. */
    assert(batch_line_do_gc(s, true, &s->gtd_wps[0], NULL) == 0);
    check_state(s);
    batch_gtd_do_gc(s, true, &s->trans_wp, s->trans_wp.vic_cnt, NULL);
    check_state(s);
    puts("PASS partial current data and translation line GC");
    /* Nearly full namespace: force the global free-line pressure path, rather
     * than relying only on a hot group's per-group line threshold. */
    struct ssd *dense = g_new0(struct ssd, 1);
    init_test(dense);
    const uint64_t dense_pages = 28 * dense->sp.pgs_per_line;
    for (uint64_t first = 0; first < dense_pages; first += 128) {
        NvmeRequest req = { .slba = first * dense->sp.secs_per_pg,
                            .nlb = 128 * dense->sp.secs_per_pg, .stime = test_clock };
        test_clock += ssd_write(dense, &req) + 1000;
    }
    check_state(dense);
    seed = 20261011;
    int lowest_free = dense->lm.free_line_cnt;
    for (int i = 0; i < 70000; i++) {
        seed = seed * 1664525U + 1013904223U;
        uint64_t lpn = (seed >> 8) % dense_pages;
        NvmeRequest req = { .slba = lpn * dense->sp.secs_per_pg,
                            .nlb = dense->sp.secs_per_pg, .stime = test_clock };
        test_clock += ssd_write(dense, &req) + 1000;
        if (dense->lm.free_line_cnt < lowest_free) lowest_free = dense->lm.free_line_cnt;
        if (i % 257 == 0) check_state(dense);
    }
    for (int pass = 0; pass < 6; pass++) {
        for (uint64_t first = 0; first < dense_pages; first += 128) {
            NvmeRequest req = { .slba = first * dense->sp.secs_per_pg,
                                .nlb = 128 * dense->sp.secs_per_pg, .stime = test_clock };
            test_clock += ssd_write(dense, &req) + 1000;
            req.stime = test_clock;
            test_clock += ssd_read(dense, &req) + 1000;
        }
        check_state(dense);
    }
    assert(lowest_free < free_line_threshold);
    assert(dense->stat.line_wp_gc_times > 0);
    printf("PASS near-capacity mixed overwrite: 28/32 groups, 70000 random writes, "
           "six sequential passes, min free=%d, GC lines=%" PRIu64 "\n",
           lowest_free, dense->stat.line_wp_gc_times);
    /* Cold tails remain usable under global pressure while the paired GC
     * reserve exists. Both groups may append to the one donor cursor. */
    struct ssd *shared = g_new0(struct ssd, 1);
    init_test(shared);
    for (int group = 1; group < 28; group++) {
        NvmeRequest req = { .slba = (uint64_t)group * 1024 * shared->sp.secs_per_pg,
                            .nlb = shared->sp.secs_per_pg, .stime = test_clock };
        test_clock += ssd_write(shared, &req) + 1000;
    }
    for (int page = 0; page < 1024; page++) {
        NvmeRequest req = { .slba = (uint64_t)page * shared->sp.secs_per_pg,
                            .nlb = shared->sp.secs_per_pg, .stime = test_clock };
        test_clock += ssd_write(shared, &req) + 1000;
    }
    assert(shared->lm.free_line_cnt == 3);
    uint64_t gc_before = shared->stat.line_wp_gc_times;
    for (int page = 0; page < 32; page++) {
        NvmeRequest req = { .slba = (uint64_t)page * shared->sp.secs_per_pg,
                            .nlb = shared->sp.secs_per_pg, .stime = test_clock };
        test_clock += ssd_write(shared, &req) + 1000;
    }
    assert(shared->gtd_wps[0].is_borrower);
    assert(shared->gtd_wps[0].borrow_peer == &shared->gtd_wps[1]);
    assert(shared->gtd_wps[0].borrowed_pages == 32);
    assert(shared->stat.line_wp_gc_times == gc_before);
    uint64_t last_vppn = ppa2vppn(shared, &shared->maptbl[31]);
    for (int page = 1025; page < 1033; page++) {
        NvmeRequest req = { .slba = (uint64_t)page * shared->sp.secs_per_pg,
                            .nlb = shared->sp.secs_per_pg, .stime = test_clock };
        test_clock += ssd_write(shared, &req) + 1000;
        assert(ppa2vppn(shared, &shared->maptbl[page]) == ++last_vppn);
    }
    check_state(shared);
    assert(batch_line_do_gc(shared, true, &shared->gtd_wps[1], NULL) == 0);
    assert(!shared->gtd_wps[0].borrow_peer && !shared->gtd_wps[1].borrow_peer);
    check_state(shared);
    puts("PASS cold-tail borrowing with three free lines, concurrent donor appends, donor-initiated paired GC");
    return 0;
}
