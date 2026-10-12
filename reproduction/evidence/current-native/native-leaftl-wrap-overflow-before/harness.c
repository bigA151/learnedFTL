#define NATIVE_WRAP_MODEL 1
#define NATIVE_MATCH_RESERVE 1
#include "variants-diagnostic/leaftl/femu/bbssd/leaftl.c"
int64_t qemu_clock_get_ns(QEMUClockType type) { return 1000000000; }
static void expect_mapping(learned_mgmt *lm, uint64_t lpn, uint64_t ppn)
{
    simple_segment cache = {0};
    uint64_t predicted = get_ppn(lm, lpn, &cache);
    assert(predicted == ppn);
    assert(isfinite(cache.k));
    assert(lm->bitmap[lpn]);
}
int main(void)
{
    setbuf(stdout, NULL);
    struct ssd s = {0};
    ssd_init_params(&s.sp);
    learned_mgmt *lm = learned_segments_init(&s);
#ifdef NATIVE_WRAP_MODEL
    uint64_t wrap_lpns[] = {0,1}, wrap_ppns[] = {1023,0};
    train_mapping_batch(lm,wrap_lpns,wrap_ppns,2);
    expect_mapping(lm,0,1023); expect_mapping(lm,1,0);
    simple_segment wrap_cache={0};
    get_ppn(lm,0,&wrap_cache);
    assert(fabs(wrap_cache.k + 1023.0) < 0.001);
    puts("PASS LeaFTL physical-line wrap has finite, bounded predictions");
#endif
    uint64_t lpns[] = {254,255,256,257}, ppns[] = {100,101,102,103};
    train_mapping_batch(lm, lpns, ppns, 4);
    for (int i=0;i<4;i++) expect_mapping(lm,lpns[i],ppns[i]);
    uint64_t single_lpn=509, single_ppn=300;
    train_mapping_batch(lm,&single_lpn,&single_ppn,1);
    expect_mapping(lm,509,300);
    uint64_t sparse_lpns[]={10,12,14}, sparse_ppns[]={200,202,204};
    train_mapping_batch(lm,sparse_lpns,sparse_ppns,3);
    for(int i=0;i<3;i++) expect_mapping(lm,sparse_lpns[i],sparse_ppns[i]);
    uint8_t live[256]={0};
    get_valid_lpns(lm->all_segs->levels->level_seg[0],live,0,lm->sp,lm);
    assert(live[10] && live[12] && live[14] && live[254] && live[255]);
    uint64_t replace=254, replacement=400;
    train_mapping_batch(lm,&replace,&replacement,1);
    expect_mapping(lm,254,400); expect_mapping(lm,255,101);
    compact(&s,lm);
    expect_mapping(lm,254,400); expect_mapping(lm,255,101);
    expect_mapping(lm,10,200); expect_mapping(lm,14,204);
    struct ssd *dev = g_new0(struct ssd, 1);
    ssd_init_params(&dev->sp);
#ifdef NATIVE_MATCH_RESERVE
    /* Full geometry has 12 reserve lines and a 1/16-line host buffer.
     * The 32-line fixture otherwise retains only one reserve line while
     * the unchanged 2048-page buffer consumes two lines. Keep enough
     * reserve for a full buffer and DATA/TP relocation in this test only. */
    dev->sp.gc_thres_lines_high =
        (dev->sp.buffer_size + dev->sp.pgs_per_line - 1) /
            dev->sp.pgs_per_line + 2;
    printf("Native-only GC reserve: %d lines\n",dev->sp.gc_thres_lines_high);
#endif
    dev->ssdname = "native-leaftl";
    dev->ch = g_new0(struct ssd_channel, dev->sp.nchs);
    for (int i=0;i<dev->sp.nchs;i++) ssd_init_ch(&dev->ch[i],&dev->sp);
    ssd_init_maptbl(dev); ssd_init_rmap(dev); ssd_init_gtd(dev);
    ssd_init_cmt(dev); ssd_init_lines(dev); ssd_init_write_pointer(dev);
    ssd_init_trans_write_pointer(dev); ssd_init_statistics(dev);
    dev->lem = learned_segments_init(dev);
    dev->buffer_lpns = g_new0(uint64_t,dev->sp.buffer_size);
    const int pages=24576;
    for (int pass=0;pass<6;pass++) {
        for(int lpn=0;lpn<pages;lpn+=128) {
            NvmeRequest req={.slba=(uint64_t)lpn*dev->sp.secs_per_pg,
                             .nlb=128*dev->sp.secs_per_pg,.stime=1000000000};
            ssd_write(dev,&req);
        }
        assert(dev->buffer_pointer==0);
        for(int lpn=0;lpn<pages;lpn++) {
            struct ppa current=dev->maptbl[lpn];
            assert(mapped_ppa(&current) && valid_ppa(dev,&current));
            assert(get_pg(dev,&current)->status==PG_VALID);
            assert(get_rmap_ent(dev,&current)==(uint64_t)lpn);
        }
        for(int line=0;line<dev->sp.tt_lines;line++) {
            struct ppa p={.ppa=0};p.g.blk=line;
            for(int ch=0;ch<dev->sp.nchs;ch++) for(int lun=0;lun<dev->sp.luns_per_ch;lun++)
                for(int page=0;page<dev->sp.pgs_per_blk;page++) {
                    p.g.ch=ch;p.g.lun=lun;p.g.pg=page;
                    if(get_pg(dev,&p)->status!=PG_VALID) continue;
                    uint64_t index=get_rmap_ent(dev,&p);
                    struct ppa expected=dev->lm.lines[line].type==DATA?
                        dev->maptbl[index]:dev->gtd[index];
                    assert(expected.ppa==p.ppa);
                }
        }
    }
    long double before_reads = dev->stat.read_joule;
    for (int lpn=0;lpn<pages;lpn++) {
        NvmeRequest req={.slba=(uint64_t)lpn*dev->sp.secs_per_pg,
                         .nlb=dev->sp.secs_per_pg,.stime=1000000000};
        ssd_read(dev,&req);
    }
    uint64_t reads = dev->stat.single_read + 2ULL*dev->stat.double_read +
                     3ULL*dev->stat.triple_read;
    assert(fabsl((dev->stat.read_joule-before_reads) - reads*3.5L) < 0.001L);
    assert(dev->stat.access_cnt == (uint64_t)(dev->stat.single_read +
           dev->stat.double_read + dev->stat.triple_read));
    puts("PASS LeaFTL read multiplicity equals actual NAND read energy count");
    struct ppa tp0=get_gtd_ent(dev,0);
    assert(mapped_ppa(&tp0));
    assert(dev->stat.gc_cnt>0 && dev->stat.wa_cnt>=6*pages);
    puts("PASS LeaFTL six 512KiB write passes: physical mapping/rmap/TP0/GC program counts");
    uint32_t random_seed=20261012;
    for(int i=0;i<50000;i++) {
        random_seed=random_seed*1664525U+1013904223U;
        NvmeRequest req={.slba=(uint64_t)((random_seed>>8)%pages)*dev->sp.secs_per_pg,
                         .nlb=dev->sp.secs_per_pg,.stime=1000000000};
        ssd_write(dev,&req);
    }
    for(int lpn=0;dev->buffer_pointer;lpn=(lpn+1)%pages) {
        NvmeRequest req={.slba=(uint64_t)lpn*dev->sp.secs_per_pg,
                         .nlb=dev->sp.secs_per_pg,.stime=1000000000};
        ssd_write(dev,&req);
    }
    for(int lpn=0;lpn<pages;lpn++) {
        struct ppa current=dev->maptbl[lpn];
        assert(mapped_ppa(&current) && valid_ppa(dev,&current));
        assert(get_pg(dev,&current)->status==PG_VALID);
        assert(get_rmap_ent(dev,&current)==(uint64_t)lpn);
        NvmeRequest req={.slba=(uint64_t)lpn*dev->sp.secs_per_pg,
                         .nlb=dev->sp.secs_per_pg,.stime=1000000000};
        ssd_read(dev,&req);
    }
    puts("PASS LeaFTL 50000 random overwrites, buffer drain and all-LPN reads");
    puts("PASS LeaFTL singleton/final-sample/boundary/CRB/interval insertion/compaction mappings");
    return 0;
}
