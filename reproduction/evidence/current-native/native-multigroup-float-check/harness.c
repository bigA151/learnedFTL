#define NATIVE_TEST_BLOCKS 256
#define NATIVE_PAPER_CACHE
#define main original_regression_main
#include "native_gc_test.c"
#undef main
#include <signal.h>
static struct ssd *device;
static void dump(int sig)
{
    struct ssd *s = device;
    fprintf(stderr, "FAIL signal=%d free=%d TP rest=%d lines=%d\n", sig,
            s->lm.free_line_cnt, s->trans_wp.curline->rest, s->trans_wp.vic_cnt);
    for (int i = 0; i < s->sp.tt_line_wps; i++) {
        struct write_pointer *w = &s->gtd_wps[i];
        if (w->curline) fprintf(stderr, "group=%d lines=%d rest=%d invalid=%"PRIu64" peer=%d hot=%d borrowed=%"PRIu64"\n",
            i,w->vic_cnt,w->curline->rest,w->invalid_pages,
            w->borrow_peer?w->borrow_peer->id:-1,w->is_borrower,w->borrowed_pages);
    }
    _Exit(128 + sig);
}
int main(void)
{
    setbuf(stdout, NULL);
    signal(SIGABRT, dump);
    device = g_new0(struct ssd, 1);
    init_test(device);
    const uint64_t region = (uint64_t)device->sp.tt_pgs * 476 / 32768;
    printf("64 interleaved jobs: region=%"PRIu64" pages, total=%"PRIu64"/%d, CMT=%d\n",
            region,region*64,device->sp.tt_pgs,device->sp.tt_cmt_size);
    for (int pass = 0; pass < 3; pass++) {
        for (uint64_t offset = 0; offset < (pass == 0 ? region * 3 / 5 : region); offset++) {
            for (int job = 0; job < 64; job++) {
                NvmeRequest req = { .slba = (job*region+offset)*device->sp.secs_per_pg,
                                   .nlb = device->sp.secs_per_pg, .stime = test_clock };
                test_clock += ssd_write(device, &req) + 1000;
            }
            if (offset % 256 == 0) {
                printf("pass=%d offset=%"PRIu64" free=%d GC=%"PRIu64"\n",pass,offset,
                        device->lm.free_line_cnt,device->stat.line_wp_gc_times);
                check_state(device);
            }
        }
        check_state(device);
    }
    puts("PASS partial pass and two full-capacity passes of 64 interleaved 4KiB sequential writers");
    return 0;
}
