/* Diagnostic: preserve 64 concurrent outstanding requests in virtual time.
 * This exercises the FTL NAND timeline, not NVMe queue teardown or host speed.
 */
#define NATIVE_TEST_BLOCKS 256
#define NATIVE_PAPER_CACHE
#ifdef NATIVE_FULL_GEOMETRY
#define NATIVE_TEST_CHANNELS 8
#define NATIVE_TEST_LUNS 8
#define NATIVE_TEST_PAGES 512
#endif
#define main original_regression_main
#include "native_gc_test.c"
#undef main

int main(void)
{
    setbuf(stdout, NULL);
    struct ssd *s = g_new0(struct ssd, 1);
    init_test(s);
    uint64_t logical = (uint64_t)s->sp.tt_pgs * 30518 / 32768;
    logical -= logical % 128;
    uint64_t region = (uint64_t)s->sp.tt_pgs * 476 / 32768;
#ifdef NATIVE_RANDOM_WORKLOAD
    uint32_t random_seed = 20261011;
    uint64_t chunks = logical / 128;
    uint64_t *order = g_new(uint64_t, chunks);
    uint8_t *seen = g_new0(uint8_t, region * 64);
    for (uint64_t i=0;i<chunks;i++) order[i]=i;
    puts("workload=randwrite seed=20261011; each 512KiB chunk once per prewarm pass, unique 4KiB pages per job");
#endif
    printf("geometry=%dch/%dlun/%dpg/%dblocks logical=%"PRIu64" region=%"PRIu64"\n",
           s->sp.nchs,s->sp.luns_per_ch,s->sp.pgs_per_blk,
           s->sp.blks_per_pl,logical,region);
    int warm_passes=6;
#ifdef NATIVE_RANDOM_PREFLIGHT
#ifndef NATIVE_RANDOM_WORKLOAD
#error NATIVE_RANDOM_PREFLIGHT requires NATIVE_RANDOM_WORKLOAD
#endif
    warm_passes=1;
#endif
    for (int pass = 0; pass < warm_passes; pass++) {
#ifdef NATIVE_RANDOM_WORKLOAD
        for (uint64_t i=chunks-1;i>0;i--) {
            random_seed ^= random_seed << 13;
            random_seed ^= random_seed >> 17;
            random_seed ^= random_seed << 5;
            uint64_t j=random_seed%(i+1),tmp=order[i];
            order[i]=order[j];order[j]=tmp;
        }
#endif
        for (uint64_t lpn = 0; lpn < logical; lpn += 128) {
            uint64_t selected=lpn;
#ifdef NATIVE_RANDOM_WORKLOAD
            selected=order[lpn/128]*128;
#endif
            NvmeRequest req = {.slba=selected*s->sp.secs_per_pg,
                .nlb=128*s->sp.secs_per_pg, .stime=test_clock};
            test_clock += ssd_write(s, &req) + 1000;
        }
        printf("prewarm=%d free=%d gc=%"PRIu64"\n",pass+1,
               s->lm.free_line_cnt,s->stat.line_wp_gc_times);
        check_state(s);
    }
#ifdef NATIVE_RANDOM_PREFLIGHT
    /* A targeted regression, not the paper's six-pass performance prewarm.
     * Full occupancy plus 1024 random 512KiB overwrites exercises pressure
     * and fragmentation before 64 concurrent random-page submissions.
     */
    for (uint64_t i=chunks-1;i>0;i--) {
        random_seed ^= random_seed << 13;
        random_seed ^= random_seed >> 17;
        random_seed ^= random_seed << 5;
        uint64_t j=random_seed%(i+1),tmp=order[i];
        order[i]=order[j];order[j]=tmp;
    }
    assert(chunks>=1024);
    for (int i=0;i<1024;i++) {
        NvmeRequest req={.slba=order[i]*128*s->sp.secs_per_pg,
            .nlb=128*s->sp.secs_per_pg,.stime=test_clock};
        test_clock+=ssd_write(s,&req)+1000;
    }
    check_state(s);
    printf("targeted_random_overwrites=1024 free=%d gc=%"PRIu64"\n",
           s->lm.free_line_cnt,s->stat.line_wp_gc_times);
#endif
    uint64_t ready[64], offset[64] = {0}, maxlat = 0;
    uint64_t writes_before=s->stat.write_num;
    uint64_t gc_before=s->stat.line_wp_gc_times;
    for (int j=0;j<64;j++) ready[j]=test_clock;
    for (int step=0;step<4096;step++) {
        int job=0;
        for (int j=1;j<64;j++) if(ready[j]<ready[job])job=j;
        test_clock=ready[job];
        uint64_t selected=offset[job];
#ifdef NATIVE_RANDOM_WORKLOAD
        do {
            random_seed ^= random_seed << 13;
            random_seed ^= random_seed >> 17;
            random_seed ^= random_seed << 5;
            selected=random_seed%region;
        } while(seen[job*region+selected]);
        seen[job*region+selected]=1;
#endif
        NvmeRequest req={.slba=(job*region+selected)*s->sp.secs_per_pg,
            .nlb=s->sp.secs_per_pg,.stime=test_clock};
        uint64_t lat=ssd_write(s,&req);
        ready[job]=test_clock+lat;
        offset[job]=(offset[job]+1)%region;
        bool first_over_30s = maxlat < 30000000000ULL && lat >= 30000000000ULL;
        if(lat>maxlat)maxlat=lat;
        if(step%64==0 || first_over_30s) {
            printf("submitted=%d max_latency_ms=%.3f free=%d gc=%"PRIu64" nand_writes=%"PRIu64" WAF=%.3f\n",
                step+1,maxlat/1e6,s->lm.free_line_cnt,
                s->stat.line_wp_gc_times-gc_before,s->stat.write_num-writes_before,
                (double)(s->stat.write_num-writes_before)/(step+1));
        }
#ifndef NATIVE_CONTINUE_AFTER_30S
        if(maxlat>=30000000000ULL) {
            check_state(s);
            puts("DIAGNOSTIC: queued FTL latency exceeds 30 seconds");
            return 0;
        }
#endif
    }
    check_state(s);
#ifdef NATIVE_EXPECT_BELOW_TIMEOUT
    assert(maxlat < 30000000000ULL);
#endif
#ifdef NATIVE_RANDOM_WORKLOAD
    g_free(seen);g_free(order);
#endif
    puts("PASS concurrent virtual-time consistency; see maximum latency above");
    return 0;
}
