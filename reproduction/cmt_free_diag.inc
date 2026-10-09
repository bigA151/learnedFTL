/* Diagnostic only: bounded in-memory free-CMT snapshots, dumped after fio.
 * Stable slot IDs refer to cm->cmt_entries[], never virtual addresses.
 * Hooks run on the FTL thread; admin reset/report must run with fio stopped.
 */
#define CF_MAX_SNAPSHOTS 256
#define CF_MAX_BYTES (96ULL * 1024 * 1024)
struct cf_entry {
    uint64_t lpn, ppn, next_avail_time;
    int slot, dirty, prefetch;
};
struct cf_snapshot {
    uint64_t ns, writes, allocs, returns;
    int phase, free_count, used_count, count;
    const char *reason;
    struct cf_entry *entries;
};
static struct {
    struct cf_snapshot snapshots[CF_MAX_SNAPSHOTS];
    uint64_t start_ns, last_ns, writes, allocs, returns, bytes;
    unsigned count, dropped, transition_pairs;
    int phase, min_free, max_free;
    bool enabled, ready, first_empty;
} cf;

static void cf_capture(struct ssd *ssd, const char *reason)
{
    struct cmt_mgmt *cm = &ssd->cm;
    struct cmt_entry *e;
    struct cf_snapshot *s;
    uint8_t *seen;
    int count = 0;
    uint64_t bytes;
    if (!cf.enabled) return;
    if (cm->free_cmt_entry_cnt < 0 || cm->used_cmt_entry_cnt < 0 ||
        cm->free_cmt_entry_cnt + cm->used_cmt_entry_cnt != cm->tt_entries) {
        femu_log("CMT_FREE_INVARIANT counters reason=%s\n", reason);
        abort();
    }
    bytes = (uint64_t)cm->free_cmt_entry_cnt * sizeof(struct cf_entry);
    if (cf.count >= CF_MAX_SNAPSHOTS || cf.bytes + bytes > CF_MAX_BYTES) {
        cf.dropped++;
        return;
    }
    s = &cf.snapshots[cf.count++];
    s->reason = reason;
    s->ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - cf.start_ns;
    s->phase = cf.phase;
    s->writes = cf.writes;
    s->allocs = cf.allocs;
    s->returns = cf.returns;
    s->free_count = cm->free_cmt_entry_cnt;
    s->used_count = cm->used_cmt_entry_cnt;
    s->entries = bytes ? g_malloc0(bytes) : NULL;
    cf.bytes += bytes;
    seen = g_malloc0(cm->tt_entries);
    QTAILQ_FOREACH(e, &cm->free_cmt_entry_list, entry) {
        uintptr_t p = (uintptr_t)e, base = (uintptr_t)cm->cmt_entries;
        int slot;
        if (p < base || p >= base + (uint64_t)cm->tt_entries * sizeof(*e) ||
            (p - base) % sizeof(*e) || count >= s->free_count) goto bad;
        slot = (p - base) / sizeof(*e);
        if (seen[slot] || e->lpn != INVALID_LPN || e->ppn != UNMAPPED_PPA ||
            e->dirty != CLEAN || e->prefetch || e->next_avail_time) goto bad;
        seen[slot] = 1;
        s->entries[count++] = (struct cf_entry){e->lpn, e->ppn,
            e->next_avail_time, slot, e->dirty, e->prefetch};
    }
    if (count != s->free_count) goto bad;
    s->count = count;
    g_free(seen);
    return;
bad:
    femu_log("CMT_FREE_INVARIANT list reason=%s walked=%d expected=%d\n",
             reason, count, s->free_count);
    abort();
}

static void cf_init(struct ssd *ssd)
{
    cf.enabled = getenv("CMT_FREE_DUMP_PREFIX") != NULL;
    cf.start_ns = cf.last_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    cf.min_free = cf.max_free = ssd->cm.free_cmt_entry_cnt;
    cf.ready = true;
    cf_capture(ssd, "init");
}

static void cf_reset(struct ssd *ssd)
{
    if (!cf.ready || !cf.enabled) return;
    cf.phase++;
    cf.writes = cf.allocs = cf.returns = 0;
    cf.transition_pairs = 0;
    cf.first_empty = false;
    cf.min_free = cf.max_free = ssd->cm.free_cmt_entry_cnt;
    cf.last_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    cf_capture(ssd, "before_fio");
}

static void cf_alloc(struct ssd *ssd)
{
    if (!cf.enabled) return;
    cf.allocs++;
    cf.min_free = MIN(cf.min_free, ssd->cm.free_cmt_entry_cnt);
    if (cf.allocs == 1 || cf.allocs == 64 || cf.allocs == 1024 ||
        cf.allocs == 8192 || cf.allocs == 65536)
        cf_capture(ssd, "allocation_milestone");
    if (!ssd->cm.free_cmt_entry_cnt && !cf.first_empty) {
        cf.first_empty = true;
        cf_capture(ssd, "first_empty");
    }
    if (cf.returns > 0 && cf.returns <= 8)
        cf_capture(ssd, "allocated_after_return");
}

static void cf_return(struct ssd *ssd)
{
    if (!cf.enabled) return;
    cf.returns++;
    cf.max_free = MAX(cf.max_free, ssd->cm.free_cmt_entry_cnt);
    if (cf.returns <= 8) cf_capture(ssd, "eviction_return");
}

static void cf_write(struct ssd *ssd)
{
    uint64_t now;
    if (!cf.enabled) return;
    cf.writes++;
    if (cf.writes % 1024) return;
    now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    if (now - cf.last_ns >= 1000000000ULL) {
        cf_capture(ssd, "periodic");
        cf.last_ns = now;
    }
}

static void cf_report(struct ssd *ssd)
{
    const char *prefix = getenv("CMT_FREE_DUMP_PREFIX");
    char *path;
    FILE *snap, *rows;
    unsigned i;
    if (!cf.enabled || !prefix) return;
    cf_capture(ssd, "after_fio");
    path = g_strdup_printf("%s-snapshots.csv", prefix);
    snap = fopen(path, "w");
    g_free(path);
    path = g_strdup_printf("%s-entries.csv", prefix);
    rows = fopen(path, "w");
    g_free(path);
    if (!snap || !rows) {
        femu_log("CMT_FREE_DUMP_ERROR fopen errno=%d\n", errno);
        abort();
    }
    fprintf(snap, "snapshot_id,phase,reason,elapsed_ns,host_write_pages,allocations,returns,capacity,free_count,used_count,walk_count\n");
    fprintf(rows, "snapshot_id,queue_position,slot_id,lpn,ppn,dirty,prefetch,next_avail_time\n");
    for (i = 0; i < cf.count; i++) {
        struct cf_snapshot *s = &cf.snapshots[i];
        int j;
        fprintf(snap, "%u,%d,%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64
                ",%" PRIu64 ",%d,%d,%d,%d\n", i, s->phase, s->reason,
                s->ns, s->writes, s->allocs, s->returns, ssd->cm.tt_entries,
                s->free_count, s->used_count, s->count);
        for (j = 0; j < s->count; j++) {
            struct cf_entry *e = &s->entries[j];
            fprintf(rows, "%u,%d,%d,%" PRIu64 ",%" PRIu64 ",%d,%d,%" PRIu64 "\n",
                    i, j, e->slot, e->lpn, e->ppn, e->dirty,
                    e->prefetch, e->next_avail_time);
        }
    }
    if (fclose(snap) || fclose(rows)) {
        femu_log("CMT_FREE_DUMP_ERROR fclose\n");
        abort();
    }
    femu_log("CMT_FREE_DONE snapshots=%u dropped=%u capacity=%d min_free=%d"
             " max_free=%d end_free=%d allocations=%" PRIu64 " returns=%" PRIu64
             " host_write_pages=%" PRIu64 " memory_bytes=%" PRIu64 " prefix=%s\n",
             cf.count, cf.dropped, ssd->cm.tt_entries, cf.min_free, cf.max_free,
             ssd->cm.free_cmt_entry_cnt, cf.allocs, cf.returns, cf.writes,
             cf.bytes, prefix);
}
