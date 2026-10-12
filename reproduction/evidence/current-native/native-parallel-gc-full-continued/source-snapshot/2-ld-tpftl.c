/* 中文阅读说明：顺序初始化与整组 GC 训练按论文 III-D/III-E 补全。
 * LPN→TP→group WP；读取使用 CMT 的 PPN 或 bitmap 保证准确的 VPPN，maptbl 模拟 TP 内容。
 * WP 的 vic_cnt 含 current；全局 victim_line_cnt 只数队列。DATA 首写后先推进再分配，TP 写后推进。
 * stime/模拟时延单位 ns；GC 移动的是映射和页状态，用户 payload 由外围逻辑 DRAM 后端维护。
 * 被 // 或 #if 0 禁用的代码保留为历史草案，标识符与字符串不作自然语言翻译。
 */

/**
 * 文件名：ld-tpftl.c
 * 作者：Shengzhe Wang
 * 创建时间：2022-05
 * 版本：1.0
 * 功能简介：实现 LearnedFTL（HPCA 2024）的主要 FTL 逻辑。
 * 版权所有 (C) 2023 Shengzhe Wang，保留所有权利。
 * 许可证：按 GPL 许可证分发；作者与权利声明保持原含义。
 */

// #pragma GCC push_options
// #pragma GCC optimize(0)

/* 引入 FTL 类型、状态、常量及 QEMU/NVMe 依赖。 */
#include "ld-tpftl.h"
/* 引入 排序、最小二乘与线性预测函数声明。 */
#include "util.h"
#include <float.h>

/* float16 存储后再用 float32 计算；训练评估与读取采用完全相同的算术。 */
static float model_value(uint64_t x, const lr_breakpoint *brk)
{
    return (float)brk->w * x + (float)brk->b;
}

static bool model_bitmap_get(struct ssd *ssd, uint64_t lpn)
{
    unsigned bit = lpn % ssd->sp.ents_per_pg;
    return (ssd->lr_nodes[lpn / ssd->sp.ents_per_pg].bitmap[bit / 8] >> (bit % 8)) & 1;
}

static void model_bitmap_set(struct ssd *ssd, uint64_t lpn, bool value)
{
    unsigned bit = lpn % ssd->sp.ents_per_pg;
    uint8_t *byte = &ssd->lr_nodes[lpn / ssd->sp.ents_per_pg].bitmap[bit / 8];
    uint8_t mask = 1U << (bit % 8);
    *byte = value ? *byte | mask : *byte & ~mask;
}

/* 历史/诊断代码：以下被注释的声明或实现不参与编译，保留原 C 标识符；不得按它推导当前行为。 */
// static int hit_num = 0;
/* 历史/诊断代码：以下被注释的声明或实现不参与编译，保留原 C 标识符；不得按它推导当前行为。 */
// static int gc_num = 0;
/* 历史/诊断代码：以下被注释的声明或实现不参与编译，保留原 C 标识符；不得按它推导当前行为。 */
// static int gc_line_num = 0;
/* 局部 GC 阈值：WP 拥有 line 数达到 5（含 current）时批量回收。 */
static int gc_threshold = 5;   // ! gc参数：当一个gtd_wp使用了多少个Line时开始GC
/* 全局空间压力阈值：条件严格 free_line_cnt<4，为成对 GC 与元数据写回预留空间。 */
static int free_line_threshold = 4;    // ! gc参数：当还剩多少未使用的free_line时开始GC
/* 历史/诊断代码：以下被注释的声明或实现不参与编译，保留原 C 标识符；不得按它推导当前行为。 */
// static int train_num = 0;

/* 历史/诊断代码：以下被注释的声明或实现不参与编译，保留原 C 标识符；不得按它推导当前行为。 */
// static FILE* gc_fp;

/* 历史/诊断代码：以下被注释的声明或实现不参与编译，保留原 C 标识符；不得按它推导当前行为。 */
// static uint64_t tmp_counter = 0;
/* 历史/诊断代码：以下被注释的声明或实现不参与编译，保留原 C 标识符；不得按它推导当前行为。 */
// static uint64_t counter = 0;
/* 历史/诊断代码：以下被注释的声明或实现不参与编译，保留原 C 标识符；不得按它推导当前行为。 */
// static int line_init = 0;
/* 历史/诊断代码：以下被注释的声明或实现不参与编译，保留原 C 标识符；不得按它推导当前行为。 */
// static int batch_do_gtd_gc = 0;
/* 历史/诊断代码：以下被注释的声明或实现不参与编译，保留原 C 标识符；不得按它推导当前行为。 */
// static int actual_but_not_bitmap = 0;
/* 历史/诊断代码：以下被注释的声明或实现不参与编译，保留原 C 标识符；不得按它推导当前行为。 */
// static int all_zero_num = 0;
/* 历史/诊断代码：以下被注释的声明或实现不参与编译，保留原 C 标识符；不得按它推导当前行为。 */
// static int total_no_pred_num = 0;
/* 历史/诊断代码：以下被注释的声明或实现不参与编译，保留原 C 标识符；不得按它推导当前行为。 */
// static int total_pred_num = 0;
/* 历史/诊断代码：以下被注释的声明或实现不参与编译，保留原 C 标识符；不得按它推导当前行为。 */
// static int no_model_num = 0;


/* 前向声明 gtd_do_gc：回收一条 GTD line：逐 channel/LUN 搬移有效 TP、清 block，再归还 line。 */
static int gtd_do_gc(struct ssd *ssd, bool force, struct write_pointer *wpp, struct line *victim_line, bool delete);
/* 前向声明 batch_gtd_do_gc：遍历 trans WP 所有历史 line，逐条 GTD GC，最后只保留新 current。 */
static void batch_gtd_do_gc(struct ssd *ssd, bool force, struct write_pointer *wpp, int num, struct line *delete_line);
/* 前向声明 batch_line_do_gc：收集整个 group，搬移/重训/写回 TP 后回收旧 line。 */
static int batch_line_do_gc(struct ssd* ssd, bool force, struct write_pointer *wpp, struct line *delete_line);
/* 历史/诊断代码：以下被注释的声明或实现不参与编译，保留原 C 标识符；不得按它推导当前行为。 */
// static void should_do_gc(struct ssd *ssd, struct write_pointer *wpp);
/* 历史/诊断代码：以下被注释的声明或实现不参与编译，保留原 C 标识符；不得按它推导当前行为。 */
// static struct line *select_victim_line(struct ssd *ssd, bool force);
/* 前向声明 insert_wp_lines：把新 curline 的独立所有权节点插到 WP 哨兵后，并增加 vic_cnt。 */
static void insert_wp_lines(struct write_pointer *wpp);
/* 前向声明 should_do_gc_v3：按 group 占用及全局 free 压力选择整组 GC。 */
static bool should_do_gc_v3(struct ssd *ssd, struct write_pointer *wpp);
static void free_all_blocks(struct ssd *ssd, struct ppa *tppa);
static inline struct nand_page *get_pg(struct ssd *ssd, struct ppa *ppa);
static inline struct line *get_line(struct ssd *ssd, struct ppa *ppa);


/**
 * 功能：诊断辅助函数：打印固定字符串并返回指针所指整数，不修改该整数。
 * 参数 c：输入整数指针；只解引用返回，不修改其值。
 * 作用与边界：返回 *c；调用处多用来观察 rest 异常，不能视为恢复或 GC。
 */
static int func(int *c) {
    /* 调用 printf：输出原有诊断/计数信息；日志字符串保留，打印本身不会恢复异常状态。 */
    printf("test\n");
    return *c;
}

/* 前向声明 ftl_thread：等待数据面启动，逐 poller 取请求，执行 FTL 读写并回送带延迟的完成请求。 */
static void *ftl_thread(void *arg);

/**
 * 功能：把翻译页索引 tvpn 映射到 TP 哈希表桶。
 * 参数 tvpn：翻译页逻辑索引，通常由 LPN/ents_per_pg 得到；特殊调用见本函数说明。
 * 作用与边界：返回 tvpn % TP_HASH_SIZE；冲突由 next 单链解决。
 */
static inline uint64_t tp_hash(uint64_t tvpn)
{
    return tvpn % TP_HASH_SIZE;
}

/* 计算 CMT 哈希桶 */
/**
 * 功能：把逻辑页号 lpn 映射到 CMT 哈希表桶。
 * 参数 lpn：当前逻辑页号，DATA 映射索引。
 * 作用与边界：返回 lpn % CMT_HASH_SIZE；与 TP 哈希是两张独立表。
 */
static inline uint64_t cmt_hash(uint64_t lpn)
{
    return lpn % CMT_HASH_SIZE;
}

/**
 * 功能：将 TPnode 插到所属 TP 哈希桶的链头。
 * 参数 ht：哈希表引用，包含 LPN entry 桶和 TPnode 桶。
 * 参数 tpnode：当前 TP 缓存组织节点；一个 TP 不等于一个 allocation group。
 * 作用与边界：仅维护 tpnode.next 和桶头；不改变热度队列或缓存容量。
 */
static void insert_tp_hashtable(hash_table *ht, TPnode *tpnode) 
{
    /* 变量 pos：哈希桶索引，取模得到。 */
    /* 调用 tp_hash：把翻译页索引 tvpn 映射到 TP 哈希表桶。 */
    uint64_t pos = tp_hash(tpnode->tvpn);
    tpnode->next = ht->tp_table[pos];
    ht->tp_table[pos] = tpnode;
}



/**
 * 功能：按 LPN 遍历 CMT 哈希桶链，寻找已有缓存项。
 * 参数 ht：哈希表引用，包含 LPN entry 桶和 TPnode 桶。
 * 参数 lpn：当前逻辑页号，DATA 映射索引。
 * 作用与边界：返回匹配 entry 或 NULL；不改变两层热度队列。
 */
static struct cmt_entry* find_hash_entry(hash_table *ht, uint64_t lpn)
{
    /* 变量 pos：哈希桶索引，取模得到。 */
    /* 调用 cmt_hash：把逻辑页号 lpn 映射到 CMT 哈希表桶。 */
    uint64_t pos = cmt_hash(lpn);
    /* 变量 entry：CMT 缓存项引用；next 为哈希链接，entry 为队列链接。 */
    cmt_entry *entry = ht->cmt_table[pos];
    while (entry != NULL && entry->lpn != lpn) {
        entry = entry->next;
    }
    return entry;
}

/**
 * 功能：按翻译页索引遍历 TP 哈希桶链。
 * 参数 ht：哈希表引用，包含 LPN entry 桶和 TPnode 桶。
 * 参数 tvpn：翻译页逻辑索引，通常由 LPN/ents_per_pg 得到；特殊调用见本函数说明。
 * 作用与边界：返回匹配 TPnode 或 NULL；不是按 allocation group 查询。
 */
static struct TPnode* find_hash_tpnode(hash_table *ht, uint64_t tvpn)
{
    /* 变量 pos：哈希桶索引，取模得到。 */
    /* 调用 tp_hash：把翻译页索引 tvpn 映射到 TP 哈希表桶。 */
    uint64_t pos = tp_hash(tvpn);
    /* 变量 tpnode：当前 TP 缓存组织节点；一个 TP 不等于一个 allocation group。 */
    TPnode *tpnode = ht->tp_table[pos];
    while (tpnode != NULL && tpnode->tvpn != tvpn) {
        tpnode = tpnode->next;
    }
    return tpnode;
}

/**
 * 功能：将缓存映射 entry 插到 LPN 哈希桶链头。
 * 参数 ht：哈希表引用，包含 LPN entry 桶和 TPnode 桶。
 * 参数 entry：CMT 缓存项引用；next 为哈希链接，entry 为队列链接。
 * 作用与边界：entry.next 用于哈希冲突，和 entry.entry 的双向队列链接独立。
 */
static void insert_cmt_hashtable(hash_table *ht, cmt_entry *entry) 
{
    /* 变量 pos：哈希桶索引，取模得到。 */
    /* 调用 cmt_hash：把逻辑页号 lpn 映射到 CMT 哈希表桶。 */
    uint64_t pos = cmt_hash(entry->lpn);
    entry->next = ht->cmt_table[pos];
    ht->cmt_table[pos] = entry;
}

/**
 * 功能：按节点地址从 CMT 哈希桶摘除一个 entry。
 * 参数 ht：哈希表引用，包含 LPN entry 桶和 TPnode 桶。
 * 参数 entry：CMT 缓存项引用；next 为哈希链接，entry 为队列链接。
 * 作用与边界：找到则返回 true，未找到返回 false；只解除哈希链接，不释放槽。入口假定桶非空，空桶缺少防护。
 */
static bool delete_cmt_hashnode(hash_table *ht, cmt_entry *entry)
{
    /* 变量 pos：哈希桶索引，取模得到。 */
    /* 调用 cmt_hash：把逻辑页号 lpn 映射到 CMT 哈希表桶。 */
    uint64_t pos = cmt_hash(entry->lpn);
    /* 变量：
     * tmp_entry：缓存项遍历游标，不是新的缓存槽。
     * pre_entry：哈希冲突单链的前驱缓存项。
     */
    cmt_entry *tmp_entry = ht->cmt_table[pos], *pre_entry;
    if (!tmp_entry) return false;
    if (tmp_entry == entry) {
        ht->cmt_table[pos] = tmp_entry->next;
        tmp_entry->next = NULL;
    } else {
        pre_entry = tmp_entry;
        tmp_entry = tmp_entry->next;
        while (tmp_entry != NULL && tmp_entry != entry) {
            pre_entry = tmp_entry;
            tmp_entry = tmp_entry->next;
        }
        if (tmp_entry == NULL)
            return false;
        pre_entry->next = tmp_entry->next;
        tmp_entry->next = NULL;
    }
    return true;
}

/**
 * 功能：按节点地址从 TP 哈希桶摘除一个 TPnode。
 * 参数 ht：哈希表引用，包含 LPN entry 桶和 TPnode 桶。
 * 参数 tpnode：当前 TP 缓存组织节点；一个 TP 不等于一个 allocation group。
 * 作用与边界：找到则返回 true，未找到返回 false；不释放 TPnode。入口假定桶非空。
 */
static bool delete_tp_hashnode(hash_table *ht, TPnode *tpnode)
{
    /* 变量 pos：哈希桶索引，取模得到。 */
    /* 调用 tp_hash：把翻译页索引 tvpn 映射到 TP 哈希表桶。 */
    uint64_t pos = tp_hash(tpnode->tvpn);
    /* 变量：
     * tmp_tp：TP 哈希链的当前节点。
     * pre_tp：TP 哈希链的前驱节点。
     */
    TPnode *tmp_tp = ht->tp_table[pos], *pre_tp;
    if (!tmp_tp) return false;
    if (tmp_tp == tpnode) {
        ht->tp_table[pos] = tmp_tp->next;
        tmp_tp->next = NULL;
    } else {
        pre_tp = tmp_tp;
        tmp_tp = tmp_tp->next;
        while (tmp_tp != NULL && tmp_tp != tpnode) {
            pre_tp = tmp_tp;
            tmp_tp = tmp_tp->next;
        }
        if (tmp_tp == NULL)
            return false;
        pre_tp->next = tmp_tp->next;
        tmp_tp->next = NULL;
    }
    return true;
}


/**
 * 功能：旧全局 GC 阈值检查：free line 数小于等于 gc_thres_lines 时返回 true。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 作用与边界：当前线程内调用被注释；活跃调度是 should_do_gc_v3，不能把此条件当作实际后台 GC。
 */
static inline bool should_gc(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines);
}

/**
 * 功能：读取完整正向真值表中的 LPN→PPA。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 参数 lpn：当前逻辑页号，DATA 映射索引。
 * 作用与边界：按值返回 PPA；本函数不读 NAND、不查 CMT、不检查 LPN 范围。
 */
static inline struct ppa get_maptbl_ent(struct ssd *ssd, uint64_t lpn)
{
    return ssd->maptbl[lpn];
}

/**
 * 功能：把指定 PPA 按值写入完整 LPN 映射表。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 参数 lpn：当前逻辑页号，DATA 映射索引。
 * 参数 ppa：本次物理地址；DATA/TP 及输入输出语义由本函数路径确定。
 * 作用与边界：仅修改 maptbl；rmap、CMT、bitmap 与页状态需由调用者同步维护。
 */
static inline void set_maptbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    /* 调用 ftl_assert：只在 FEMU_DEBUG_FTL 下检查表达式；默认空宏，不求值也不提供错误恢复。 */
    ftl_assert(lpn < ssd->sp.tt_pgs);
    ssd->maptbl[lpn] = *ppa;
}

/**
 * 功能：把物理地址展平为常规 PPN，供 rmap 和 CMT 使用。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 参数 ppa：本次物理地址；DATA/TP 及输入输出语义由本函数路径确定。
 * 作用与边界：维度权重依次为 ch/lun/pl/blk/pg；sector 不计入页号，返回值不是 packed ppa.ppa。
 */
static uint64_t ppa2pgidx(struct ssd *ssd, struct ppa *ppa)
{
    /* 变量 spp：ssdparams 的局部引用，读取几何/时延/映射尺寸，不复制设备参数。 */
    struct ssdparams *spp = &ssd->sp;
    /* 变量 pgidx：常规物理页号 PPN，供 rmap 线性数组索引。 */
    uint64_t pgidx;

    pgidx = ppa->g.ch  * spp->pgs_per_ch  + \
            ppa->g.lun * spp->pgs_per_lun + \
            ppa->g.pl  * spp->pgs_per_pl  + \
            ppa->g.blk * spp->pgs_per_blk + \
            ppa->g.pg;

    /* 调用 ftl_assert：只在 FEMU_DEBUG_FTL 下检查表达式；默认空宏，不求值也不提供错误恢复。 */
    ftl_assert(pgidx < spp->tt_pgs);

    return pgidx;
}

// 采用按分配顺序编号的 PPA→VPPN 变换
/**
 * 功能：按 channel 最快的布局计算 VPPN，使 line 内 round-robin 分配连续编号。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 参数 ppa：本次物理地址；DATA/TP 及输入输出语义由本函数路径确定。
 * 作用与边界：权重顺序为 ch/lun/pl/pg/blk；没有地址范围检查，不修改 PPA。
 */
static uint64_t ppa2vppn(struct ssd *ssd, struct ppa *ppa) {
    
    /* 变量 spp：ssdparams 的局部引用，读取几何/时延/映射尺寸，不复制设备参数。 */
    struct ssdparams *spp = &ssd->sp;
    /* 变量 vppn：按 channel 最快排列的虚拟物理页号；拆解函数会消耗局部余数。 */
    uint64_t vppn;
    vppn = ppa->g.ch + \
            ppa->g.lun * spp->chn_per_lun + \
            ppa->g.pl * spp->chn_per_pl + \
            ppa->g.pg * spp->chn_per_pg + \
            ppa->g.blk * spp->chn_per_blk;
    
    return vppn;
}

/* 普通 PPN 是 CMT 保存的地址格式，不能按 VPPN 的次序解码。 */
static struct ppa pgidx2ppa(struct ssd *ssd, uint64_t ppn)
{
    struct ssdparams *p = &ssd->sp;
    struct ppa ppa = { .ppa = 0 };
    if (ppn >= (uint64_t)p->tt_pgs) {
        ppa.ppa = UNMAPPED_PPA;
        return ppa;
    }
    ppa.g.ch = ppn / p->pgs_per_ch; ppn %= p->pgs_per_ch;
    ppa.g.lun = ppn / p->pgs_per_lun; ppn %= p->pgs_per_lun;
    ppa.g.pl = ppn / p->pgs_per_pl; ppn %= p->pgs_per_pl;
    ppa.g.blk = ppn / p->pgs_per_blk;
    ppa.g.pg = ppn % p->pgs_per_blk;
    return ppa;
}

/**
 * 功能：按 VPPN 几何步长逐维拆解成 PPA。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 参数 vppn：按 channel 最快排列的虚拟物理页号；拆解函数会消耗局部余数。
 * 作用与边界：返回 blk/pg/pl/lun/ch 字段；局部 union 整体清零，sec/rsv 为零。模型预测通过本函数生成最终 PPA，所有字段先清零。
 */
static struct ppa vppn2ppa(struct ssd *ssd, uint64_t vppn) {
    /* 变量 ppa：本次物理地址；DATA/TP 及输入输出语义由本函数路径确定。 */
    struct ppa ppa = { .ppa = 0 };
    /* 变量 spp：ssdparams 的局部引用，读取几何/时延/映射尺寸，不复制设备参数。 */
    struct ssdparams *spp = &ssd->sp;
    ppa.g.blk = vppn / spp->chn_per_blk;
    vppn -= ppa.g.blk*spp->chn_per_blk;
    ppa.g.pg = vppn / spp->chn_per_pg;
    vppn -= ppa.g.pg * spp->chn_per_pg;
    ppa.g.pl = vppn / spp->chn_per_pl;
    vppn -= ppa.g.pl * spp->chn_per_pl;
    ppa.g.lun = vppn / spp->chn_per_lun;
    ppa.g.ch = vppn - ppa.g.lun * spp->chn_per_lun;

    return ppa;
}

/**
 * 功能：由 PPA 得到 PPN，再读取该物理页的反向标识。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 参数 ppa：本次物理地址；DATA/TP 及输入输出语义由本函数路径确定。
 * 作用与边界：DATA 页标识为 LPN，GTD 页标识为翻译页索引；不区分页类型，也不检查越界。
 */
static inline uint64_t get_rmap_ent(struct ssd *ssd, struct ppa *ppa)
{
    /* 变量 pgidx：常规物理页号 PPN，供 rmap 线性数组索引。 */
    /* 调用 ppa2pgidx：把物理地址展平为常规 PPN，供 rmap 和 CMT 使用。 */
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    return ssd->rmap[pgidx];
}

/* 设置反向标识 rmap[普通 PPN]=逻辑标识，元数据页传 TP 索引 */
/**
 * 功能：由 PPA 得到 PPN，将反向标识写入 rmap。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 参数 lpn：当前逻辑页号，DATA 映射索引。
 * 参数 ppa：本次物理地址；DATA/TP 及输入输出语义由本函数路径确定。
 * 作用与边界：参数 lpn 名字泛指逻辑标识；元数据调用会传 TP 索引，失效调用会传 INVALID_LPN。
 */
static inline void set_rmap_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    /* 变量 pgidx：常规物理页号 PPN，供 rmap 线性数组索引。 */
    /* 调用 ppa2pgidx：把物理地址展平为常规 PPN，供 rmap 和 CMT 使用。 */
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    /* INVALID data pages retain their OOB LPN until erase. This lets the
     * logical group's invalid-page counter survive cross-group placement.
     * Page validity, not an erased reverse label, determines whether it is live.
     */
    if (lpn == INVALID_LPN && get_line(ssd, ppa)->type == DATA &&
        get_pg(ssd, ppa)->status == PG_INVALID) {
        return;
    }

    ssd->rmap[pgidx] = lpn;
}

/**
 * 功能：为旧优先队列提供优先级比较回调。
 * 参数 next：待比较的下一个优先级值。
 * 参数 curr：当前优先级值。
 * 作用与边界：返回 next > curr，比较的是 vpc；活跃 victim 选择使用 FIFO 链表，优先队列更新代码已注释。
 */
static inline int victim_line_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
}

/**
 * 功能：读取 line.vpc，供旧优先队列用作优先级。
 * 参数 a：通用回调对象指针；本文件 pqueue 回调将其解释为 line。
 * 作用与边界：a 是 line 的通用指针；不扫描 NAND。
 */
static inline pqueue_pri_t victim_line_get_pri(void *a)
{
    return ((struct line *)a)->vpc;
}

/**
 * 功能：将旧优先队列传入的优先级写到 line.vpc。
 * 参数 a：通用回调对象指针；本文件 pqueue 回调将其解释为 line。
 * 参数 pri：旧优先队列传入的优先级，实际使用 line.vpc。
 * 作用与边界：属于 pqueue 回调；活跃回收未通过它选择 victim。
 */
static inline void victim_line_set_pri(void *a, pqueue_pri_t pri)
{
    ((struct line *)a)->vpc = pri;
}

/**
 * 功能：读取 line 在旧优先队列堆中的位置。
 * 参数 a：通用回调对象指针；本文件 pqueue 回调将其解释为 line。
 * 作用与边界：pos 是堆位置，不是 NAND page 或逻辑 group。
 */
static inline size_t victim_line_get_pos(void *a)
{
    return ((struct line *)a)->pos;
}

/**
 * 功能：保存旧优先队列堆位置到 line.pos。
 * 参数 a：通用回调对象指针；本文件 pqueue 回调将其解释为 line。
 * 参数 pos：旧 pqueue 堆中的位置下标，保存到 line.pos；不是哈希桶索引。
 * 作用与边界：只修改位置字段。
 */
static inline void victim_line_set_pos(void *a, size_t pos)
{
    ((struct line *)a)->pos = pos;
}

/**
 * 功能：分配全部 line 实体、初始化全局队列，并把每条 line 放入 free 池。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 作用与边界：line.id 对应共同 block id；默认一条 line 含 64 个 block。旧 pqueue/full 队列虽初始化，活跃回收用 victim_list。
 */
static void ssd_init_lines(struct ssd *ssd)
{
    /* 变量 spp：ssdparams 的局部引用，读取几何/时延/映射尺寸，不复制设备参数。 */
    struct ssdparams *spp = &ssd->sp;
    /* 变量 lm：全局 line 管理器引用，管理实体和 free/victim 队列计数。 */
    struct line_mgmt *lm = &ssd->lm;
    /* 变量 line：设备内的跨 LUN line 实体引用，id 对应 block id。 */
    struct line *line;

    /* 阶段 1：分配 line 实体并初始化 free/full/victim 容器与旧优先队列。 */
    lm->tt_lines = spp->blks_per_pl;
    /* 调用 ftl_assert：只在 FEMU_DEBUG_FTL 下检查表达式；默认空宏，不求值也不提供错误恢复。 */
    ftl_assert(lm->tt_lines == spp->tt_lines);
    /* 调用 g_malloc0：按给定字节数分配零初始化内存；分配的是状态/引用结构而不是 NAND payload。 */
    lm->lines = g_malloc0(sizeof(struct line) * lm->tt_lines);

    /* 调用 QTAILQ_INIT：初始化双向队列头，不创建/释放元素。 */
    QTAILQ_INIT(&lm->free_line_list);
    /* 调用 pqueue_init：创建旧 victim 优先队列并绑定比较/读写优先级/堆位置回调；活跃选择使用 victim_list。 */
    lm->victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri,
            victim_line_get_pri, victim_line_set_pri,
            victim_line_get_pos, victim_line_set_pos);
    /* 调用 QTAILQ_INIT：初始化双向队列头，不创建/释放元素。 */
    QTAILQ_INIT(&lm->full_line_list);
    
    // LearnedFTL 扩展部分
    /* 调用 QTAILQ_INIT：初始化双向队列头，不创建/释放元素。 */
    QTAILQ_INIT(&lm->victim_list);

    lm->free_line_cnt = 0;
    /* 变量 i：当前数组/外层循环下标，范围由紧邻 for 条件确定。 */
    for (int i = 0; i < lm->tt_lines; i++) {
        /* 阶段 2：按 block id 建立每条 line，初始容量全部可用并加入 free 池。 */
        line = &lm->lines[i];
        line->id = i;
        line->rest = spp->pgs_per_line;
        line->ipc = 0;
        line->vpc = 0;
        line->pos = 0;
        line->type = UNUSED;
        // line->rest = spp
        /* 初始化时把所有 line 作为 free line 加入队列 */
        /* 调用 QTAILQ_INSERT_TAIL：用对象内 entry 链接插到队尾；free 池回收/victim FIFO 均显式维护计数。 */
        QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
        lm->free_line_cnt++;
    }

    /* 阶段 3：核对 free 数量，初始化全局 victim/full 计数。 */
    /* 调用 ftl_assert：只在 FEMU_DEBUG_FTL 下检查表达式；默认空宏，不求值也不提供错误恢复。 */
    ftl_assert(lm->free_line_cnt == lm->tt_lines);
    lm->victim_line_cnt = 0;
    lm->full_line_cnt = 0;

}



/**
 * 功能：为 WP 取得一条 free line，建立初始坐标、页类型和所有权关系。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 参数 wpp：本次分配/回收使用的写指针；DATA group 与 trans_wp 的推进约定不同。
 * 参数 gc_flag：分配前是否允许在 free 紧张时调用 GC；false 避免该入口立即递归调度。
 * 作用与边界：gc_flag=true 且 free<3 时先调度 GC；res=true 直接返回。无 free line 时终止并保留错误现场，避免继续写满页。分配 line 时 vic_cnt 增加，含 current。
 */
static void init_line_write_pointer(struct ssd *ssd, struct write_pointer *wpp, bool gc_flag)
{
    /* 变量 lm：全局 line 管理器引用，管理实体和 free/victim 队列计数。 */
    struct line_mgmt *lm = &ssd->lm;
    /* 变量 curline：即将分配给 WP 的 free line 引用。 */
    struct line *curline = NULL;
    
    /* 阶段 1：按 gc_flag 决定是否先处理全局 free<3 的压力。 */
    if (gc_flag && !ssd->gc_in_progress) {
        if (lm->free_line_cnt < free_line_threshold) {
            // struct timespec time1, time2;

            // clock_gettime(CLOCK_MONOTONIC, &time1);
            /* 变量 res：GC 调度返回的 caller 控制标志；true 不证明分配成功，false 也可能回收了其他 WP。 */
            /* 调用 should_do_gc_v3：按当前 WP 占用、翻译 WP 占用及全局 free 压力选择整组 GC。 */
            bool res = should_do_gc_v3(ssd, wpp);
            // clock_gettime(CLOCK_MONOTONIC, &time2);
                
            // ssd->stat.GC_time += ((time2.tv_sec - time1.tv_sec)*1000000000 + (time2.tv_nsec - time1.tv_nsec));
            if (res) {
                return;
            }
        } 
    }

        
    /* 阶段 2：取得并摘除 free 队头；失败仅报错返回，不建立有效目标。 */

    /* 调用 QTAILQ_FIRST：取得双向队列头元素或 NULL，不摘链。 */
    curline = QTAILQ_FIRST(&lm->free_line_list);
    if (!curline) {
        /* 调用 ftl_err：通过错误日志宏输出到 stderr；宏本身不终止函数或修复状态。 */
        ftl_err("No free lines left in [%s]\n", ssd->ssdname);
        abort();
    }
    /* 调用 QTAILQ_REMOVE：按队列头、节点、链接字段 entry 摘除对象；不释放对象，也不改业务计数。 */
    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;
    /* 阶段 3：设置新 line 的首坐标、DATA/GTD 类型及 line→WP 反查。 */

    /* wpp->curline is always our next-to-write super-block */
    wpp->curline = curline;
    wpp->free_pages = curline->rest;
    wpp->ch = 0;
    wpp->lun = 0;
    wpp->pg = 0;
    wpp->blk = curline->id;
    wpp->pl = 0;
    ssd->line2write_pointer[wpp->curline->id] = wpp;
    if (&ssd->trans_wp == wpp) {
        wpp->curline->type = GTD;
    } else {
        wpp->curline->type = DATA;
        // printf("write pointer %d init line id: %d\n", wpp->id, wpp->curline->id);
    }

    if (wpp->curline->rest == 0) {
        /* 调用 func：诊断辅助函数：打印固定字符串并返回指针所指整数，不修改该整数。 */
        func(&wpp->curline->rest);
        /* 调用 printf：输出原有诊断/计数信息；日志字符串保留，打印本身不会恢复异常状态。 */
        printf("what's up?\n");
    }    

    /* 阶段 4：创建 WP 所有权节点并增加包含 current 的 vic_cnt。 */
    /* 调用 insert_wp_lines：把新 curline 的独立所有权节点插到 WP 哨兵后，并增加 vic_cnt。 */
    insert_wp_lines(wpp);
    

}

/**
 * 功能：分配数据 group 的 WP 数组、哨兵链和 line→owner 反查表，并先给 trans_wp 分配 line。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 作用与边界：初始化全部 tt_line_wps 个 group 的哨兵；trans_wp.id=tt_lines。
 */
static void ssd_init_write_pointer(struct ssd *ssd)
{
    
    
    /* 阶段 1：为每个逻辑 group 建立哨兵及 id。 */
    /* 调用 g_malloc0：按给定字节数分配零初始化内存；分配的是状态/引用结构而不是 NAND payload。 */
    ssd->gtd_wps = g_malloc0(sizeof(struct write_pointer) * ssd->sp.tt_line_wps);
    
    /* 变量 i：当前数组/外层循环下标，范围由紧邻 for 条件确定。 */
    for (int i = 0; i < ssd->sp.tt_line_wps; i++) {
        ssd->gtd_wps[i].curline = NULL;
        /* 调用 g_malloc0：按给定字节数分配零初始化内存；分配的是状态/引用结构而不是 NAND payload。 */
        ssd->gtd_wps[i].wpl = g_malloc0(sizeof(struct wp_lines));
        ssd->gtd_wps[i].wpl->line = NULL;
        ssd->gtd_wps[i].wpl->next = NULL;
        ssd->gtd_wps[i].vic_cnt = 0;
        ssd->gtd_wps[i].id = i;
    }
    /* 阶段 2：建立独立 trans_wp 哨兵，id 默认为 256。 */
    /* 调用 g_malloc0：按给定字节数分配零初始化内存；分配的是状态/引用结构而不是 NAND payload。 */
    ssd->trans_wp.wpl = g_malloc0(sizeof(struct wp_lines));
    ssd->trans_wp.wpl->line = NULL;
    ssd->trans_wp.wpl->next = NULL;
    ssd->trans_wp.vic_cnt = 0;
    ssd->trans_wp.id = ssd->sp.tt_lines;
    // 初始化 line id→所属 WP 的反查数组
    /* 阶段 3：建立 owner 反查并首先分配翻译页 line；数据 line 按需分配。 */
    /* 调用 g_malloc0：按给定字节数分配零初始化内存；分配的是状态/引用结构而不是 NAND payload。 */
    ssd->line2write_pointer = g_malloc0(sizeof(struct write_pointer *) * ssd->sp.tt_lines);
    /* 调用 init_line_write_pointer：为 WP 取得一条 free line，建立初始坐标、页类型和所有权关系。 */
    init_line_write_pointer(ssd, &ssd->trans_wp, false);


}

/**
 * 功能：在调试模式下检查一个坐标是否属于 [0,max)。
 * 参数 a：通用回调对象指针；本文件 pqueue 回调将其解释为 line。
 * 参数 max：合法坐标的排他上界。
 * 作用与边界：通过 ftl_assert 实现；默认非调试构建不执行检查，不能当作运行时错误处理。
 */
static inline void check_addr(int a, int max)
{
    /* 调用 ftl_assert：只在 FEMU_DEBUG_FTL 下检查表达式；默认空宏，不求值也不提供错误恢复。 */
    ftl_assert(a >= 0 && a < max);
}

/* 历史/诊断代码：以下被注释的声明或实现不参与编译，保留原 C 标识符；不得按它推导当前行为。 */
// static struct line *get_next_free_line(struct ssd *ssd)
// {
//     struct line_mgmt *lm = &ssd->lm;
//     struct line *curline = NULL;

//     curline = QTAILQ_FIRST(&lm->free_line_list);
//     if (!curline) {
//         ftl_err("No free lines left in [%s] !!!!\n", ssd->ssdname);
//         return NULL;
//     }

//     QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
//     lm->free_line_cnt--;
//     return curline;
// }



/**
 * 功能：把新 curline 的独立所有权节点插到 WP 哨兵后，并增加 vic_cnt。
 * 参数 wpp：本次分配/回收使用的写指针；DATA group 与 trans_wp 的推进约定不同。
 * 作用与边界：WP 链包含 current；不把 line 加入全局 victim 队列，也不分配 line 本体。
 */
static void insert_wp_lines(struct write_pointer *wpp) {
    /* 变量 wpl：WP 所有权单链的遍历/插入节点，不是 line 本体；链头是哨兵。 */
    /* 调用 g_malloc0：按给定字节数分配零初始化内存；分配的是状态/引用结构而不是 NAND payload。 */
    struct wp_lines *wpl = g_malloc0(sizeof(struct wp_lines));
    wpl->line = wpp->curline;
    wpl->next = wpp->wpl->next;
    wpp->wpl->next = wpl;
    wpp->vic_cnt++;
    return;
}


// 待完成：跨组借用或 GC 函数；下面整段草案被注释，不执行
/* 历史/诊断代码：以下被注释的声明或实现不参与编译，保留原 C 标识符；不得按它推导当前行为。 */
// static bool borrow_or_gc(struct ssd *ssd, struct write_pointer *wpp) {
    
//     struct line_mgmt *lm = &ssd->lm;

//     if (lm->free_line_cnt < 4) {



//     } else {
//         步骤 1：草案拟查找未满 line，尚未实现可用借页机制
//         int min_remain_pgs = ssd->sp.pgs_per_line;
//         int min_remain_line_id = -1;
//         for (int i = 0; i < lm->tt_lines; i++) {
//             if (lm->lines[i].rest < min_remain_line_id) {
//                 min_remain_line_id = i;
//             }
//         }

//         wpp->invade_lines ++;
//         步骤 2：草案拟把未满 line 放入队尾

//         步骤 3：草案拟给借用者 WP 增加所有权引用节点

//         说明：草案考虑一条 line 混写多个 WP 的页，
//         因此还需改 GC 写回以维护多 owner；当前未完成
//     }
    
    
// }

/* current 可能已由写满路径封存；部分写入的 current 只入 victim 队列一次。 */
static void seal_current_line(struct ssd *ssd, struct write_pointer *wpp)
{
    struct line *line;
    if (!wpp->curline) {
        return;
    }
    QTAILQ_FOREACH(line, &ssd->lm.victim_list, entry) {
        if (line == wpp->curline) {
            return;
        }
    }
    QTAILQ_INSERT_TAIL(&ssd->lm.victim_list, wpp->curline, entry);
    ssd->lm.victim_line_cnt++;
}

/* 空间压力下按组内无效页总数选择 victim；同一组整体搬移、整体重训。 */
static bool should_do_gc_v3(struct ssd *ssd, struct write_pointer *wpp)
{
    if (ssd->gc_in_progress) {
        return false;
    }
    struct write_pointer *selected = NULL;
    /* 数据 GC 在擦除源 line 前要写回至多两组 TP。若备用 line 紧张，
     * 先压缩几乎写满的 TP line，恢复写回槽位，再继续选择数据组。
     * 这次元数据整理本身不代表已给 caller 建立新的数据目标。 */
    if (wpp != &ssd->trans_wp &&
        ssd->lm.free_line_cnt < free_line_threshold &&
        ssd->trans_wp.curline->rest <= 2 * ssd->sp.trans_per_line &&
        ssd->lm.free_line_cnt > 0) {
        batch_gtd_do_gc(ssd, true, &ssd->trans_wp, ssd->trans_wp.vic_cnt, NULL);
    }
    /* 满的 TP line 尚未取得下一空页；先压缩元数据，数据 GC 才有 TP 写回目标。 */
    if (wpp == &ssd->trans_wp && wpp->curline && wpp->curline->rest == 0) {
        selected = wpp;
    } else if (ssd->trans_wp.vic_cnt >= gc_threshold ||
               (ssd->lm.free_line_cnt < free_line_threshold &&
                ssd->trans_wp.vic_cnt > 1)) {
        /* 空间紧张时先合并多条 TP line，可净释放 line；单条 TP line
         * 的压缩没有这项收益，不能抢占数据组回收。 */
        selected = &ssd->trans_wp;
    } else if ((wpp && wpp->vic_cnt >= gc_threshold) ||
               ssd->lm.free_line_cnt < free_line_threshold) {
        uint64_t most_invalid = 0;
        /* 数据空间压力只在逻辑数据组间按无效页选择。翻译页已有
         * 独立的满 line/累计阈值触发；不能用其无效页数抢占此处，
         * 否则反复压缩一条 TP line 不释放 line，却继续消耗数据备用空间。 */
        for (int group = 0; group < ssd->sp.tt_line_wps; group++) {
            struct write_pointer *candidate = &ssd->gtd_wps[group];
            if (!candidate->curline || !candidate->wpl) {
                continue;
            }
            uint64_t invalid = candidate->invalid_pages;
            int targets = candidate->borrow_peer ? 2 : 1;
            int writes = targets * ssd->sp.trans_per_line;
            int metadata = writes >= ssd->trans_wp.curline->rest;
            if (ssd->lm.free_line_cnt < targets + metadata) continue;
            /* 回收其他组必须净释放 line，caller 随后还要分配自己的目标。
             * 成对组可能仅有两个源 line；整理到两个目标虽能清无效页，
             * 却不增加 free line，不能据此继续消耗全局 GC 备用空间。
             * 若直接重建 caller，则它使用整理后的 current，无需额外目标。 */
            int sources = candidate->vic_cnt +
                          (candidate->borrow_peer ? candidate->borrow_peer->vic_cnt : 0);
            bool rebuilds_caller = candidate == wpp || candidate->borrow_peer == wpp;
            if (!rebuilds_caller && sources <= targets + metadata) continue;
            if (invalid > most_invalid) {
                most_invalid = invalid;
                selected = candidate;
            }
        }
    }
    if (!selected || ssd->lm.free_line_cnt < 1) {
        return false;
    }
    bool rebuilt_caller = selected == wpp || selected->borrow_peer == wpp;
    if (selected == &ssd->trans_wp) {
        batch_gtd_do_gc(ssd, true, selected, selected->vic_cnt, NULL);
    } else if (batch_line_do_gc(ssd, true, selected, NULL) != 0) {
        return false;
    }
    return rebuilt_caller;
}

/**
 * 功能：从带哨兵的 WP 所有权单链中删除指定 line 的一个引用节点。
 * 参数 wpl：WP 所有权单链的遍历/插入节点，不是 line 本体；链头是哨兵。
 * 参数 victim_line：待回收历史 line 的实体，须已在全局 victim 队列。
 * 作用与边界：只 g_free 引用节点，不释放 line 实体、不改 vic_cnt；计数由 caller 单独维护。
 */
static void clear_one_write_pointer_victim_lines(struct wp_lines *wpl, struct line *victim_line) {
    /* 变量 tmp：待移除的 WP 引用节点，摘链后释放。 */
    struct wp_lines *tmp;
    while (wpl) {
        if (wpl->next && wpl->next->line == victim_line) {
            tmp = wpl->next;
            wpl->next = tmp->next;
            
            /* 调用 g_free：释放指定动态分配对象；ownership 节点释放不等于释放其引用的 line 实体。 */
            g_free(tmp);
            break;
        }
        wpl = wpl->next;
    }
    
}

/**
 * 功能：推进 line 写坐标并在回绕时调度 GC。
 * 原作者：wsz
 * 参数 ssd：设备状态；参数 wpp：本次推进的写指针。
 * 原注释的 lwp 对应实际签名中的 wpp。
 */
/**
 * 功能：把 line 写坐标按 channel→lun→page 推进；写满后封存 line 并调度 GC/分配。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 参数 wpp：本次分配/回收使用的写指针；DATA group 与 trans_wp 的推进约定不同。
 * 作用与边界：不执行 blk++；新 blk 来自 free line.id。DATA 在 GC 搬移了页时才再推进；零页搬移保留首空页，translation 写后推进。
 */
static void advance_line_write_pointer (struct ssd *ssd, struct write_pointer *wpp) {
    /* 变量 spp：ssdparams 的局部引用，读取几何/时延/映射尺寸，不复制设备参数。 */
    struct ssdparams *spp = &ssd->sp;
    /* 变量 lm：全局 line 管理器引用，管理实体和 free/victim 队列计数。 */
    struct line_mgmt *lm = &ssd->lm;

    /* 阶段 1：channel 最快轮转；未回绕时函数直接结束。 */
    another_try:

    /* 校正原注释：实际为 channel→lun→page；blk 不自增，只在分配新 line 时取其 id。 */
    //先channel++,再lun++,再block++
    /* 调用 check_addr：在调试模式下检查一个坐标是否属于 [0,max)。 */
    check_addr(wpp->ch, spp->nchs);
    wpp->ch++;
    if (wpp->ch == spp->nchs) {
        wpp->ch = 0;
        /* 调用 check_addr：在调试模式下检查一个坐标是否属于 [0,max)。 */
        check_addr(wpp->lun, spp->luns_per_ch);
        /* 阶段 2：channel 回绕后增加 LUN；LUN 回绕后增加 block 内 page。 */
        wpp->lun++;
        /* LUN 轮转判断；此前 channel 回绕后已增加 lun */
        if (wpp->lun == spp->luns_per_ch) {
            wpp->lun = 0;
            /* LUN 一圈结束，进入各 block 的下一页层 */
            /* 调用 check_addr：在调试模式下检查一个坐标是否属于 [0,max)。 */
            check_addr(wpp->pg, spp->pgs_per_blk);
            wpp->pg++;
            /* 阶段 3：page 层全部走完，当前 line 封存入全局 victim 队列。 */
            if (wpp->pg == spp->pgs_per_blk) {
                
                wpp->pg = 0;

                /* 调用 QTAILQ_INSERT_TAIL：用对象内 entry 链接插到队尾；free 池回收/victim FIFO 均显式维护计数。 */
                QTAILQ_INSERT_TAIL(&lm->victim_list, wpp->curline, entry);
                // pqueue_insert(lm->victim_line_pq, wpp->curline);
                // wpp->vic_cnt++;
                

                lm->victim_line_cnt++;

                // 待完成：此处考虑跨 group 借页；当前没有执行机制
                // bool res = borrow_or_gc(ssd, wpp);
                // struct timespec time1, time2;

                // clock_gettime(CLOCK_MONOTONIC, &time1);
                /* 阶段 4：调度 GC；false 另取 free line，DATA 的 true 再推进到 GC 最后写页之后。 */
                /* 变量 res：GC 调度返回的 caller 控制标志；true 不证明分配成功，false 也可能回收了其他 WP。 */
                /* 调用 should_do_gc_v3：按当前 WP 占用、翻译 WP 占用及全局 free 压力选择整组 GC。 */
                bool res = !ssd->gc_in_progress && should_do_gc_v3(ssd, wpp);
                // clock_gettime(CLOCK_MONOTONIC, &time2);
                
                // ssd->stat.GC_time += ((time2.tv_sec - time1.tv_sec)*1000000000 + (time2.tv_nsec - time1.tv_nsec));
                
                
                
                
                if (!res)
                    /* 调用 init_line_write_pointer：为 WP 取得一条 free line，建立初始坐标、页类型和所有权关系。 */
                    init_line_write_pointer(ssd, wpp, false);
                else if (wpp != &ssd->trans_wp && wpp->curline->rest < spp->pgs_per_line) {
                    if (wpp->curline->rest == 0) {
                        /* 调用 func：诊断辅助函数：打印固定字符串并返回指针所指整数，不修改该整数。 */
                        func(&wpp->curline->rest);
                        /* 调用 printf：输出原有诊断/计数信息；日志字符串保留，打印本身不会恢复异常状态。 */
                        printf("what's up?\n");
                    }

                    goto another_try;
                }
            }
        }
    }
}

/* 历史/诊断代码：以下被注释的声明或实现不参与编译，保留原 C 标识符；不得按它推导当前行为。 */
// static void advance_gc_trans_write_pointer (struct ssd *ssd, struct write_pointer *wpp) {
//     struct ssdparams *spp = &ssd->sp;
//     struct line_mgmt *lm = &ssd->lm;

    /* 校正原注释：实际为 channel→lun→page；blk 不自增，只在分配新 line 时取其 id。 */
//     //先channel++,再lun++,再block++
//     check_addr(wpp->ch, spp->nchs);
//     wpp->ch++;
//     if (wpp->ch == spp->nchs) {
//         wpp->ch = 0;
//         check_addr(wpp->lun, spp->luns_per_ch);
//         wpp->lun++;
//         LUN 轮转判断；此前 channel 回绕后已增加 lun
//         if (wpp->lun == spp->luns_per_ch) {
//             wpp->lun = 0;
//             LUN 一圈结束，进入各 block 的下一页层
//             check_addr(wpp->pg, spp->pgs_per_blk);
//             wpp->pg++;
//             if (wpp->pg == spp->pgs_per_blk) {
                
//                 wpp->pg = 0;
//                 QTAILQ_INSERT_TAIL(&lm->victim_list, wpp->curline, entry);
//                 // pqueue_insert(lm->victim_line_pq, wpp->curline);
//                 wpp->vic_cnt++;
//                 lm->victim_line_cnt++;

//                 init_line_write_pointer(ssd, wpp, false);
//                 wpp->vic_cnt++;
//                 batch_gtd_do_gc(ssd, true, wpp, wpp->vic_cnt, NULL);
                
                
                
//             }
//         }
//     }
// }

/**
 * 功能：把 WP 当前坐标打包成 PPA，并消耗 curline 的一个剩余页槽。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 参数 wpp：本次分配/回收使用的写指针；DATA group 与 trans_wp 的推进约定不同。
 * 作用与边界：先 ppa.ppa=0，再填几何字段，rest--；不推进 WP、不设页有效、不更新映射。当前默认要求单 plane。
 */
static struct ppa get_new_line_page(struct ssd *ssd, struct write_pointer *wpp)
{
    if (!wpp->curline || wpp->curline->rest <= 0) {
        ftl_err("Allocation exhausted: group=%d line=%d rest=%d ch=%d lun=%d pg=%d\n",
                wpp->id, wpp->curline ? wpp->curline->id : -1,
                wpp->curline ? wpp->curline->rest : -1, wpp->ch, wpp->lun, wpp->pg);
        abort();
    }
    /* 变量 ppa：本次物理地址；DATA/TP 及输入输出语义由本函数路径确定。 */
    struct ppa ppa;
    ppa.ppa = 0;
    ppa.g.ch = wpp->ch;
    ppa.g.lun = wpp->lun;
    ppa.g.pg = wpp->pg;
    ppa.g.blk = wpp->blk;
    ppa.g.pl = wpp->pl;
    wpp->curline->rest--;
    wpp->free_pages = wpp->curline->rest;
    if (wpp->curline->rest < 0) {
        /* 调用 printf：输出原有诊断/计数信息；日志字符串保留，打印本身不会恢复异常状态。 */
        printf("buduijin\n");
        /* 调用 func：诊断辅助函数：打印固定字符串并返回指针所指整数，不修改该整数。 */
        func(&wpp->curline->rest);
    }
    /* 调用 ftl_assert：只在 FEMU_DEBUG_FTL 下检查表达式；默认空宏，不求值也不提供错误恢复。 */
    ftl_assert(ppa.g.pl == 0);

    return ppa;
}

/**
 * 功能：保留的几何检查接口。
 * 参数 spp：ssdparams 的局部引用，读取几何/时延/映射尺寸，不复制设备参数。
 * 作用与边界：原来的 2 的幂断言均被注释，当前函数为空；没有实际参数验证。
 */
static void check_params(struct ssdparams *p)
{
    /* 当前论文几何采用单 plane；拒绝不支持的配置，不能静默少回收 plane。 */
    if (p->pls_per_lun != 1 || p->nchs <= 0 || p->luns_per_ch <= 0 ||
        p->ents_per_pg <= 0 || p->ents_per_pg > 512 ||
        p->trans_per_line <= 0 || p->tt_cmt_size <= 0 ||
        p->tt_lines > 512 || p->tt_line_wps > 512 ||
        p->trans_per_line * p->ents_per_pg != p->pgs_per_line ||
        p->tt_gtd_size >= p->pgs_per_line) {
        ftl_err("Unsupported LearnedFTL geometry or metadata reserve\n");
        abort();
    }
}

/**
 * 功能：设置默认 NAND 几何、时延、映射缓存、group 尺寸及 VPPN 步长。
 * 参数 spp：ssdparams 的局部引用，读取几何/时延/映射尺寸，不复制设备参数。
 * 作用与边界：默认物理 32 GiB、每 TP 512 映射、每 group 64 TP、每 line 32768 页。实际暴露容量由外围控制器决定；当前分配仍限单 plane。
 */
static void ssd_init_params(struct ssdparams *spp)
{
    /* 阶段 1：默认 sector/page/block/plane/LUN/channel 几何。 */
    spp->secsz = 512;
    spp->secs_per_pg = 8;
    spp->pgs_per_blk = 512;
    spp->blks_per_pl = 256; /* 原配置说明：物理 32 GiB，逻辑 30 GiB，预留 2 GiB；实际 namespace 容量还由外围参数决定 */
    spp->pls_per_lun = 1;
    spp->luns_per_ch = 8;   /* 默认 8 */
    spp->nchs = 8;          /* 默认 8 */

    /* 阶段 2：设置基础读写擦时延，单位 ns；channel transfer 当前为 0。 */
    spp->pg_rd_lat = NAND_READ_LATENCY;
    spp->pg_wr_lat = NAND_PROG_LATENCY;
    spp->blk_er_lat = NAND_ERASE_LATENCY;
    spp->ch_xfer_lat = 0;

    /* 由基础几何推导的数量 */
    /* 阶段 3：由基础几何计算 sector/page/block/plane/LUN 总量。 */
    spp->secs_per_blk = spp->secs_per_pg * spp->pgs_per_blk;
    spp->secs_per_pl = spp->secs_per_blk * spp->blks_per_pl;
    spp->secs_per_lun = spp->secs_per_pl * spp->pls_per_lun;
    spp->secs_per_ch = spp->secs_per_lun * spp->luns_per_ch;
    spp->tt_secs = spp->secs_per_ch * spp->nchs;

    spp->pgs_per_pl = spp->pgs_per_blk * spp->blks_per_pl;
    spp->pgs_per_lun = spp->pgs_per_pl * spp->pls_per_lun;
    spp->pgs_per_ch = spp->pgs_per_lun * spp->luns_per_ch;
    spp->tt_pgs = spp->pgs_per_ch * spp->nchs;

    spp->blks_per_lun = spp->blks_per_pl * spp->pls_per_lun;
    spp->blks_per_ch = spp->blks_per_lun * spp->luns_per_ch;
    spp->tt_blks = spp->blks_per_ch * spp->nchs;

    spp->pls_per_ch =  spp->pls_per_lun * spp->luns_per_ch;
    spp->tt_pls = spp->pls_per_ch * spp->nchs;

    spp->tt_luns = spp->luns_per_ch * spp->nchs;

    /* line 是跨 LUN 的特殊分配单位，在总几何计算后推导 */
    /* 阶段 4：在单 plane 假设下推导 line 几何和旧全局 GC 阈值。 */
    spp->blks_per_line = spp->tt_luns; /* 待完成：多 plane 条件下需重新校正 line 几何 */
    spp->pgs_per_line = spp->blks_per_line * spp->pgs_per_blk;
    spp->secs_per_line = spp->pgs_per_line * spp->secs_per_pg;
    spp->tt_lines = spp->blks_per_lun; /* 待完成：多 plane 条件下需重新校正 line 几何 */

    spp->gc_thres_pcent = 0.75;
    spp->gc_thres_lines = (int)((1 - spp->gc_thres_pcent) * spp->tt_lines);
    spp->gc_thres_pcent_high = 0.95;
    spp->gc_thres_lines_high = (int)((1 - spp->gc_thres_pcent_high) * spp->tt_lines);
    spp->enable_gc_delay = true;


    //LearnedFTL 扩展部分
    // 在 demand-mapping 中计算每张翻译页能容纳多少映射地址
    // 512 = 4KB / 8Byte 
    /* 阶段 5：推导翻译页/group/WP 个数并设置 CMT 容量和预取。 */
    spp->addr_size = 8;
    spp->pg_size = spp->secsz * spp->secs_per_pg;
    spp->ents_per_pg = spp->pg_size / spp->addr_size;
    spp->tt_trans_pgs = spp->tt_pgs / spp->ents_per_pg;
    // 每 TP 覆盖 512 个逻辑页；默认 64 张 TP 的逻辑覆盖量等于一条 line 的页容量，从而组成一个 allocation group
    spp->trans_per_line = spp->pgs_per_line / spp->ents_per_pg;
    spp->tt_line_wps = spp->tt_trans_pgs/spp->trans_per_line;
    /* 调用 printf：输出原有诊断/计数信息；日志字符串保留，打印本身不会恢复异常状态。 */
    printf("total pages: %d\n", spp->tt_line_wps);

    spp->tt_gtd_size = spp->tt_pgs / spp->ents_per_pg;
    spp->tt_cmt_size = spp->tt_pgs * 15 / 1000; /* 论文：总映射数的 1.5% */
    spp->enable_request_prefetch = true;    /* 原作者要求保持请求预取开启；这里只是注释，源码未强制禁止 false */
    spp->enable_select_prefetch = true;

    // 计算 VPPN 各维度的页号步长
    /* 阶段 6：建立 VPPN 按 ch/lun/pl/pg/blk 展平的权重。 */
    spp->chn_per_lun = spp->nchs;
    spp->chn_per_pl = spp->nchs * spp->luns_per_ch;
    spp->chn_per_pg = spp->chn_per_pl * spp->pls_per_lun;
    spp->chn_per_blk = spp->chn_per_pg * spp->pgs_per_blk;

    /* 调用 check_params：保留的几何检查接口。 */
    check_params(spp);
}

/**
 * 功能：给一页分配 sector 状态数组并置 sector/page 为 FREE。
 * 参数 pg：当前 NAND page 状态对象引用或页内索引，取决于声明类型。
 * 参数 spp：ssdparams 的局部引用，读取几何/时延/映射尺寸，不复制设备参数。
 * 作用与边界：初始化元数据状态；不分配或复制用户数据 payload。
 */
static void ssd_init_nand_page(struct nand_page *pg, struct ssdparams *spp)
{
    pg->nsecs = spp->secs_per_pg;
    /* 调用 g_malloc0：按给定字节数分配零初始化内存；分配的是状态/引用结构而不是 NAND payload。 */
    pg->sec = g_malloc0(sizeof(nand_sec_status_t) * pg->nsecs);
    /* 变量 i：当前数组/外层循环下标，范围由紧邻 for 条件确定。 */
    for (int i = 0; i < pg->nsecs; i++) {
        pg->sec[i] = SEC_FREE;
    }
    pg->status = PG_FREE;
}

/**
 * 功能：分配一个 block 的 page 数组，逐页初始化并清零计数。
 * 参数 blk：当前 NAND block 状态对象引用。
 * 参数 spp：ssdparams 的局部引用，读取几何/时延/映射尺寸，不复制设备参数。
 * 作用与边界：erase_cnt 和旧 block.wp 从 0 开始；实际分配使用 group WP。
 */
static void ssd_init_nand_blk(struct nand_block *blk, struct ssdparams *spp)
{
    blk->npgs = spp->pgs_per_blk;
    /* 调用 g_malloc0：按给定字节数分配零初始化内存；分配的是状态/引用结构而不是 NAND payload。 */
    blk->pg = g_malloc0(sizeof(struct nand_page) * blk->npgs);
    /* 变量 i：当前数组/外层循环下标，范围由紧邻 for 条件确定。 */
    for (int i = 0; i < blk->npgs; i++) {
        /* 调用 ssd_init_nand_page：给一页分配 sector 状态数组并置 sector/page 为 FREE。 */
        ssd_init_nand_page(&blk->pg[i], spp);
    }
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt = 0;
    blk->wp = 0;
}

/**
 * 功能：为一个 plane 分配 block 数组并逐 block 初始化。
 * 参数 pl：当前 plane 引用或编号，取决于声明类型。
 * 参数 spp：ssdparams 的局部引用，读取几何/时延/映射尺寸，不复制设备参数。
 * 作用与边界：所有子对象属于 SSD 生命周期。
 */
static void ssd_init_nand_plane(struct nand_plane *pl, struct ssdparams *spp)
{
    pl->nblks = spp->blks_per_pl;
    /* 调用 g_malloc0：按给定字节数分配零初始化内存；分配的是状态/引用结构而不是 NAND payload。 */
    pl->blk = g_malloc0(sizeof(struct nand_block) * pl->nblks);
    /* 变量 i：当前数组/外层循环下标，范围由紧邻 for 条件确定。 */
    for (int i = 0; i < pl->nblks; i++) {
        /* 调用 ssd_init_nand_blk：分配一个 block 的 page 数组，逐页初始化并清零计数。 */
        ssd_init_nand_blk(&pl->blk[i], spp);
    }
}

/**
 * 功能：为一个 LUN 分配 plane 数组，并初始化可用时间及 busy。
 * 参数 lun：当前 LUN 引用或编号，取决于声明类型。
 * 参数 spp：ssdparams 的局部引用，读取几何/时延/映射尺寸，不复制设备参数。
 * 作用与边界：LUN 是 NAND 时序排队单元；busy 在此文件活跃时序路径中未维护。
 */
static void ssd_init_nand_lun(struct nand_lun *lun, struct ssdparams *spp)
{
    lun->npls = spp->pls_per_lun;
    /* 调用 g_malloc0：按给定字节数分配零初始化内存；分配的是状态/引用结构而不是 NAND payload。 */
    lun->pl = g_malloc0(sizeof(struct nand_plane) * lun->npls);
    /* 变量 i：当前数组/外层循环下标，范围由紧邻 for 条件确定。 */
    for (int i = 0; i < lun->npls; i++) {
        /* 调用 ssd_init_nand_plane：为一个 plane 分配 block 数组并逐 block 初始化。 */
        ssd_init_nand_plane(&lun->pl[i], spp);
    }
    lun->next_lun_avail_time = 0;
    lun->busy = false;
}

/**
 * 功能：为一个 channel 分配 LUN 数组并初始化 channel 状态。
 * 参数 ch：当前 channel 引用或编号，取决于声明类型。
 * 参数 spp：ssdparams 的局部引用，读取几何/时延/映射尺寸，不复制设备参数。
 * 作用与边界：channel 数据传输时序分支被 #if 0 禁用，活跃延迟主要更新 LUN。
 */
static void ssd_init_ch(struct ssd_channel *ch, struct ssdparams *spp)
{
    ch->nluns = spp->luns_per_ch;
    /* 调用 g_malloc0：按给定字节数分配零初始化内存；分配的是状态/引用结构而不是 NAND payload。 */
    ch->lun = g_malloc0(sizeof(struct nand_lun) * ch->nluns);
    /* 变量 i：当前数组/外层循环下标，范围由紧邻 for 条件确定。 */
    for (int i = 0; i < ch->nluns; i++) {
        /* 调用 ssd_init_nand_lun：为一个 LUN 分配 plane 数组，并初始化可用时间及 busy。 */
        ssd_init_nand_lun(&ch->lun[i], spp);
    }
    ch->next_ch_avail_time = 0;
    ch->busy = 0;
}

/**
 * 功能：分配完整正向表、GTD、模型数组与预留数组，并设置未映射初值。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 作用与边界：模型只分配一次；未训练的 bitmap 全为零。gtd_usage/valid_lines 当前只初始化。
 */
static void ssd_init_maptbl(struct ssd *ssd)
{
    /* 变量 spp：ssdparams 的局部引用，读取几何/时延/映射尺寸，不复制设备参数。 */
    struct ssdparams *spp = &ssd->sp;

    /* 阶段 1：完整正向映射全部置 UNMAPPED。 */
    /* 调用 g_malloc0：按给定字节数分配零初始化内存；分配的是状态/引用结构而不是 NAND payload。 */
    ssd->maptbl = g_malloc0(sizeof(struct ppa) * spp->tt_pgs);
    /* 变量 i：当前数组/外层循环下标，范围由紧邻 for 条件确定。 */
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->maptbl[i].ppa = UNMAPPED_PPA;
    }

    // 初始化 GTD 与模型等常驻 DRAM 数组
    /* 阶段 2：建立 GTD/模型/预留数组；模型与 GTD 各分配一次。 */
    /* 调用 g_malloc0：按给定字节数分配零初始化内存；分配的是状态/引用结构而不是 NAND payload。 */
    /* 调用 g_malloc0：按给定字节数分配零初始化内存；分配的是状态/引用结构而不是 NAND payload。 */
    ssd->gtd = g_malloc0(sizeof(struct ppa) * spp->tt_trans_pgs);
    /* 调用 g_malloc0：按给定字节数分配零初始化内存；分配的是状态/引用结构而不是 NAND payload。 */
    ssd->lr_nodes = g_malloc0(sizeof(struct lr_node) * spp->tt_trans_pgs);
    /* 调用 g_malloc0：按给定字节数分配零初始化内存；分配的是状态/引用结构而不是 NAND payload。 */
    ssd->gtd_usage = g_malloc0(sizeof(uint64_t) * spp->tt_trans_pgs);

    /* 调用 g_malloc0：按给定字节数分配零初始化内存；分配的是状态/引用结构而不是 NAND payload。 */
    ssd->valid_lines = g_malloc0(sizeof(uint8_t) * spp->tt_lines);
    /* 变量 i：当前数组/外层循环下标，范围由紧邻 for 条件确定。 */
    for (int i = 0; i < spp->blks_per_pl; i++) 
        ssd->valid_lines[i] = 0;

    // 原注释称初始化 CMT；此处实际继续初始化每 TP 的 GTD/模型标志，CMT 在独立函数初始化

    /* 阶段 3：每 TP 置未映射；模型有效性由 bitmap 表示。 */
    /* 变量 i：当前数组/外层循环下标，范围由紧邻 for 条件确定。 */
    for (int i = 0; i < spp->tt_trans_pgs; i++) {
        ssd->gtd[i].ppa = UNMAPPED_PPA;
        // ssd->lr_nodes[i].b = 0;
        // ssd->lr_nodes[i].w = 0;

        // 原预留最小/最大属性说明；此处没有实际维护逻辑

        ssd->gtd_usage[i] = 512;    // 预留的每 TP 映射使用量，初值 512；活跃路径未进一步维护
    }
}

/**
 * 功能：建立固定 CMT 槽池、TP 热度队列及两张哈希表。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 作用与边界：所有槽先为 CLEAN、未映射、非预取，并加入 free_cmt_entry_list；TPnode 在插入时动态创建。
 */
static void ssd_init_cmt(struct ssd *ssd)
{
    /* 变量 spp：ssdparams 的局部引用，读取几何/时延/映射尺寸，不复制设备参数。 */
    struct ssdparams *spp = &ssd->sp;
    /* 变量 cm：CMT 管理器引用，负责槽池、TP 队列与哈希。 */
    struct cmt_mgmt *cm = &ssd->cm;
    /* 变量 cmt_entry：当前缓存映射槽，按 LPN 查找或从 free 池取得。 */
    struct cmt_entry *cmt_entry;
    /* 变量 ht：哈希表引用，包含 LPN entry 桶和 TPnode 桶。 */
    struct hash_table *ht = &cm->ht;

    /* 阶段 1：设置容量计数并创建固定槽池和两层队列头。 */
    cm->tt_entries = spp->tt_cmt_size;
    cm->tt_TPnodes = 0;
    cm->free_cmt_entry_cnt = 0;
    cm->used_cmt_entry_cnt = 0;
    cm->counter = 0;

    /* 调用 g_malloc0：按给定字节数分配零初始化内存；分配的是状态/引用结构而不是 NAND payload。 */
    cm->cmt_entries = g_malloc0(sizeof(struct cmt_entry) * cm->tt_entries);
    /* 调用 QTAILQ_INIT：初始化双向队列头，不创建/释放元素。 */
    QTAILQ_INIT(&cm->free_cmt_entry_list);
    /* 调用 QTAILQ_INIT：初始化双向队列头，不创建/释放元素。 */
    QTAILQ_INIT(&cm->TPnode_list);

    /* 变量 i：当前数组/外层循环下标，范围由紧邻 for 条件确定。 */
    for (int i = 0; i < spp->tt_cmt_size; i++) {
        /* 阶段 2：清理每个槽的映射/脏/预取/哈希状态，加入 free 槽队列。 */
        cmt_entry = &cm->cmt_entries[i];
        cmt_entry->dirty = CLEAN;
        cmt_entry->lpn = INVALID_LPN;
        cmt_entry->ppn = UNMAPPED_PPA;
        /* 清除该槽的预取状态与翻译完成依赖 */
        cmt_entry->prefetch = false;
        cmt_entry->next_avail_time = 0;
        /* 初始化哈希链接或桶头 */
        cmt_entry->next = NULL;

        /* 调用 QTAILQ_INSERT_TAIL：用对象内 entry 链接插到队尾；free 池回收/victim FIFO 均显式维护计数。 */
        QTAILQ_INSERT_TAIL(&cm->free_cmt_entry_list, cmt_entry, entry);
        cm->free_cmt_entry_cnt++;
    }
    /* 调用 ftl_assert：只在 FEMU_DEBUG_FTL 下检查表达式；默认空宏，不求值也不提供错误恢复。 */
    ftl_assert(cm->free_cmt_entry_cnt == cm->tt_entries);

    /* 变量 i：当前数组/外层循环下标，范围由紧邻 for 条件确定。 */
    for (int i = 0; i < CMT_HASH_SIZE; i++) {
        /* 阶段 3：清空 CMT 与 TP 两张哈希表的桶头。 */
        ht->cmt_table[i] = NULL;
    }

    /* 变量 i：当前数组/外层循环下标，范围由紧邻 for 条件确定。 */
    for (int i = 0; i < TP_HASH_SIZE; i++) {
        ht->tp_table[i] = NULL;
    }
}

/**
 * 功能：分配按普通 PPN 索引的反向标识数组，全部设 INVALID_LPN。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 作用与边界：代码存于 DRAM；OOB 只是假设，不是实际闪存 OOB I/O。
 */
static void ssd_init_rmap(struct ssd *ssd)
{
    /* 变量 spp：ssdparams 的局部引用，读取几何/时延/映射尺寸，不复制设备参数。 */
    struct ssdparams *spp = &ssd->sp;

    /* 调用 g_malloc0：按给定字节数分配零初始化内存；分配的是状态/引用结构而不是 NAND payload。 */
    ssd->rmap = g_malloc0(sizeof(uint64_t) * spp->tt_pgs);
    /* 变量 i：当前数组/外层循环下标，范围由紧邻 for 条件确定。 */
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->rmap[i] = INVALID_LPN;
    }
}

/**
 * 功能：清零每 TP 内嵌的 512 位 bitmap。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 作用与边界：按位保存，默认总 bitmap 为 1 MiB。0 表示读 miss 不尝试模型。
 */
static void ssd_init_bitmap(struct ssd *ssd)
{
    for (int i = 0; i < ssd->sp.tt_gtd_size; i++) {
        memset(ssd->lr_nodes[i].bitmap, 0, sizeof(ssd->lr_nodes[i].bitmap));
    }
}

// * 将所有的模型都初始化为y=x
/**
 * 功能：给每 TP 的八段模型设置初始 y=x 和零准确覆盖数。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 作用与边界：默认 key=j*64，从 0 到 448；不核实真实映射，也不置 bitmap=1，不能称已训练成功。
 */
static void ssd_init_all_models(struct ssd *ssd) {
    /* 变量 sp：几何与映射参数的局部引用。 */
    struct ssdparams* sp = &ssd->sp;
    /* 变量 avg_valid_cnt：初始每段 key 的间隔，默认 512/8=64，不是已准确预测页数。 */
    int avg_valid_cnt = sp->ents_per_pg / MAX_INTERVALS;

    /* 变量 i：当前数组/外层循环下标，范围由紧邻 for 条件确定。 */
    for (int i = 0; i < sp->tt_gtd_size; i++) {

        /* 变量 j：当前模型段或循环下标，范围由紧邻 for 条件确定。 */
        for (int j = 0; j < MAX_INTERVALS; j++) {
            /* 变量 brk：当前 lr_breakpoint 引用，指向全局模型的分段参数。 */
            lr_breakpoint* brk = &ssd->lr_nodes[i].brks[j];
            brk->w = 1;
            brk->b = 0;

            // 每段准确覆盖数初始为 0；顺序更新时以 bitmap 统计旧覆盖数
            brk->key = j * avg_valid_cnt;
        }
    }
}

/**
 * 功能：清零显式列出的统计成员。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 作用与边界：其他成员依赖外围对 ssd 的 g_malloc0 初值；候选、尝试、fallback 与回收 line 的定义见头文件。
 */
static void ssd_init_statistics(struct ssd *ssd)
{
    /* 变量 st：统计结构引用；事件口径见 ld-tpftl.h。 */
    struct statistics *st = &ssd->stat;

    st->cmt_hit_cnt = 0;
    st->cmt_miss_cnt = 0;
    st->cmt_hit_ratio = 0;
    st->access_cnt = 0;
    st->model_hit_num = 0;
    st->model_use_num = 0;
    st->model_out_range = 0;
    st->predict_time = 0;
    st->calculate_time = 0;
    st->sort_time = 0;
    st->model_training_nums = 0;
    // st->max_read_lpn = 0;
    // st->min_read_lpn = INVALID_LPN;
    // st->max_write_lpn = 0;
    // st->min_write_lpn = INVALID_LPN;
    st->read_joule = 0;
    st->write_joule = 0;
    st->erase_joule = 0;
    st->joule = 0;
}

/**
 * 功能：建立整个 SSD 状态树、映射、缓存、line/WP/模型，并启动 FTL 工作线程。
 * 参数 n：外围 FemuCtrl 控制器引用；ssd 与请求 ring 的来源。
 * 作用与边界：n->ssd 由 bb_init 先零分配。此函数只初始化原型状态，不恢复已持久化模型；线程由 dataplane_started 门控。
 */
void ssd_init(FemuCtrl *n)
{
    /* 变量 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。 */
    struct ssd *ssd = n->ssd;
    /* 变量 spp：ssdparams 的局部引用，读取几何/时延/映射尺寸，不复制设备参数。 */
    struct ssdparams *spp = &ssd->sp;

    /* 调用 ftl_assert：只在 FEMU_DEBUG_FTL 下检查表达式；默认空宏，不求值也不提供错误恢复。 */
    ftl_assert(ssd);

    /* 阶段 1：配置几何并从 channel 向下递归创建 NAND 元数据树。 */
    /* 调用 ssd_init_params：设置默认 NAND 几何、时延、映射缓存、group 尺寸及 VPPN 步长。 */
    ssd_init_params(spp);

    /* 初始化 SSD 的 channel/LUN/plane/block/page 元数据树 */
    /* 调用 g_malloc0：按给定字节数分配零初始化内存；分配的是状态/引用结构而不是 NAND payload。 */
    ssd->ch = g_malloc0(sizeof(struct ssd_channel) * spp->nchs);
    /* 变量 i：当前数组/外层循环下标，范围由紧邻 for 条件确定。 */
    for (int i = 0; i < spp->nchs; i++) {
        /* 调用 ssd_init_ch：为一个 channel 分配 LUN 数组并初始化 channel 状态。 */
        ssd_init_ch(&ssd->ch[i], spp);
    }

    /* 初始化完整 LPN→PPA 真值表及 GTD/模型数组 */
    /* 阶段 2：建立完整真值、反向标识及逐 LPN 候选标志。 */
    /* 调用 ssd_init_maptbl：分配完整正向表、GTD、模型数组与预留数组，并设置未映射初值。 */
    ssd_init_maptbl(ssd);

    /* 初始化按普通 PPN 索引的反向标识 */
    /* 调用 ssd_init_rmap：分配按普通 PPN 索引的反向标识数组，全部设 INVALID_LPN。 */
    ssd_init_rmap(ssd);

    // 初始化每 TP 的 512 位 bitmap
    /* 调用 ssd_init_bitmap：清零每 TP 内嵌的 512 位 bitmap。 */
    ssd_init_bitmap(ssd);

    /* 初始化全局 line 实体、free 池与 victim 队列 */
    /* 阶段 3：建立全局 line 池、CMT 和 group/trans WP。 */
    /* 调用 ssd_init_lines：分配全部 line 实体、初始化全局队列，并把每条 line 放入 free 池。 */
    ssd_init_lines(ssd);

    // 初始化 CMT 槽池与两层组织
    /* 调用 ssd_init_cmt：建立固定 CMT 槽池、TP 热度队列及两张哈希表。 */
    ssd_init_cmt(ssd);

    /* 初始化数据 group WP 与独立 trans WP，供后续页分配使用 */
    /* 调用 ssd_init_write_pointer：分配数据 group 的 WP 数组、哨兵链和 line→owner 反查表，并先给 trans_wp 分配 line。 */
    ssd_init_write_pointer(ssd);

    /* 阶段 4：初始化统计/模型并启动等待数据面的 FTL 工作线程。 */
    /* 调用 ssd_init_statistics：清零显式列出的统计成员。 */
    ssd_init_statistics(ssd);

    /* 调用 ssd_init_all_models：给每 TP 的八段模型设置初始 y=x 和零准确覆盖数。 */
    ssd_init_all_models(ssd);

    ssd->model_used = true;
    ssd->gc_in_progress = false;

    /* 调用 qemu_thread_create：建立 FTL 工作线程，将控制器 n 作为 arg 传入 ftl_thread；线程等待数据面启动。 */
    qemu_thread_create(&ssd->ftl_thread, "FEMU-FTL-Thread", ftl_thread, n,
                       QEMU_THREAD_JOINABLE);
}

/**
 * 功能：检查 PPA 的六个几何字段是否位于设备范围内。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 参数 ppa：本次物理地址；DATA/TP 及输入输出语义由本函数路径确定。
 * 作用与边界：只检查坐标范围，不检查 PG_VALID、rmap 或 maptbl 一致性。
 */
static inline bool valid_ppa(struct ssd *ssd, struct ppa *ppa)
{
    /* 变量 spp：ssdparams 的局部引用，读取几何/时延/映射尺寸，不复制设备参数。 */
    struct ssdparams *spp = &ssd->sp;
    /* 变量 ch：channel 编号，扫描或 PPA 几何范围检查使用。 */
    int ch = ppa->g.ch;
    /* 变量 lun：channel 内 LUN 编号，扫描或几何检查使用。 */
    int lun = ppa->g.lun;
    /* 变量 pl：plane 编号，当前活跃 line 写主要为 0。 */
    int pl = ppa->g.pl;
    /* 变量 blk：block 编号，默认等于 line id。 */
    int blk = ppa->g.blk;
    /* 变量 pg：block 内页编号，扫描所有页或检查范围使用。 */
    int pg = ppa->g.pg;
    /* 变量 sec：PPA 内的 sector 编号，valid_ppa 用于范围检查。 */
    int sec = ppa->g.sec;

    if (ch >= 0 && ch < spp->nchs && lun >= 0 && lun < spp->luns_per_ch && pl >=
        0 && pl < spp->pls_per_lun && blk >= 0 && blk < spp->blks_per_pl && pg
        >= 0 && pg < spp->pgs_per_blk && sec >= 0 && sec < spp->secs_per_pg)
        return true;

    return false;
}

/**
 * 功能：检查逻辑页号是否小于 sp.tt_pgs。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 参数 lpn：当前逻辑页号，DATA 映射索引。
 * 作用与边界：上界按物理总页数，不等于外围 namespace 实际逻辑容量。
 */
static inline bool valid_lpn(struct ssd *ssd, uint64_t lpn)
{
    return (lpn < ssd->sp.tt_pgs);
}

/**
 * 功能：检查 packed PPA 是否不同于 UNMAPPED_PPA 哨兵。
 * 参数 ppa：本次物理地址；DATA/TP 及输入输出语义由本函数路径确定。
 * 作用与边界：不检查几何范围或页有效状态。
 */
static inline bool mapped_ppa(struct ppa *ppa)
{
    return !(ppa->ppa == UNMAPPED_PPA);
}

/**
 * 功能：按 PPA.ch 返回 channel 状态对象。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 参数 ppa：本次物理地址；DATA/TP 及输入输出语义由本函数路径确定。
 * 作用与边界：数组引用，无范围检查、无 I/O。
 */
static inline struct ssd_channel *get_ch(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->ch[ppa->g.ch]);
}

/**
 * 功能：沿 channel→LUN 返回 PPA 所属 LUN 对象。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 参数 ppa：本次物理地址；DATA/TP 及输入输出语义由本函数路径确定。
 * 作用与边界：返回设备内引用，不分配新 LUN。
 */
static inline struct nand_lun *get_lun(struct ssd *ssd, struct ppa *ppa)
{
    /* 变量 ch：当前 channel 引用或编号，取决于声明类型。 */
    /* 调用 get_ch：按 PPA.ch 返回 channel 状态对象。 */
    struct ssd_channel *ch = get_ch(ssd, ppa);
    return &(ch->lun[ppa->g.lun]);
}

/**
 * 功能：沿 PPA 的 channel/LUN/plane 返回 plane 对象。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 参数 ppa：本次物理地址；DATA/TP 及输入输出语义由本函数路径确定。
 * 作用与边界：只是状态树寻址。
 */
static inline struct nand_plane *get_pl(struct ssd *ssd, struct ppa *ppa)
{
    /* 变量 lun：当前 LUN 引用或编号，取决于声明类型。 */
    /* 调用 get_lun：沿 channel→LUN 返回 PPA 所属 LUN 对象。 */
    struct nand_lun *lun = get_lun(ssd, ppa);
    return &(lun->pl[ppa->g.pl]);
}

/**
 * 功能：沿 PPA 的 channel/LUN/plane/block 返回 block 对象。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 参数 ppa：本次物理地址；DATA/TP 及输入输出语义由本函数路径确定。
 * 作用与边界：只是状态树寻址；block 是基本擦除单元。
 */
static inline struct nand_block *get_blk(struct ssd *ssd, struct ppa *ppa)
{
    /* 变量 pl：当前 plane 引用或编号，取决于声明类型。 */
    /* 调用 get_pl：沿 PPA 的 channel/LUN/plane 返回 plane 对象。 */
    struct nand_plane *pl = get_pl(ssd, ppa);
    return &(pl->blk[ppa->g.blk]);
}

/**
 * 功能：使用 PPA.blk 返回全局 line 实体。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 参数 ppa：本次物理地址；DATA/TP 及输入输出语义由本函数路径确定。
 * 作用与边界：忽略 ch/lun；默认相同 block id 的跨 LUN blocks 构成一条 line。
 */
static inline struct line *get_line(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->lm.lines[ppa->g.blk]);
}

/**
 * 功能：取得 PPA 对应 block 内的 page 状态对象。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 参数 ppa：本次物理地址；DATA/TP 及输入输出语义由本函数路径确定。
 * 作用与边界：不访问用户数据后端。
 */
static inline struct nand_page *get_pg(struct ssd *ssd, struct ppa *ppa)
{
    /* 变量 blk：当前 NAND block 状态对象引用。 */
    /* 调用 get_blk：沿 PPA 的 channel/LUN/plane/block 返回 block 对象。 */
    struct nand_block *blk = get_blk(ssd, ppa);
    return &(blk->pg[ppa->g.pg]);
}

/**
 * 功能：按 LPN 所属翻译页索引读取 GTD 中该 TP 的 PPA。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 参数 lpn：当前逻辑页号，DATA 映射索引。
 * 作用与边界：内部除 ents_per_pg；与接收已计算索引的 get_gtd_ent_index 不同。
 */
static inline struct ppa get_gtd_ent(struct ssd *ssd, uint64_t lpn) {
    /* 变量 spp：ssdparams 的局部引用，读取几何/时延/映射尺寸，不复制设备参数。 */
    struct ssdparams *spp = &ssd->sp;
    /* 变量 gtd_index：逻辑页所属 TP 索引，通常为 LPN/ents_per_pg。 */
    int gtd_index = lpn / spp->ents_per_pg;
    return ssd->gtd[gtd_index];
}

/**
 * 功能：按已经计算好的 TP 索引直接读取 GTD。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 参数 index：调用者已算好的 TP 索引，不再除 ents_per_pg。
 * 作用与边界：不再除 ents_per_pg，不读 NAND。
 */
static inline struct ppa get_gtd_ent_index(struct ssd *ssd, uint64_t index) {
    return ssd->gtd[index];
}

/**
 * 功能：将新 Translation Page 的 PPA 保存到指定 GTD 索引。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 参数 gtd_ppa：新 TP 的 PPA 输入指针，按值复制入 gtd。
 * 参数 index：已经计算好的 TP 索引。
 * 作用与边界：参数 index 已是 TP 索引；不维护 rmap、页状态或 payload。
 */
static inline void set_gtd_ent(struct ssd *ssd, struct ppa *gtd_ppa, uint64_t index) {

    ssd->gtd[index] = *gtd_ppa;
}

/**
 * 功能：模拟一个 NAND 读/写/擦命令的 LUN 排队，更新完成时间、写计数与估算能耗。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 参数 ppa：本次物理地址；DATA/TP 及输入输出语义由本函数路径确定。
 * 参数 ncmd：NAND 命令描述，含 USER/GC 类型、读写擦操作和起点。
 * 作用与边界：开始=max(命令起点,LUN 可用时间)，返回从命令起点到完成的 ns，包含等待。stime=0 时取当前 QEMU 时钟；channel 分支被禁用，USER/GC 写基础时延相同。
 */
static uint64_t ssd_advance_status(struct ssd *ssd, struct ppa *ppa, struct
        nand_cmd *ncmd)
{
    /* 阶段 1：选择命令起点并定位 LUN；lat 返回从命令起点到完成的 ns。 */
    /* 变量 c：NAND 命令操作码；诊断 func 中则为整数指针。 */
    int c = ncmd->cmd;
    /* 变量 cmd_stime：NAND 命令起点，ns；stime=0 时使用当前 QEMU REALTIME 时钟。 */
    uint64_t cmd_stime = (ncmd->stime == 0) ? \
        /* 调用 qemu_clock_get_ns：取得 QEMU REALTIME 时钟的 ns 值；用于 stime=0 的 NAND 命令起点。 */
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : ncmd->stime;
    /* 变量 nand_stime：在 LUN 排队后可实际开始的 ns 时间，取可用时间与命令起点最大值。 */
    uint64_t nand_stime;
    /* 变量 spp：ssdparams 的局部引用，读取几何/时延/映射尺寸，不复制设备参数。 */
    struct ssdparams *spp = &ssd->sp;
    /* 变量 lun：当前 LUN 引用或编号，取决于声明类型。 */
    /* 调用 get_lun：沿 channel→LUN 返回 PPA 所属 LUN 对象。 */
    struct nand_lun *lun = get_lun(ssd, ppa);
    /* 变量 lat：本次模拟完成延迟，ns，包含 LUN 排队等待。 */
    uint64_t lat = 0;
    // bool flag=false;
    // if (ppa->g.ch==0&&ppa->g.lun==0)
    //     flag=true;

    /* 阶段 2：按操作码串行更新该 LUN 的 next_lun_avail_time。 */
    switch (c) {
    case NAND_READ:
        /* 读：先按 LUN 可用时间执行 NAND 读 */
        /* 读分支：先等待 LUN 可用，再增加基础读时延；channel 传输草案被禁用。 */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;
        lat = lun->next_lun_avail_time - cmd_stime;
        ssd->stat.read_joule += 3.5;
#if 0
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;

        /* 禁用草案：读完 NAND 后再计 channel 数据传输 */
        chnl_stime = (ch->next_ch_avail_time < lun->next_lun_avail_time) ? \
            lun->next_lun_avail_time : ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        lat = ch->next_ch_avail_time - cmd_stime;
#endif
        break;

    case NAND_WRITE:
        /* 写分支：每个实际模拟 WRITE 加 write_num，USER/GC 基础时延目前相同。 */
        ssd->stat.write_num++;
        /* 原注释拟先传 channel；当前活跃代码仅做 LUN 编程排队，channel 草案在 #if 0 内 */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        if (ncmd->type == USER_IO) {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        } else {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        }
        lat = lun->next_lun_avail_time - cmd_stime;
        ssd->stat.write_joule += 16.7;

#if 0
        chnl_stime = (ch->next_ch_avail_time < cmd_stime) ? cmd_stime : \
                     ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        /* 禁用草案：channel 传输后再执行 NAND 编程 */
        nand_stime = (lun->next_lun_avail_time < ch->next_ch_avail_time) ? \
            ch->next_ch_avail_time : lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
#endif
        break;

    case NAND_ERASE:
        /* 擦：只在此更新 NAND 时序，页状态另由 mark_block_free 清理 */
        /* 擦分支：增加基础擦时延与估算擦能耗；页状态清理由别的函数完成。 */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->blk_er_lat;
        // if (flag) {
        //         fprintf(gc_fp, "%ld\n",lun->next_lun_avail_time);
        //         flag = true;
        // }

        lat = lun->next_lun_avail_time - cmd_stime;
        ssd->stat.erase_joule += 132;
        break;

    default:
        /* 调用 ftl_err：通过错误日志宏输出到 stderr；宏本身不终止函数或修复状态。 */
        ftl_err("Unsupported NAND command: 0x%x\n", c);
    }

    return lat;
}

/* 更新旧页状态；原注释误写 VALID→VALID，实际为 PG_VALID→PG_INVALID */
/**
 * 功能：把有效页改为 INVALID，同时 block/line 的 ipc++、vpc--。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 参数 ppa：本次物理地址；DATA/TP 及输入输出语义由本函数路径确定。
 * 作用与边界：不返还 rest、不改映射/bitmap。默认断言为空；递减后 line.vpc>0 的原断言会拒绝最后一个有效页失效，原样保留。
 */
static void mark_page_invalid(struct ssd *ssd, struct ppa *ppa)
{
    /* 变量 spp：ssdparams 的局部引用，读取几何/时延/映射尺寸，不复制设备参数。 */
    struct ssdparams *spp = &ssd->sp;
    /* 变量 blk：当前 NAND block 状态对象引用。 */
    struct nand_block *blk = NULL;
    /* 变量 pg：当前 NAND page 状态对象引用或页内索引，取决于声明类型。 */
    struct nand_page *pg = NULL;
    /* 变量 line：设备内的跨 LUN line 实体引用，id 对应 block id。 */
    struct line *line;

    /* 更新该物理页状态 */
    /* 调用 get_pg：取得 PPA 对应 block 内的 page 状态对象。 */
    pg = get_pg(ssd, ppa);
    /* 调用 ftl_assert：只在 FEMU_DEBUG_FTL 下检查表达式；默认空宏，不求值也不提供错误恢复。 */
    ftl_assert(pg->status == PG_VALID);
    pg->status = PG_INVALID;

    /* 更新所属 block 的页计数 */
    /* 调用 get_blk：沿 PPA 的 channel/LUN/plane/block 返回 block 对象。 */
    blk = get_blk(ssd, ppa);
    /* 调用 ftl_assert：只在 FEMU_DEBUG_FTL 下检查表达式；默认空宏，不求值也不提供错误恢复。 */
    ftl_assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
    blk->ipc++;
    /* 调用 ftl_assert：只在 FEMU_DEBUG_FTL 下检查表达式；默认空宏，不求值也不提供错误恢复。 */
    ftl_assert(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
    blk->vpc--;

    /* 更新跨 LUN line 的页计数 */
    /* 调用 get_line：使用 PPA.blk 返回全局 line 实体。 */
    line = get_line(ssd, ppa);
    /* 调用 ftl_assert：只在 FEMU_DEBUG_FTL 下检查表达式；默认空宏，不求值也不提供错误恢复。 */
    ftl_assert(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
    if (line->vpc == spp->pgs_per_line) {
        /* 调用 ftl_assert：只在 FEMU_DEBUG_FTL 下检查表达式；默认空宏，不求值也不提供错误恢复。 */
        ftl_assert(line->ipc == 0);
        // was_full_line = true;
    }
    line->ipc++;
    line->vpc--;
    if (line->type == DATA) {
        uint64_t lpn = get_rmap_ent(ssd, ppa);
        if (!valid_lpn(ssd, lpn)) {
            ftl_err("Invalid OOB LPN while invalidating data page\n");
            abort();
        }
        ssd->gtd_wps[lpn / spp->ents_per_pg / spp->trans_per_line].invalid_pages++;
    }
    /* 调用 ftl_assert：只在 FEMU_DEBUG_FTL 下检查表达式；默认空宏，不求值也不提供错误恢复。 */
    ftl_assert(line->vpc >= 0 && line->vpc <= spp->pgs_per_line);
    /* 旧草案：覆盖写后调整 victim 优先队列位置；下面队列更新代码已注释 */
    // if (line->pos) {
    //     旧草案说明：此优先队列调用可能更新 line.vpc；当前不执行
    //     pqueue_change_priority(lm->victim_line_pq, line->vpc - 1, line);
    // } else {
    //     line->vpc--;
    // }

    // if (was_full_line) {
        /* 旧草案：把 line 从 full 移到 victim；相关操作被注释 */
        // QTAILQ_REMOVE(&lm->full_line_list, line, entry);
        // lm->full_line_cnt--;
        // pqueue_insert(lm->victim_line_pq, line);
        // lm->victim_line_cnt++;
    // }
}

/**
 * 功能：把已分配 FREE 页提交为 VALID，并使 block/line.vpc 各加 1。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 参数 ppa：本次物理地址；DATA/TP 及输入输出语义由本函数路径确定。
 * 作用与边界：不改变 rest、ipc、maptbl、rmap 或 bitmap；容量断言仅在调试模式生效。
 */
static void mark_page_valid(struct ssd *ssd, struct ppa *ppa)
{
    /* 变量 blk：当前 NAND block 状态对象引用。 */
    struct nand_block *blk = NULL;
    /* 变量 pg：当前 NAND page 状态对象引用或页内索引，取决于声明类型。 */
    struct nand_page *pg = NULL;
    /* 变量 line：设备内的跨 LUN line 实体引用，id 对应 block id。 */
    struct line *line;

    /* 将新物理页状态改为有效 */
    /* 调用 get_pg：取得 PPA 对应 block 内的 page 状态对象。 */
    pg = get_pg(ssd, ppa);
    /* 调用 ftl_assert：只在 FEMU_DEBUG_FTL 下检查表达式；默认空宏，不求值也不提供错误恢复。 */
    ftl_assert(pg->status == PG_FREE);
    pg->status = PG_VALID;

    /* 更新所属 block 的页计数 */
    /* 调用 get_blk：沿 PPA 的 channel/LUN/plane/block 返回 block 对象。 */
    blk = get_blk(ssd, ppa);
    /* 调用 ftl_assert：只在 FEMU_DEBUG_FTL 下检查表达式；默认空宏，不求值也不提供错误恢复。 */
    ftl_assert(blk->vpc >= 0 && blk->vpc < ssd->sp.pgs_per_blk);
    blk->vpc++;

    /* 更新跨 LUN line 的页计数 */
    /* 调用 get_line：使用 PPA.blk 返回全局 line 实体。 */
    line = get_line(ssd, ppa);
    /* 调用 ftl_assert：只在 FEMU_DEBUG_FTL 下检查表达式；默认空宏，不求值也不提供错误恢复。 */
    ftl_assert(line->vpc >= 0 && line->vpc < ssd->sp.pgs_per_line);
    line->vpc++;
}

/**
 * 功能：重置 block 内所有 page.status 与 block 有效/无效计数，erase_cnt++。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 参数 ppa：本次物理地址；DATA/TP 及输入输出语义由本函数路径确定。
 * 作用与边界：不清 sector 数组、rmap、line 队列，也不在本函数模拟 NAND_ERASE；时序由 caller 另外调用。
 */
static void mark_block_free(struct ssd *ssd, struct ppa *ppa)
{
    /* 变量 spp：ssdparams 的局部引用，读取几何/时延/映射尺寸，不复制设备参数。 */
    struct ssdparams *spp = &ssd->sp;
    /* 变量 blk：当前 NAND block 状态对象引用。 */
    /* 调用 get_blk：沿 PPA 的 channel/LUN/plane/block 返回 block 对象。 */
    struct nand_block *blk = get_blk(ssd, ppa);
    /* 变量 pg：当前 NAND page 状态对象引用或页内索引，取决于声明类型。 */
    struct nand_page *pg = NULL;

    /* 变量 i：当前数组/外层循环下标，范围由紧邻 for 条件确定。 */
    for (int i = 0; i < spp->pgs_per_blk; i++) {
        /* 重置每页 status 为 FREE；未重置 sector 状态数组 */
        pg = &blk->pg[i];
        /* 调用 ftl_assert：只在 FEMU_DEBUG_FTL 下检查表达式；默认空宏，不求值也不提供错误恢复。 */
        ftl_assert(pg->nsecs == spp->secs_per_pg);
        pg->status = PG_FREE;
    }

    /* 清有效/无效计数并增加 block.erase_cnt */
    /* 调用 ftl_assert：只在 FEMU_DEBUG_FTL 下检查表达式；默认空宏，不求值也不提供错误恢复。 */
    ftl_assert(blk->npgs == spp->pgs_per_blk);
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt++;
}

/**
 * 功能：查询 LPN 缓存项并返回 entry 或 NULL，保持两层热度位置不动。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 参数 lpn：当前逻辑页号，DATA 映射索引。
 * 作用与边界：当前使用哈希；保留的旧遍历草案不执行。
 */
static struct cmt_entry *cmt_hit_no_move(struct ssd *ssd, uint64_t lpn)
{
    // struct ssdparams *spp = &ssd->sp;
    // struct cmt_mgmt *cm = &ssd->cm;
    // uint64_t tvpn = lpn / spp->ents_per_pg;
    // struct TPnode *curTP = NULL;
    /* 变量 cmt_entry：当前缓存映射槽，按 LPN 查找或从 free 池取得。 */
    struct cmt_entry *cmt_entry = NULL;
    /* 变量 ht：哈希表引用，包含 LPN entry 桶和 TPnode 桶。 */
    struct hash_table *ht = &ssd->cm.ht;

    /* 调用 find_hash_entry：按 LPN 遍历 CMT 哈希桶链，寻找已有缓存项。 */
    cmt_entry = find_hash_entry(ht, lpn);

    // QTAILQ_FOREACH(curTP, &cm->TPnode_list, entry) {
    //     if (curTP->tvpn == tvpn) {
    //         QTAILQ_FOREACH(cmt_entry, &curTP->cmt_entry_list, entry) {
    //             if (cmt_entry->lpn == lpn) break;
    //         }
    //         break;
    //     }
    // }

    // curTP = QTAILQ_FIRST(&cm->TPnode_list);
    // if (curTP->tvpn == tvpn) {
    //     QTAILQ_FOREACH(cmt_entry, &curTP->cmt_entry_list, entry) {
    //         if (cmt_entry->lpn == lpn) break;
    //     }
    // } else {
    //     printf("error in cmt_hit_no_move! TPnode not in the first\n");
    // }

    return cmt_entry;
}

/**
 * 功能：为传入 LPN 所属 TP 分配 trans_wp 元数据页，更新 GTD/rmap/VALID 后写后推进。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 参数 tvpn：实际传入 LPN，不是已整除的 TP 索引。
 * 作用与边界：参数 tvpn 实际为 LPN，内部再除 ents_per_pg；返回固定 0。NAND_WRITE 计入写时延、write_num 与能耗估算。
 */
static uint64_t translation_write_page(struct ssd *ssd, uint64_t tvpn)
{
    /* 变量 spp：ssdparams 的局部引用，读取几何/时延/映射尺寸，不复制设备参数。 */
    struct ssdparams *spp = &ssd->sp;
    /* 变量 new_gtd_ppa：新分配 Translation Page 的物理地址。 */
    /* 调用 get_new_line_page：把 WP 当前坐标打包成 PPA，并消耗 curline 的一个剩余页槽。 */
    struct ppa new_gtd_ppa = get_new_line_page(ssd, &ssd->trans_wp);

    // 更新 GTD 中这一 Translation Page 的新 PPA
    /* 变量 index：tvpn/ents_per_pg 得到的真正 TP 索引。 */
    uint64_t index = tvpn / spp->ents_per_pg;
    /* 调用 set_gtd_ent：将新 Translation Page 的 PPA 保存到指定 GTD 索引。 */
    set_gtd_ent(ssd, &new_gtd_ppa, index);
    /* 调用 set_rmap_ent：由 PPA 得到 PPN，将反向标识写入 rmap。 */
    set_rmap_ent(ssd, index, &new_gtd_ppa);
    /* 调用 mark_page_valid：把已分配 FREE 页提交为 VALID，并使 block/line.vpc 各加 1。 */
    mark_page_valid(ssd, &new_gtd_ppa);
    if (!ssd->gc_in_progress || spp->enable_gc_delay) {
        uint64_t ready = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        uint64_t first = index * spp->ents_per_pg;
        for (uint64_t lpn = first; lpn < first + spp->ents_per_pg && lpn < (uint64_t)spp->tt_pgs; lpn++) {
            struct ppa data = get_maptbl_ent(ssd, lpn);
            if (mapped_ppa(&data)) {
                uint64_t end = get_lun(ssd, &data)->next_lun_avail_time;
                if (end > ready) ready = end;
            }
        }
        struct nand_cmd cmd = { .type = ssd->gc_in_progress ? GC_IO : USER_IO,
                                .cmd = NAND_WRITE, .stime = ready };
        ssd_advance_status(ssd, &new_gtd_ppa, &cmd);
    }

    /* 调用 advance_line_write_pointer：把 line 写坐标按 channel→lun→page 推进；写满后封存 line 并调度 GC/分配。 */
    advance_line_write_pointer(ssd, &ssd->trans_wp);

    return 0;
}


/**
 * 功能：构造 USER_IO NAND_READ，模拟一次不绑定 Host 请求的翻译页读取。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 参数 ppa：本次物理地址；DATA/TP 及输入输出语义由本函数路径确定。
 * 作用与边界：stime 固定 0，采用当前 QEMU 时钟；返回 ns 延迟并更新 TP 所在 LUN 可用时间。
 */
static inline uint64_t translation_read_page_no_req(struct ssd *ssd, struct ppa *ppa)
{
    /* 变量 lat：本次模拟完成延迟，ns，包含 LUN 排队等待。 */
    uint64_t lat = 0;
    /* 变量 trd：用于模拟翻译页 NAND_READ 的命令；stime 设 0。 */
    struct nand_cmd trd;
    trd.type = USER_IO;
    trd.cmd = NAND_READ;
    trd.stime = 0;
    /* 调用 ssd_advance_status：模拟一个 NAND 读/写/擦命令的 LUN 排队，更新完成时间、写计数与估算能耗。 */
    lat = ssd_advance_status(ssd, ppa, &trd);
    
    return lat;
}

/**
 * 功能：构造 USER_IO NAND_READ，模拟一次翻译页读取并返回 ns 延迟。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 参数 req：翻译页读取以 req.stime 为起点。
 * 参数 ppa：本次物理地址；DATA/TP 及输入输出语义由本函数路径确定。
 * 作用与边界：stime 使用 req.stime。完成时间由插入缓存项后建立数据地址依赖。
 */
static uint64_t translation_read_page(struct ssd *ssd, NvmeRequest *req, struct ppa *ppa)
{
    /* 变量 lat：本次模拟完成延迟，ns，包含 LUN 排队等待。 */
    uint64_t lat = 0;
    /* 变量 trd：用于模拟翻译页 NAND_READ 的命令；stime 设 0。 */
    struct nand_cmd trd;
    trd.type = USER_IO;
    trd.cmd = NAND_READ;
    trd.stime = req->stime;
    /* 调用 ssd_advance_status：模拟一个 NAND 读/写/擦命令的 LUN 排队，更新完成时间、写计数与估算能耗。 */
    lat = ssd_advance_status(ssd, ppa, &trd);
    
    return lat;
}


/**
 * 功能：查询 LPN 与所属 TP，并把命中的 TPnode/entry 移到各自热度队列头。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 参数 lpn：当前逻辑页号，DATA 映射索引。
 * 作用与边界：entry miss 也可提升已有 TPnode 热度；entry 存在时假定其 TPnode 同时存在。最终读地址仍由 maptbl 取得。
 */
static struct cmt_entry *cmt_hit(struct ssd *ssd, uint64_t lpn)
{
    /* 变量 spp：ssdparams 的局部引用，读取几何/时延/映射尺寸，不复制设备参数。 */
    struct ssdparams *spp = &ssd->sp;
    /* 变量 cm：CMT 管理器引用，负责槽池、TP 队列与哈希。 */
    struct cmt_mgmt *cm = &ssd->cm;
    /* 变量 tvpn：翻译页逻辑索引，通常由 LPN/ents_per_pg 得到；特殊调用见本函数说明。 */
    uint64_t tvpn = lpn / spp->ents_per_pg;
    /* 变量 curTP：当前 LPN 所属的 TP 缓存节点。 */
    struct TPnode *curTP = NULL;
    /* 变量 cmt_entry：当前缓存映射槽，按 LPN 查找或从 free 池取得。 */
    struct cmt_entry *cmt_entry = NULL;
    /* 变量 ht：哈希表引用，包含 LPN entry 桶和 TPnode 桶。 */
    struct hash_table *ht = &cm->ht;

    /* 调用 find_hash_tpnode：按翻译页索引遍历 TP 哈希桶链。 */
    curTP = find_hash_tpnode(ht, tvpn);
    /* 调用 find_hash_entry：按 LPN 遍历 CMT 哈希桶链，寻找已有缓存项。 */
    cmt_entry = find_hash_entry(ht, lpn);
    if (curTP != NULL) {
        /* 调用 QTAILQ_REMOVE：按队列头、节点、链接字段 entry 摘除对象；不释放对象，也不改业务计数。 */
        QTAILQ_REMOVE(&cm->TPnode_list, curTP, entry);
        /* 调用 QTAILQ_INSERT_HEAD：用对象内 entry 链接插到队头；表示热度时为最热，新 ownership 使用另一条单链。 */
        QTAILQ_INSERT_HEAD(&cm->TPnode_list, curTP, entry);
    }
    if (cmt_entry != NULL) {
        /* 调用 QTAILQ_REMOVE：按队列头、节点、链接字段 entry 摘除对象；不释放对象，也不改业务计数。 */
        QTAILQ_REMOVE(&curTP->cmt_entry_list, cmt_entry, entry);
        /* 调用 QTAILQ_INSERT_HEAD：用对象内 entry 链接插到队头；表示热度时为最热，新 ownership 使用另一条单链。 */
        QTAILQ_INSERT_HEAD(&curTP->cmt_entry_list, cmt_entry, entry);
    }

    return cmt_entry;
}

/**
 * 功能：取一个 free 槽，填映射/预取信息并加入 TPnode 队列与哈希。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 参数 lpn：当前逻辑页号，DATA 映射索引。
 * 参数 ppn：当前数据 PPA 展平后的普通物理页号，不是 VPPN。
 * 参数 pos：entry 在 TPnode 热度队列的插入位置，HEAD 为头、TAIL 为尾。
 * 参数 prefetch：缓存项是否作为预取加载；读 hit 时据此建立翻译完成依赖。
 * 参数 next_avail_time：翻译页 READ 完成的 ns 时间，预取 entry 用它等待地址就绪。
 * 作用与边界：入口要求 free 槽；从哈希表复用对应 TPnode。插入为 CLEAN；必要时创建 TPnode，累计 3 次创建关闭选择预取。空槽不足立即终止，避免空解引用。
 */
static void insert_entry_to_cmt(struct ssd *ssd, uint64_t lpn, uint64_t ppn, int pos, bool prefetch, uint64_t next_avail_time)
{
    /* 变量 spp：ssdparams 的局部引用，读取几何/时延/映射尺寸，不复制设备参数。 */
    struct ssdparams *spp = &ssd->sp;
    /* 变量 cm：CMT 管理器引用，负责槽池、TP 队列与哈希。 */
    struct cmt_mgmt *cm = &ssd->cm;
    /* 变量 ht：哈希表引用，包含 LPN entry 桶和 TPnode 桶。 */
    struct hash_table *ht = &cm->ht;
    /* 变量 tpnode：当前 TP 缓存组织节点；一个 TP 不等于一个 allocation group。 */
    struct TPnode *tpnode;
    /* 变量 cmt_entry：当前缓存映射槽，按 LPN 查找或从 free 池取得。 */
    struct cmt_entry *cmt_entry;
    /* 变量 tvpn：当前 LPN/ents_per_pg 得到的 TP 索引。 */
    uint64_t tvpn = lpn / spp->ents_per_pg;

    /* 调用 QTAILQ_FIRST：取得双向队列头元素或 NULL，不摘链。 */
    cmt_entry = QTAILQ_FIRST(&cm->free_cmt_entry_list);
    /* 阶段 1：从 free 槽队列取一个 entry，填映射与预取依赖，初始 CLEAN。 */
    if (cmt_entry == NULL) {
        /* 调用 ftl_err：通过错误日志宏输出到 stderr；宏本身不终止函数或修复状态。 */
        ftl_err("no cmt_entry in free cmt entry list");
        abort();
    }
    /* 调用 QTAILQ_REMOVE：按队列头、节点、链接字段 entry 摘除对象；不释放对象，也不改业务计数。 */
    QTAILQ_REMOVE(&cm->free_cmt_entry_list, cmt_entry, entry);
    cm->free_cmt_entry_cnt--;
    cmt_entry->lpn = lpn;
    struct ppa current = get_maptbl_ent(ssd, lpn);
    cmt_entry->ppn = mapped_ppa(&current) ? ppa2pgidx(ssd, &current) : UNMAPPED_PPA;
    (void)ppn;
    cmt_entry->dirty = CLEAN;
    cmt_entry->prefetch = prefetch;
    cmt_entry->next_avail_time = next_avail_time;
    cmt_entry->next = NULL;

    //旧遍历草案：查找所属 TPnode，代码被注释
    // QTAILQ_FOREACH(tpnode, &cm->TPnode_list, entry) {
    //     if (tpnode->tvpn == tvpn) {
    //         按 HEAD/TAIL 插入 TPnode 的 entry 热度队列
    //         if (pos == HEAD) {
    //             QTAILQ_INSERT_HEAD(&tpnode->cmt_entry_list, cmt_entry, entry);
    //         } else {
    //             QTAILQ_INSERT_TAIL(&tpnode->cmt_entry_list, cmt_entry, entry);
    //         }
    //         tpnode->cmt_entry_cnt++;
    //         break;
    //     }
    // }
    /* 依赖 caller 已把对应 TPnode 调到全局热度队列头 */
    /* 阶段 2：调用前已处理 TP 热度；按 TP 哈希查找现有节点。 */
    /* 调用 QTAILQ_FIRST：取得双向队列头元素或 NULL，不摘链。 */
    tpnode = find_hash_tpnode(ht, tvpn);
    if (tpnode == NULL) {
        //创建 TPnode 并插到全局 TP 热度队列头
        /* 阶段 2.1：没有对应队头时新建 TPnode，同时建立其队列和 TP 哈希。 */
        /* 调用 g_malloc0：按给定字节数分配零初始化内存；分配的是状态/引用结构而不是 NAND payload。 */
        tpnode = g_malloc0(sizeof(struct TPnode));
        tpnode->tvpn = tvpn;
        tpnode->cmt_entry_cnt = 1;
        tpnode->next = NULL;

        /* 调用 QTAILQ_INIT：初始化双向队列头，不创建/释放元素。 */
        QTAILQ_INIT(&tpnode->cmt_entry_list);
        /* 调用 QTAILQ_INSERT_HEAD：用对象内 entry 链接插到队头；表示热度时为最热，新 ownership 使用另一条单链。 */
        QTAILQ_INSERT_HEAD(&tpnode->cmt_entry_list, cmt_entry, entry);
        // if (pos == HEAD)
        /* 调用 QTAILQ_INSERT_HEAD：用对象内 entry 链接插到队头；表示热度时为最热，新 ownership 使用另一条单链。 */
        QTAILQ_INSERT_HEAD(&cm->TPnode_list, tpnode, entry);
        // else
        //     QTAILQ_INSERT_TAIL(&cm->TPnode_list, tpnode, entry);

        /* 调用 insert_tp_hashtable：将 TPnode 插到所属 TP 哈希桶的链头。 */
        insert_tp_hashtable(ht, tpnode);

        /* 阶段 2.2：累计 3 次创建会关闭选择预取；此 counter 不是访问页数。 */
        cm->tt_TPnodes++;
        cm->counter++;
        if (cm->counter == 3) {
            cm->counter = 0;
            spp->enable_select_prefetch = false;
        }
    } else {
        //按 HEAD/TAIL 插入 TPnode 的 entry 热度队列
        /* 阶段 2.3：已有 TPnode 则按 HEAD/TAIL 插入对应 entry 热度队列。 */
        if (pos == HEAD) {
            /* 调用 QTAILQ_INSERT_HEAD：用对象内 entry 链接插到队头；表示热度时为最热，新 ownership 使用另一条单链。 */
            QTAILQ_INSERT_HEAD(&tpnode->cmt_entry_list, cmt_entry, entry);
        } else {
            /* 调用 QTAILQ_INSERT_TAIL：用对象内 entry 链接插到队尾；free 池回收/victim FIFO 均显式维护计数。 */
            QTAILQ_INSERT_TAIL(&tpnode->cmt_entry_list, cmt_entry, entry);
        }
        tpnode->cmt_entry_cnt++;
    }
    //旧草案：已有 TPnode 时更新热度；下面重复分支被注释
    // if (tpnode != NULL) {
    //     if (pos == HEAD && tpnode != QTAILQ_FIRST(&cm->TPnode_list)) {
    //         QTAILQ_REMOVE(&cm->TPnode_list, tpnode, entry);
    //         QTAILQ_INSERT_HEAD(&cm->TPnode_list, tpnode, entry);
    //     }
    // } else {
    //     创建 TPnode 并插到全局 TP 热度队列头
    //     tpnode = g_malloc0(sizeof(struct TPnode));
    //     tpnode->tvpn = tvpn;
    //     tpnode->cmt_entry_cnt = 1;
    //     QTAILQ_INIT(&tpnode->cmt_entry_list);
    //     QTAILQ_INSERT_HEAD(&tpnode->cmt_entry_list, cmt_entry, entry);
    //     // if (pos == HEAD)
    //     QTAILQ_INSERT_HEAD(&cm->TPnode_list, tpnode, entry);
    //     // else
    //     //     QTAILQ_INSERT_TAIL(&cm->TPnode_list, tpnode, entry);
    //     cm->tt_TPnodes++;
    //     cm->counter++;
    //     if (cm->counter == 3) {
    //         cm->counter = 0;
    //         spp->enable_select_prefetch = false;
    //     }
    // }
    /* 阶段 3：同步存在标记、used 数量与 LPN 哈希；三个容器引用同一槽。 */
    tpnode->exist_ent[lpn % spp->ents_per_pg] = 1;
    cm->used_cmt_entry_cnt++;
    /* 调用 insert_cmt_hashtable：将缓存映射 entry 插到 LPN 哈希桶链头。 */
    insert_cmt_hashtable(ht, cmt_entry);
}


/**
 * 功能：从最冷 TP 优先淘汰冷 CLEAN，缺 CLEAN 才淘汰 DIRTY；脏项聚合更新 TP。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 作用与边界：返回 1 表示 TPnode 也被删除，0 表示只腾出 entry；不是失败码。旧 TP 读可增加 NAND_READ；翻译页读写均参与时序。剩余同 TP DIRTY 全改 CLEAN。
 */
static int evict_entry_from_cmt(struct ssd *ssd)
{
    /* 变量 spp：ssdparams 的局部引用，读取几何/时延/映射尺寸，不复制设备参数。 */
    struct ssdparams *spp = &ssd->sp;
    /* 变量 cm：CMT 管理器引用，负责槽池、TP 队列与哈希。 */
    struct cmt_mgmt *cm = &ssd->cm;
    /* 变量 tpnode：当前 TP 缓存组织节点；一个 TP 不等于一个 allocation group。 */
    struct TPnode *tpnode;
    /* 变量：
     * cmt_entry：当前缓存映射槽，按 LPN 查找或从 free 池取得。
     * tmp_entry：缓存项遍历游标，不是新的缓存槽。
     */
    struct cmt_entry *cmt_entry, *tmp_entry;
    /* 变量 tvpn：实际保存被淘汰 entry.lpn，get_gtd_ent/translation_write_page 内部才除 TP 尺寸。 */
    uint64_t tvpn;
    /* 变量 gtd_ppa：被淘汰映射所属旧 TP 的物理地址。 */
    struct ppa gtd_ppa;
    /* 变量 tpnode_evict_flag：返回标志：1 表示 TPnode 也删除，0 表示仅淘汰 entry。 */
    int tpnode_evict_flag = 0;
    /* 变量 ht：哈希表引用，包含 LPN entry 桶和 TPnode 桶。 */
    struct hash_table *ht = &cm->ht;

    //从 TP 热度队尾选最冷 TPnode
    /* 阶段 1：选最冷 TPnode，在该 TP 内优先寻找冷 CLEAN，缺少时取冷 DIRTY。 */
    /* 调用 QTAILQ_LAST：取得双向队列尾元素或 NULL，不摘链；本文件用尾部表示较冷对象。 */
    tpnode = QTAILQ_LAST(&cm->TPnode_list);
    if (tpnode->cmt_entry_cnt == 0)
        /* 调用 ftl_err：通过错误日志宏输出到 stderr；宏本身不终止函数或修复状态。 */
        ftl_err("tpnode cannot be empty!");
    //在最冷 TP 内由冷到热优先寻找 CLEAN entry
    /* 调用 QTAILQ_FOREACH_REVERSE：从尾向头遍历指定 entry 队列，此处用于从冷到热寻找 CLEAN 映射。 */
    QTAILQ_FOREACH_REVERSE(cmt_entry, &tpnode->cmt_entry_list, entry) {
        if (cmt_entry->dirty == CLEAN) break;
    }
    //若没有 CLEAN entry，则取队尾最冷 DIRTY entry
    if (cmt_entry == NULL) {
        /* 调用 QTAILQ_LAST：取得双向队列尾元素或 NULL，不摘链；本文件用尾部表示较冷对象。 */
        cmt_entry = QTAILQ_LAST(&tpnode->cmt_entry_list);
    }
    //从 TP 的 entry 队列摘除一个缓存槽
    /* 阶段 2：从 entry 队列摘槽并清该 LPN 的存在标记。 */
    /* 调用 QTAILQ_REMOVE：按队列头、节点、链接字段 entry 摘除对象；不释放对象，也不改业务计数。 */
    QTAILQ_REMOVE(&tpnode->cmt_entry_list, cmt_entry, entry);
    tpnode->cmt_entry_cnt--;

    tpnode->exist_ent[cmt_entry->lpn % spp->ents_per_pg] = 0;

    /* 阶段 3：DIRTY 触发旧 TP 读/失效与新 TP 分配，同 TP 其他脏项一起标 CLEAN。 */
    if (cmt_entry->dirty == DIRTY) {
        tvpn = cmt_entry->lpn;
        /* 调用 get_gtd_ent：按 LPN 所属翻译页索引读取 GTD 中该 TP 的 PPA。 */
        gtd_ppa = get_gtd_ent(ssd, tvpn);
        /* 调用 mapped_ppa：检查 packed PPA 是否不同于 UNMAPPED_PPA 哨兵。 */
        if (mapped_ppa(&gtd_ppa)) {
            /* 调用 translation_read_page_no_req：构造 USER_IO NAND_READ，模拟一次不绑定 Host 请求的翻译页读取。 */
            translation_read_page_no_req(ssd, &gtd_ppa);

            /* 调用 mark_page_invalid：把有效页改为 INVALID，同时 block/line 的 ipc++、vpc--。 */
            mark_page_invalid(ssd, &gtd_ppa);
            /* 调用 set_rmap_ent：由 PPA 得到 PPN，将反向标识写入 rmap。 */
            set_rmap_ent(ssd, INVALID_LPN, &gtd_ppa);
        }
        

        
        /* 调用 translation_write_page：为传入 LPN 所属 TP 分配 trans_wp 元数据页，更新 GTD/rmap/VALID 后写后推进。 */
        translation_write_page(ssd, tvpn);

        //同 TP 的其余 DIRTY 一起视为已更新并置 CLEAN；原型未序列化真实映射 payload
        /* 调用 QTAILQ_FOREACH：沿指定 entry 双向队列从头到尾遍历，不自动删除或释放节点。 */
        QTAILQ_FOREACH(tmp_entry, &tpnode->cmt_entry_list, entry) {
            if (tmp_entry->dirty == DIRTY)
                tmp_entry->dirty = CLEAN;
        }
    }

    /* 阶段 4：解除 entry 哈希，重置并归还 free 槽；无 payload 序列化。 */
    /* 调用 delete_cmt_hashnode：按节点地址从 CMT 哈希桶摘除一个 entry。 */
    delete_cmt_hashnode(ht, cmt_entry);

    //将淘汰槽重置后归还 free entry 池
    cmt_entry->dirty = CLEAN;
    cmt_entry->lpn = INVALID_LPN;
    cmt_entry->ppn = UNMAPPED_PPA;
    cmt_entry->prefetch = false;
    cmt_entry->next_avail_time = 0;

    /* 调用 QTAILQ_INSERT_TAIL：用对象内 entry 链接插到队尾；free 池回收/victim FIFO 均显式维护计数。 */
    QTAILQ_INSERT_TAIL(&cm->free_cmt_entry_list, cmt_entry, entry);
    cm->free_cmt_entry_cnt++;
    cm->used_cmt_entry_cnt--;
    //TP 已无缓存项时，移除其热度/哈希节点并释放
    /* 阶段 5：若该 TP 已空，则删除节点/哈希并更新预取调节计数。 */
    if (tpnode->cmt_entry_cnt == 0) {
        /* 调用 QTAILQ_REMOVE：按队列头、节点、链接字段 entry 摘除对象；不释放对象，也不改业务计数。 */
        QTAILQ_REMOVE(&cm->TPnode_list, tpnode, entry);

        /* 调用 delete_tp_hashnode：按节点地址从 TP 哈希桶摘除一个 TPnode。 */
        delete_tp_hashnode(ht, tpnode);

        cm->tt_TPnodes--;
        /* 调用 g_free：释放指定动态分配对象；ownership 节点释放不等于释放其引用的 line 实体。 */
        g_free(tpnode);
        tpnode = NULL;
        tpnode_evict_flag = 1;
        cm->counter--;
        if (cm->counter == -3) {
            cm->counter = 0;
            spp->enable_select_prefetch = true;
        }
    }

    return tpnode_evict_flag;
}

/* TPFTL 两级预取：需求项先装入；预取只使用空闲槽，不能淘汰本次需求项。
 * 淘汰可能同步 GC，因此在淘汰后重新读取 TP 地址及映射内容。
 */
static struct nand_lun *load_translation_page(struct ssd *ssd, NvmeRequest *req,
                                              uint64_t first, uint64_t last,
                                              bool writing)
{
    struct ssdparams *p = &ssd->sp;
    struct cmt_mgmt *cm = &ssd->cm;
    uint64_t index = first / p->ents_per_pg;
    if (cm->used_cmt_entry_cnt == cm->tt_entries) evict_entry_from_cmt(ssd);
    struct ppa tp = ssd->gtd[index];
    struct nand_lun *lun = NULL;
    uint64_t ready = req->stime;
    if (mapped_ppa(&tp)) {
        translation_read_page(ssd, req, &tp);
        lun = get_lun(ssd, &tp);
        ready = lun->next_lun_avail_time;
    }
    struct ppa data = get_maptbl_ent(ssd, first);
    uint64_t ppn = mapped_ppa(&data) ? ppa2pgidx(ssd, &data) : UNMAPPED_PPA;
    if (!writing && ppn == UNMAPPED_PPA) return lun;
    insert_entry_to_cmt(ssd, first, ppn, HEAD, false, ready);

    uint64_t tp_last = (index + 1) * p->ents_per_pg - 1;
    if (tp_last >= (uint64_t)p->tt_pgs) tp_last = p->tt_pgs - 1;
    uint64_t prefetch_last = first;
    if (p->enable_request_prefetch) prefetch_last = last < tp_last ? last : tp_last;
    if (p->enable_select_prefetch) {
        struct TPnode *node = find_hash_tpnode(&cm->ht, index);
        int offset = first % p->ents_per_pg;
        int count = 0;
        while (offset >= 0 && node->exist_ent[offset]) { count++; offset--; }
        uint64_t selected_last = first + count;
        if (selected_last > tp_last) selected_last = tp_last;
        if (selected_last > prefetch_last) prefetch_last = selected_last;
    }
    for (uint64_t lpn = first + 1; lpn <= prefetch_last; lpn++) {
        if (cmt_hit_no_move(ssd, lpn)) continue;
        if (!cm->free_cmt_entry_cnt) break;
        data = get_maptbl_ent(ssd, lpn);
        ppn = mapped_ppa(&data) ? ppa2pgidx(ssd, &data) : UNMAPPED_PPA;
        if (!writing && ppn == UNMAPPED_PPA) continue;
        insert_entry_to_cmt(ssd, lpn, ppn, TAIL, true, ready);
    }
    return lun;
}

static struct nand_lun *process_translation_page_read(struct ssd *ssd, NvmeRequest *req,
                                                      uint64_t first, uint64_t last)
{
    return load_translation_page(ssd, req, first, last, false);
}

static struct nand_lun *process_translation_page_write(struct ssd *ssd, NvmeRequest *req,
                                                       uint64_t first, uint64_t last)
{
    return load_translation_page(ssd, req, first, last, true);
}



/**
 * 功能：在 enable_gc_delay 开启时模拟一次 GC NAND_READ。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 参数 ppa：本次物理地址；DATA/TP 及输入输出语义由本函数路径确定。
 * 作用与边界：没有 payload 搬运；返回 void，但 LUN 时间及读估算能耗会更新。
 */
static void gc_read_page(struct ssd *ssd, struct ppa *ppa)
{
    /* 更新 GC 读时序；此函数不接收返回延迟，但 LUN 可用时间仍被更新 */
    if (ssd->sp.enable_gc_delay) {
        /* 变量 gcr：用于模拟 GC NAND_READ 的命令。 */
        struct nand_cmd gcr;
        gcr.type = GC_IO;
        gcr.cmd = NAND_READ;
        gcr.stime = 0;
        /* 调用 ssd_advance_status：模拟一个 NAND 读/写/擦命令的 LUN 排队，更新完成时间、写计数与估算能耗。 */
        ssd_advance_status(ssd, ppa, &gcr);
    }
}

/* 旧写回草案：把 victim 有效页的映射搬到新页；用户 payload 实际保存在逻辑 DRAM 后端，下面函数被注释 */
/* 历史/诊断代码：以下被注释的声明或实现不参与编译，保留原 C 标识符；不得按它推导当前行为。 */
// static uint64_t gc_write_page_through_gtd_wp(struct ssd *ssd, struct ppa *old_ppa, uint64_t lpn, struct write_pointer* wpp)
// {
//     struct ppa new_ppa;
//     struct nand_lun *new_lun;

//     ftl_assert(valid_lpn(ssd, lpn));

//     advance_line_write_pointer(ssd, wpp);

//     // new_ppa = get_new_page(ssd);
//     new_ppa = get_new_line_page(ssd, wpp);
//     更新完整 LPN→PPA 真值表
//     ssd->gtd[lpn] = new_ppa;
//     更新新物理页的反向标识

//     mark_page_valid(ssd, &new_ppa);

//     写指针推进约定见本函数；部分旧调用被注释，不可当作已执行
//     // ssd_advance_write_pointer(ssd);
//     // advance_line_write_pointer(ssd, wpp);

//     if (ssd->sp.enable_gc_delay) {
//         struct nand_cmd gcw;
//         gcw.type = GC_IO;
//         gcw.cmd = NAND_WRITE;
//         gcw.stime = 0;
//         ssd_advance_status(ssd, &new_ppa, &gcw);
//     }

//     禁用草案：也更新 channel 级 gc_endtime；当前仅更新 LUN 级时间
// #if 0
//     new_ch = get_ch(ssd, &new_ppa);
//     new_ch->gc_endtime = new_ch->next_ch_avail_time;
// #endif

//     new_lun = get_lun(ssd, &new_ppa);
//     new_lun->gc_endtime = new_lun->next_lun_avail_time;

//     return 0;
// }


/* GC 数据写：同步真值、反向表、CMT 和 bitmap，随后模拟 NAND 编程。 */
static uint64_t gc_write_page_through_line_wp(struct ssd *ssd, uint64_t lpn,
                                            struct ppa *new_ppa, struct write_pointer *wpp)
{
    if (wpp->curline->rest < ssd->sp.pgs_per_line) {
        advance_line_write_pointer(ssd, wpp);
    }
    struct ppa old = get_maptbl_ent(ssd, lpn);
    uint64_t source_ready = mapped_ppa(&old) ? get_lun(ssd, &old)->next_lun_avail_time : 0;
    *new_ppa = get_new_line_page(ssd, wpp);
    if (mapped_ppa(&old) && valid_ppa(ssd, &old)) {
        if (get_pg(ssd, &old)->status == PG_VALID) {
            mark_page_invalid(ssd, &old);
        }
        set_rmap_ent(ssd, INVALID_LPN, &old);
    }
    model_bitmap_set(ssd, lpn, 0);
    set_maptbl_ent(ssd, lpn, new_ppa);
    set_rmap_ent(ssd, lpn, new_ppa);
    mark_page_valid(ssd, new_ppa);
    struct cmt_entry *entry = cmt_hit_no_move(ssd, lpn);
    if (entry) {
        entry->ppn = ppa2pgidx(ssd, new_ppa);
        entry->dirty = DIRTY;
    }
    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd cmd = { .type = GC_IO, .cmd = NAND_WRITE, .stime = source_ready };
        ssd_advance_status(ssd, new_ppa, &cmd);
    }
    struct nand_lun *lun = get_lun(ssd, new_ppa);
    lun->gc_endtime = lun->next_lun_avail_time;
    return 0;
}

/* 历史/诊断代码：以下被注释的声明或实现不参与编译，保留原 C 标识符；不得按它推导当前行为。 */
// static struct line *select_victim_line(struct ssd *ssd, bool force)
// {
//     struct line_mgmt *lm = &ssd->lm;
//     struct line *victim_line = NULL;

//     victim_line = pqueue_peek(lm->victim_line_pq);
//     if (!victim_line) {
//         return NULL;
//     }

//     if (!force && victim_line->ipc < ssd->sp.pgs_per_line / 8) {
//         return NULL;
//     }

//     pqueue_pop(lm->victim_line_pq);
//     victim_line->pos = 0;
//     lm->victim_line_cnt--;

//     旧优先队列草案：victim 已摘除队列，返回待回收引用
//     return victim_line;
// }

/**
 * 功能：把已清理 line 从 victim 队列移回 free 池，恢复容量并解除 owner。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 参数 ppa：本次物理地址；DATA/TP 及输入输出语义由本函数路径确定。
 * 作用与边界：不释放 line 本体、不删 WP 引用、不减 vic_cnt、不把 type 设 UNUSED；必须由 caller 完成其他步骤。
 */
static void mark_line_free(struct ssd *ssd, struct ppa *ppa)
{
    /* 变量 lm：全局 line 管理器引用，管理实体和 free/victim 队列计数。 */
    struct line_mgmt *lm = &ssd->lm;
    /* 变量 line：设备内的跨 LUN line 实体引用，id 对应 block id。 */
    /* 调用 get_line：使用 PPA.blk 返回全局 line 实体。 */
    struct line *line = get_line(ssd, ppa);
    line->ipc = 0;
    line->vpc = 0;
    line->rest = ssd->sp.pgs_per_line;
    /* 把此 line 从全局 victim 队列移回 free 池 */
    /* 调用 QTAILQ_REMOVE：按队列头、节点、链接字段 entry 摘除对象；不释放对象，也不改业务计数。 */
    QTAILQ_REMOVE(&lm->victim_list, line, entry);

    /* 调用 QTAILQ_INSERT_TAIL：用对象内 entry 链接插到队尾；free 池回收/victim FIFO 均显式维护计数。 */
    QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
    lm->victim_line_cnt--;
    lm->free_line_cnt++;
    ssd->line2write_pointer[line->id] = NULL;
}

/* 元数据搬移沿用 translation WP 的“指向下一空页”约定。 */
static uint64_t gc_translation_page_write(struct ssd *ssd, struct ppa *old_ppa)
{
    uint64_t source_ready = get_lun(ssd, old_ppa)->next_lun_avail_time;
    uint64_t index = get_rmap_ent(ssd, old_ppa);
    struct ppa updated = get_new_line_page(ssd, &ssd->trans_wp);
    mark_page_invalid(ssd, old_ppa);
    set_rmap_ent(ssd, INVALID_LPN, old_ppa);
    set_gtd_ent(ssd, &updated, index);
    set_rmap_ent(ssd, index, &updated);
    mark_page_valid(ssd, &updated);
    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd cmd = { .type = GC_IO, .cmd = NAND_WRITE, .stime = source_ready };
        ssd_advance_status(ssd, &updated, &cmd);
    }
    struct nand_lun *lun = get_lun(ssd, &updated);
    lun->gc_endtime = lun->next_lun_avail_time;
    /* 只有一个预留 line 且目标恰写满时，先归还源 line，再推进到下一空页。 */
    if (ssd->trans_wp.curline->rest > 0 || ssd->lm.free_line_cnt > 0) {
        advance_line_write_pointer(ssd, &ssd->trans_wp);
    }
    return 0;
}

static void clean_one_trans_block(struct ssd *ssd, struct ppa *ppa)
{
    for (int pg = 0; pg < ssd->sp.pgs_per_blk; pg++) {
        ppa->g.pg = pg;
        if (get_pg(ssd, ppa)->status != PG_VALID) {
            continue;
        }
        uint64_t index = get_rmap_ent(ssd, ppa);
        if (index >= (uint64_t)ssd->sp.tt_gtd_size) {
            ftl_err("GC: invalid translation-page reverse mapping\n");
            abort();
        }
        struct ppa actual = ssd->gtd[index];
        if (mapped_ppa(&actual) && valid_ppa(ssd, &actual) &&
            ppa2pgidx(ssd, &actual) == ppa2pgidx(ssd, ppa)) {
            gc_read_page(ssd, ppa);
            gc_translation_page_write(ssd, ppa);
        } else {
            ftl_err("GC: valid translation page is not referenced by GTD\n");
            abort();
        }
    }
}

static int gtd_do_gc(struct ssd *ssd, bool force, struct write_pointer *wpp,
                     struct line *victim_line, bool delete)
{
    (void)force;
    struct ppa victim = { .ppa = 0 };
    victim.g.blk = victim_line->id;
    for (int ch = 0; ch < ssd->sp.nchs; ch++) {
        for (int lun = 0; lun < ssd->sp.luns_per_ch; lun++) {
            victim.g.ch = ch;
            victim.g.lun = lun;
            clean_one_trans_block(ssd, &victim);
        }
    }
    free_all_blocks(ssd, &victim);
    if (delete) {
        clear_one_write_pointer_victim_lines(wpp->wpl, victim_line);
    }
    mark_line_free(ssd, &victim);
    ssd->stat.trans_wp_gc_times++;
    return 0;
}

static void batch_gtd_do_gc(struct ssd *ssd, bool force, struct write_pointer *wpp,
                             int num, struct line *delete_line)
{
    (void)num;
    (void)delete_line;
    if (!wpp->curline || ssd->gc_in_progress || ssd->lm.free_line_cnt < 1) {
        return;
    }
    int n = 0;
    struct line **sources = g_new(struct line *, wpp->vic_cnt);
    for (struct wp_lines *link = wpp->wpl->next; link; link = link->next) {
        sources[n++] = link->line;
    }
    seal_current_line(ssd, wpp);
    ssd->gc_in_progress = true;
    init_line_write_pointer(ssd, wpp, false);
    for (int i = 0; i < n; i++) {
        gtd_do_gc(ssd, force, wpp, sources[i], true);
        wpp->vic_cnt--;
        if (wpp->curline->rest == 0) {
            advance_line_write_pointer(ssd, wpp);
        }
    }
    ssd->gc_in_progress = false;
    g_free(sources);
}

/* 搬移完成后统一擦除一次，清 rmap；GC_time 累计各 block 实际模拟延迟。 */
static void free_all_blocks(struct ssd *ssd, struct ppa *tppa)
{
    uint64_t ready = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    for (int ch = 0; ch < ssd->sp.nchs; ch++) {
        for (int lun = 0; lun < ssd->sp.luns_per_ch; lun++) {
            uint64_t end = ssd->ch[ch].lun[lun].next_lun_avail_time;
            if (end > ready) ready = end;
        }
    }
    struct ppa ppa = { .ppa = 0 };
    ppa.g.blk = tppa->g.blk;
    for (int ch = 0; ch < ssd->sp.nchs; ch++) {
        for (int lun = 0; lun < ssd->sp.luns_per_ch; lun++) {
            ppa.g.ch = ch;
            ppa.g.lun = lun;
            for (int pg = 0; pg < ssd->sp.pgs_per_blk; pg++) {
                ppa.g.pg = pg;
                if (get_pg(ssd, &ppa)->status == PG_VALID) {
                    ftl_err("GC attempted to erase a live source page\n");
                    abort();
                }
                if (get_line(ssd, &ppa)->type == DATA && get_pg(ssd, &ppa)->status == PG_INVALID) {
                    uint64_t lpn = get_rmap_ent(ssd, &ppa);
                    if (!valid_lpn(ssd, lpn)) {
                        ftl_err("GC invalid source page has no OOB identity\n");
                        abort();
                    }
                    struct write_pointer *group = &ssd->gtd_wps[lpn / ssd->sp.ents_per_pg / ssd->sp.trans_per_line];
                    ftl_assert(group->invalid_pages > 0);
                    group->invalid_pages--;
                }
            }
            ppa.g.pg = 0;
            mark_block_free(ssd, &ppa);
            for (int pg = 0; pg < ssd->sp.pgs_per_blk; pg++) {
                ppa.g.pg = pg;
                set_rmap_ent(ssd, INVALID_LPN, &ppa);
            }
            ppa.g.pg = 0;
            if (ssd->sp.enable_gc_delay) {
                struct nand_cmd cmd = { .type = GC_IO, .cmd = NAND_ERASE, .stime = ready };
                ssd->stat.GC_time += ssd_advance_status(ssd, &ppa, &cmd);
            }
            struct nand_lun *state = get_lun(ssd, &ppa);
            state->gc_endtime = state->next_lun_avail_time;
        }
    }
}

/* 最小二乘使用完整的 [first,end) 样本和 double 累加；单样本也是合法模型。 */
static void model_fit_segment(uint64_t *x, uint64_t *y, int first, int end,
                              lr_breakpoint *brk)
{
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int n = end - first;
    for (int i = first; i < end; i++) {
        double dx = x[i];
        double dy = y[i];
        sx += dx;
        sy += dy;
        sxx += dx * dx;
        sxy += dx * dy;
    }
    double denominator = n * sxx - sx * sx;
    brk->w = denominator > 0 ? (n * sxy - sx * sy) / denominator : 0;
    brk->b = (sy - brk->w * sx) / n;
    brk->key = x[end - 1];
}

/* 搬移后写回整张 TP；此模拟器以 maptbl 作为 TP 内容的真值。 */
static void gc_flush_translation_page(struct ssd *ssd, int gtd_index)
{
    struct ppa old = ssd->gtd[gtd_index];
    if (mapped_ppa(&old) && valid_ppa(ssd, &old)) {
        mark_page_invalid(ssd, &old);
        set_rmap_ent(ssd, INVALID_LPN, &old);
    }
    translation_write_page(ssd, (uint64_t)gtd_index * ssd->sp.ents_per_pg);
    struct ppa updated = ssd->gtd[gtd_index];
    struct nand_lun *lun = get_lun(ssd, &updated);
    lun->gc_endtime = lun->next_lun_avail_time;
    uint64_t first = (uint64_t)gtd_index * ssd->sp.ents_per_pg;
    uint64_t end = first + ssd->sp.ents_per_pg;
    for (uint64_t lpn = first; lpn < end && lpn < (uint64_t)ssd->sp.tt_pgs; lpn++) {
        struct cmt_entry *entry = cmt_hit_no_move(ssd, lpn);
        if (entry) {
            entry->dirty = CLEAN;
        }
    }
}

/* 论文 III-E2：排序、搬移、贪心分段回归、逐页评估。固定最多八段，不漏余数/末项。 */
static void model_training(struct ssd *ssd, struct write_pointer *wpp,
                           uint64_t *group_lpns, int *counts, int start_gtd)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t *x = g_new(uint64_t, spp->ents_per_pg);
    uint64_t *y = g_new(uint64_t, spp->ents_per_pg);
    ssd->stat.model_training_nums++;
    for (int slot = 0; slot < spp->trans_per_line; slot++) {
        int index = start_gtd + slot;
        if (index >= spp->tt_gtd_size) {
            break;
        }
        int n = counts[slot];
        lr_node *node = &ssd->lr_nodes[index];
        uint64_t tp_start = (uint64_t)index * spp->ents_per_pg;
        uint64_t tp_end = tp_start + spp->ents_per_pg;
        /* 整个 TP 都将被重建，包含空 TP、小样本及先前未置零的尾部。 */
        memset(node, 0, sizeof(*node));
        for (uint64_t page = tp_start; page < tp_end && page < (uint64_t)spp->tt_pgs; page++) {
            model_bitmap_set(ssd, page, 0);
        }
        if (!n) {
            continue;
        }
        uint64_t *lpns = &group_lpns[slot * spp->ents_per_pg];
        quick_sort(lpns, 0, n - 1);
        uint64_t base = UINT64_MAX;
        for (int i = 0; i < n; i++) {
            struct ppa moved;
            gc_write_page_through_line_wp(ssd, lpns[i], &moved, wpp);
            x[i] = lpns[i] - lpns[0];
            y[i] = ppa2vppn(ssd, &moved);
            if (y[i] < base) {
                base = y[i];
            }
        }
        node->start_lpn = lpns[0];
        node->start_ppa = base;
        for (int i = 0; i < n; i++) {
            y[i] -= base;
        }

        int first = 0;
        for (int piece = 0; piece < MAX_INTERVALS; piece++) {
            lr_breakpoint *brk = &node->brks[piece];
            if (first == n) {
                brk->key = x[n - 1];
                continue;
            }
            int end = first + 1;
            if (piece == MAX_INTERVALS - 1) {
                /* 最后一段接纳全部剩余样本；无法准确预测的页由 bitmap 回退。 */
                end = n;
            } else {
                /* 贪心扩大能满足半页舍入误差的斜率区间，空交集处开始下一段。 */
                double low = -DBL_MAX, high = DBL_MAX;
                while (end < n) {
                    double dx = x[end] - x[first];
                    double dy = (double)y[end] - (double)y[first];
                    double lower = (dy - 0.499) / dx;
                    double upper = (dy + 0.499) / dx;
                    double next_low = lower > low ? lower : low;
                    double next_high = upper < high ? upper : high;
                    if (next_low > next_high) {
                        break;
                    }
                    low = next_low;
                    high = next_high;
                    end++;
                }
            }
            model_fit_segment(x, y, first, end, brk);
            first = end;
        }

        int piece = 0;
        for (int i = 0; i < n; i++) {
            while (piece < MAX_INTERVALS - 1 && x[i] > (uint64_t)node->brks[piece].key) {
                piece++;
            }
            lr_breakpoint *brk = &node->brks[piece];
            float predicted = model_value(x[i], brk);
            if (predicted >= 0 && predicted < (double)(spp->tt_pgs - base)) {
                uint64_t rounded = (uint64_t)predicted;
                if (predicted - rounded >= 0.5) {
                    rounded++;
                }
                if (rounded == y[i]) {
                    model_bitmap_set(ssd, lpns[i], 1);
                }
            }
        }
        gc_flush_translation_page(ssd, index);
    }
    g_free(y);
    g_free(x);
}

/* 整组 GC：有借页关系时同时回收 hot/donor 的全部 line，先完整预算再搬移。
 * 为限制关系元数据和迁移峰值，一个组一次仅参与一个借页配对。
 */
static int batch_line_do_gc(struct ssd *ssd, bool force, struct write_pointer *wpp,
                            struct line *delete_line)
{
    (void)force;
    (void)delete_line;
    struct ssdparams *p = &ssd->sp;
    if (!wpp || !wpp->curline || ssd->gc_in_progress) return -1;
    struct write_pointer *groups[2] = { wpp, wpp->borrow_peer };
    int group_count = groups[1] ? 2 : 1;
    int source_count = 0;
    int capacity = wpp->vic_cnt + (groups[1] ? groups[1]->vic_cnt : 0);
    struct line **sources = g_new(struct line *, capacity);
    uint64_t *lpns[2] = { NULL, NULL };
    int *counts[2] = { NULL, NULL };
    int result = -1;
    int tp_writes = 0;
    for (int g = 0; g < group_count; g++) {
        lpns[g] = g_new(uint64_t, p->trans_per_line * p->ents_per_pg);
        counts[g] = g_new0(int, p->trans_per_line);
        for (struct wp_lines *link = groups[g]->wpl->next; link; link = link->next) {
            sources[source_count++] = link->line;
        }
    }
    /* 每条源 line 唯一属于一个物理 owner；逐页按实际 LPN 分配到逻辑组。 */
    for (int i = 0; i < source_count; i++) {
        struct ppa source = { .ppa = 0 };
        source.g.blk = sources[i]->id;
        for (int ch = 0; ch < p->nchs; ch++) {
            for (int lun = 0; lun < p->luns_per_ch; lun++) {
                source.g.ch = ch; source.g.lun = lun;
                for (int pg = 0; pg < p->pgs_per_blk; pg++) {
                    source.g.pg = pg;
                    if (get_pg(ssd, &source)->status != PG_VALID) continue;
                    uint64_t lpn = get_rmap_ent(ssd, &source);
                    if (!valid_lpn(ssd, lpn)) goto done;
                    struct ppa actual = get_maptbl_ent(ssd, lpn);
                    if (actual.ppa != source.ppa) goto done;
                    int group = lpn / p->ents_per_pg / p->trans_per_line;
                    int g = group == groups[0]->id ? 0 : 1;
                    if (g >= group_count || group != groups[g]->id) goto done;
                    int slot = lpn / p->ents_per_pg - groups[g]->id * p->trans_per_line;
                    if (counts[g][slot] >= p->ents_per_pg) goto done;
                    lpns[g][slot * p->ents_per_pg + counts[g][slot]++] = lpn;
                }
            }
        }
    }
    for (int g = 0; g < group_count; g++) {
        for (int slot = 0; slot < p->trans_per_line; slot++) tp_writes += counts[g][slot] > 0;
    }
    int metadata_lines = 0;
    if (tp_writes && tp_writes >= ssd->trans_wp.curline->rest) {
        metadata_lines = 1 + (tp_writes - ssd->trans_wp.curline->rest) / p->pgs_per_line;
    }
    if (ssd->lm.free_line_cnt < group_count + metadata_lines) goto done;

    /* 验证和空间预算通过后才发出 NAND 读写及修改所有权。 */
    for (int g = 0; g < group_count; g++) {
        int start = groups[g]->id * p->trans_per_line;
        for (int slot = 0; slot < p->trans_per_line && start + slot < p->tt_gtd_size; slot++) {
            struct ppa tp = ssd->gtd[start + slot];
            if (mapped_ppa(&tp)) gc_read_page(ssd, &tp);
            for (int j = 0; j < counts[g][slot]; j++) {
                struct ppa data = get_maptbl_ent(ssd, lpns[g][slot * p->ents_per_pg + j]);
                gc_read_page(ssd, &data);
            }
        }
        seal_current_line(ssd, groups[g]);
    }
    ssd->gc_in_progress = true;
    for (int g = 0; g < group_count; g++) init_line_write_pointer(ssd, groups[g], false);
    for (int g = 0; g < group_count; g++) {
        model_training(ssd, groups[g], lpns[g], counts[g], groups[g]->id * p->trans_per_line);
        groups[g]->trained = true;
    }
    for (int i = 0; i < source_count; i++) {
        struct ppa victim = { .ppa = 0 };
        victim.g.blk = sources[i]->id;
        struct write_pointer *owner = ssd->line2write_pointer[sources[i]->id];
        free_all_blocks(ssd, &victim);
        clear_one_write_pointer_victim_lines(owner->wpl, sources[i]);
        mark_line_free(ssd, &victim);
        owner->vic_cnt--;
        ssd->stat.gc_times++;
        ssd->stat.line_gc_times[sources[i]->id]++;
        ssd->stat.wp_victims[owner->id]++;
        ssd->stat.line_wp_gc_times++;
    }
    for (int g = 0; g < group_count; g++) {
        groups[g]->borrow_peer = NULL;
        groups[g]->is_borrower = false;
        groups[g]->borrowed_pages = 0;
    }
    ssd->gc_in_progress = false;
    result = 0;
 done:
    for (int g = 0; g < group_count; g++) { g_free(counts[g]); g_free(lpns[g]); }
    g_free(sources);
    return result;
}

/* 论文 III-D：hot 组写满后优先使用未训练 cold 组的空尾页。
 * 论文没有给出借页阈值；这里显式采用一条 line 的 1/4，并保留成对 GC 空间。
 */
static struct ppa allocate_host_data_page(struct ssd *ssd, struct write_pointer *wp)
{
    uint64_t threshold = ssd->sp.pgs_per_line / 4;
    if (!threshold) threshold = 1;
    if (wp->borrow_peer) {
        struct write_pointer *hot = wp->is_borrower ? wp : wp->borrow_peer;
        struct write_pointer *donor = hot->borrow_peer;
        if (hot->borrowed_pages >= threshold || donor->free_pages <= 1) {
            if (batch_line_do_gc(ssd, true, hot, NULL) != 0) {
                ftl_err("Insufficient reserve for paired group GC\n");
                abort();
            }
        }
    }
    if (!wp->borrow_peer && wp->curline && wp->curline->rest == 0 &&
        /* 两个数据目标和至多一个翻译页目标仍有备用 line 时，
         * 全局空间压力不妨碍使用已分配 cold line 的空尾页。 */
        ssd->lm.free_line_cnt >= 3) {
        struct write_pointer *donor = NULL;
        for (int g = 0; g < ssd->sp.tt_line_wps; g++) {
            struct write_pointer *candidate = &ssd->gtd_wps[g];
            if (candidate == wp || candidate->trained || candidate->borrow_peer ||
                !candidate->curline || candidate->free_pages <= threshold) continue;
            if (!donor || candidate->free_pages > donor->free_pages) donor = candidate;
        }
        if (donor) {
            seal_current_line(ssd, wp);
            wp->borrow_peer = donor; donor->borrow_peer = wp;
            wp->is_borrower = true;
            wp->borrowed_pages = 0;
            wp->invade_lines++;
        }
    }
    struct write_pointer *physical = wp->is_borrower ? wp->borrow_peer : wp;
    if (!physical->curline) {
        init_line_write_pointer(ssd, physical, true);
    } else if (physical->curline->rest < ssd->sp.pgs_per_line) {
        advance_line_write_pointer(ssd, physical);
    }
    struct ppa result = get_new_line_page(ssd, physical);
    if (wp->is_borrower) wp->borrowed_pages++;
    return result;
}

/* 论文 III-B：bitmap 是准确性保证，读路径直接采用模型生成的物理地址。
 * 完整 maptbl 仅用于训练/模拟 TP 内容；这里不拿真值替换预测结果。
 */
static bool model_predict(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    if (!valid_lpn(ssd, lpn) || !model_bitmap_get(ssd, lpn)) {
        return false;
    }
    lr_node *node = &ssd->lr_nodes[lpn / ssd->sp.ents_per_pg];
    if (lpn < node->start_lpn ||
        node->start_ppa >= (uint64_t)ssd->sp.tt_pgs) {
        return false;
    }
    ssd->stat.model_use_num++;
    uint64_t x = lpn - node->start_lpn;
    for (int i = 0; i < MAX_INTERVALS; i++) {
        lr_breakpoint *brk = &node->brks[i];
        if (x > (uint64_t)brk->key) {
            continue;
        }
        float y = model_value(x, brk);
        uint64_t remaining = ssd->sp.tt_pgs - node->start_ppa;
        /* 同时排除 NaN/Inf、负值和超界值，避免未定义的浮点转整数。 */
        if (!(y >= 0 && y < (double)remaining)) {
            ssd->stat.model_out_range++;
            return false;
        }
        uint64_t rounded = (uint64_t)y;
        if (y - rounded >= 0.5) {
            rounded++;
        }
        if (rounded >= remaining) {
            ssd->stat.model_out_range++;
            return false;
        }
        *ppa = vppn2ppa(ssd, node->start_ppa + rounded);
        return true;
    }
    return false;
}


/**
 * 功能：打印访问/CMT hit/bitmap 候选计数，并只清零这三个计数。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 作用与边界：名字虽含 segments，实际不数模型段；部分清零会破坏统计窗口一致性。
 */
void count_segments(struct ssd* ssd) {
    /* 调用 printf：输出原有诊断/计数信息；日志字符串保留，打印本身不会恢复异常状态。 */
    printf("total cnt: %lld\n", (long long)ssd->stat.access_cnt);
    /* 调用 printf：输出原有诊断/计数信息；日志字符串保留，打印本身不会恢复异常状态。 */
    printf("cmt cnt: %lld\n", (long long)ssd->stat.cmt_hit_cnt);
    /* 调用 printf：输出原有诊断/计数信息；日志字符串保留，打印本身不会恢复异常状态。 */
    printf("model cnt: %lld\n", (long long)ssd->stat.model_hit_num);
    ssd->stat.access_cnt = 0;
    ssd->stat.cmt_hit_cnt = 0;
    ssd->stat.model_hit_num = 0;
}

/**
 * 功能：逐逻辑页先查 CMT，miss 后按 bitmap 尝试模型，失败再读 TP，最终模拟数据 READ。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 参数 req：外围 NVMe 请求；nlb 已转换为实际 sector 数，不是原始零基 NLB。
 * 作用与边界：CMT hit 使用缓存 PPN，模型命中使用计算得到的 VPPN。cmt_miss_cnt 只在 fallback 加，model_hit_num 只计成功；数据等翻译完成，maxlat 取各页最大值。缓存淘汰/GC 可增加基础 double read 之外的 I/O。
 */
static uint64_t ssd_read(struct ssd *ssd, NvmeRequest *req)
{
    /* 变量 spp：ssdparams 的局部引用，读取几何/时延/映射尺寸，不复制设备参数。 */
    struct ssdparams *spp = &ssd->sp;
    /* 变量 lba：请求起始逻辑 sector，来自 req.slba。 */
    uint64_t lba = req->slba;
    if (!req->nlb || lba >= (uint64_t)ssd->sp.tt_secs ||
        req->nlb > (uint64_t)ssd->sp.tt_secs - lba) {
        ftl_err("FTL request outside physical geometry\n");
        return 0;
    }
    /* 阶段 1：sector 请求换成 LPN 闭区间；提前拒绝空请求、溢出和越界范围。 */
    /* 变量 nsecs：请求实际 sector 数，来自已解码 req.nlb。 */
    int nsecs = req->nlb;
    /* 变量 ppa：本次物理地址；DATA/TP 及输入输出语义由本函数路径确定。 */
    struct ppa ppa;
    /* 变量 start_lpn：由 req.slba/secs_per_pg 得到的请求首 LPN。 */
    uint64_t start_lpn = lba / spp->secs_per_pg;
    /* 变量 end_lpn：由 (req.slba+req.nlb-1)/secs_per_pg 得到的请求末 LPN，闭区间；请求可跨 TP。 */
    uint64_t end_lpn = (lba + nsecs - 1) / spp->secs_per_pg;
    /* 变量：
     * lpn：当前逻辑页号，DATA 映射索引。
     * last_lpn：本 TP 的最后页或预取截断终点。
     */
    uint64_t lpn, last_lpn;
    /* 变量：
     * sublat：当前读页从请求起点到完成的模拟延迟，ns。
     * maxlat：本请求所有页完成延迟的最大值，非逐页相加。
     */
    uint64_t sublat, maxlat = 0;
    /* 变量 lun：当前 LUN 引用或编号，取决于声明类型。 */
    struct nand_lun *lun;

    // struct timespec time1, time2;
    

    if (end_lpn >= spp->tt_pgs) {
        /* 调用 ftl_err：通过错误日志宏输出到 stderr；宏本身不终止函数或修复状态。 */
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    }

    /* 逐逻辑页执行 Host 读路径 */
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        // clock_gettime(CLOCK_MONOTONIC, &time1);
        /* 阶段 2：逐页先查 CMT；命中读取 maptbl 真值，预取项建立 TP 时间依赖。 */
        ssd->stat.access_cnt++;
        sublat = 0;
        
        // 第一步查询 CMT，命中时调整两层热度
        /* 变量 cmt_entry：当前缓存映射槽，按 LPN 查找或从 free 池取得。 */
        /* 调用 cmt_hit：查询 LPN 与所属 TP，并把命中的 TPnode/entry 移到各自热度队列头。 */
        struct cmt_entry *cmt_entry = cmt_hit(ssd, lpn);
        if (cmt_entry) {
            ssd->stat.cmt_hit_cnt++;
            /* 调用 get_maptbl_ent：读取完整正向真值表中的 LPN→PPA。 */
            ppa = pgidx2ppa(ssd, cmt_entry ? cmt_entry->ppn : UNMAPPED_PPA);
            /* 调用 mapped_ppa：检查 packed PPA 是否不同于 UNMAPPED_PPA 哨兵。 */
            /* 调用 valid_ppa：检查 PPA 的六个几何字段是否位于设备范围内。 */
            if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
                //printf("%s,lpn(%" PRId64 ") not mapped to valid ppa\n", ssd->ssdname, lpn);
                //printf("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d,sec:%d\n",
                //ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl, ppa.g.pg, ppa.g.sec);
                ssd->stat.access_cnt--;
                ssd->stat.cmt_hit_cnt--;
                ssd->stat.model_out_range++;
                continue;
            }

            

            if (cmt_entry->prefetch) {
                /* 调用 get_lun：沿 channel→LUN 返回 PPA 所属 LUN 对象。 */
                lun = get_lun(ssd, &ppa);
                lun->next_lun_avail_time = (cmt_entry->next_avail_time > lun->next_lun_avail_time) ? \
                            cmt_entry->next_avail_time : lun->next_lun_avail_time;
            }

            goto ssd_read_latency;
        } 

        /* 阶段 3：CMT miss 且 bitmap=1 才进入候选；模型尝试要求 model_used。 */
        if (model_bitmap_get(ssd, lpn) == 1) {
            /* 变量 gtd_index：逻辑页所属 TP 索引，通常为 LPN/ents_per_pg。 */
            if (ssd->model_used) {
                
                if (ssd->model_used){
                    
                    /* 变量 f：model_predict 的边界验证结果；仅 true 才跳过翻译页 fallback。 */
                    /* 调用 model_predict：选取 LPN 分段模型，计算 VPPN 并检查数值与几何边界。 */
                    bool f = model_predict(ssd, lpn, &ppa);

                    if (f) {
                        ssd->stat.model_hit_num++;
                        goto ssd_read_latency;
                    }
                }
            }
        }
        
        /* 阶段 4：模型未接受才计 cmt_miss 并回退 TP；模型成功未计该 miss。 */
        ssd->stat.cmt_miss_cnt++;
            // 将本次回退的预取范围截到请求末尾或 TP 末尾
        last_lpn = (lpn / spp->ents_per_pg + 1) * spp->ents_per_pg - 1;
        last_lpn = (last_lpn < end_lpn) ? last_lpn : end_lpn;
        /* 调用 process_translation_page_read：处理读 CMT miss：查询 TP、模拟翻译读、加载当前映射并做两级预取。 */
        process_translation_page_read(ssd, req, lpn, last_lpn);
        /* 调用 cmt_hit_no_move：查询 LPN 缓存项并返回 entry 或 NULL，保持两层热度位置不动。 */
        cmt_entry = cmt_hit_no_move(ssd, lpn);
        /* 调用 get_maptbl_ent：读取完整正向真值表中的 LPN→PPA。 */
        ppa = pgidx2ppa(ssd, cmt_entry ? cmt_entry->ppn : UNMAPPED_PPA);
        /* 调用 mapped_ppa：检查 packed PPA 是否不同于 UNMAPPED_PPA 哨兵。 */
        /* 调用 valid_ppa：检查 PPA 的六个几何字段是否位于设备范围内。 */
        if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
            //printf("%s,lpn(%" PRId64 ") not mapped to valid ppa\n", ssd->ssdname, lpn);
            //printf("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d,sec:%d\n",
            //ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl, ppa.g.pg, ppa.g.sec);
            ssd->stat.access_cnt--;
            ssd->stat.cmt_miss_cnt--;
            // ssd->stat.model_out_range++;
            continue;
        }
        //DATA 读取必须等本次 TP READ 完成，即使二者在不同 LUN
        /* 阶段 5：data LUN 等地址翻译完成；即使 TP/data 位于不同 LUN 也有依赖。 */
        /* 调用 get_lun：沿 channel→LUN 返回 PPA 所属 LUN 对象。 */
        lun = get_lun(ssd, &ppa);
        lun->next_lun_avail_time = (cmt_entry->next_avail_time > lun->next_lun_avail_time) ? \
                                    cmt_entry->next_avail_time : lun->next_lun_avail_time;  
        


    /* 阶段 6：三路径合流，模拟 DATA READ，返回请求各页完成延迟最大值。 */
    ssd_read_latency:

        /* 调用 mapped_ppa：检查 packed PPA 是否不同于 UNMAPPED_PPA 哨兵。 */
        /* 调用 valid_ppa：检查 PPA 的六个几何字段是否位于设备范围内。 */
        if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
            ssd->stat.access_cnt--;
            //printf("%s,lpn(%" PRId64 ") not mapped to valid ppa\n", ssd->ssdname, lpn);
            //printf("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d,sec:%d\n",
            //ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl, ppa.g.pg, ppa.g.sec);
            continue;
        }

        /* 变量 srd：用于模拟 Host 数据 NAND_READ 的命令。 */
        struct nand_cmd srd;
        srd.type = USER_IO;
        srd.cmd = NAND_READ;
        srd.stime = req->stime;
        /* 调用 ssd_advance_status：模拟一个 NAND 读/写/擦命令的 LUN 排队，更新完成时间、写计数与估算能耗。 */
        sublat = ssd_advance_status(ssd, &ppa, &srd);
        maxlat = (sublat > maxlat) ? sublat : maxlat;
        // clock_gettime(CLOCK_MONOTONIC, &time2);
    }
    // ssd->stat.read_time += (maxlat + (time2.tv_sec - time1.tv_sec)*1000000000 + (time2.tv_nsec - time1.tv_nsec));
    return maxlat;
}

/* 按论文 III-E1，用一个 TP 内 VPPN 连续的写区间原位更新模型。
 * key 是右闭边界；被新区间吞并的后续段保留槽位，以相同 key 表示空段。
 */
static void model_sequential_update(struct ssd *ssd, uint64_t start_lpn,
                                   uint64_t end_lpn, uint64_t start_vppn)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t gtd_index = start_lpn / spp->ents_per_pg;
    uint64_t tp_start = gtd_index * spp->ents_per_pg;
    uint64_t tp_end = tp_start + spp->ents_per_pg - 1;
    lr_node *lrn = &ssd->lr_nodes[gtd_index];
    lr_node next = *lrn;
    int first = (int)(start_lpn - tp_start);
    int last = (int)(end_lpn - tp_start);
    int old_valid = 0, total_valid = 0;
    int piece = 0;

    if (tp_end >= (uint64_t)spp->tt_pgs) {
        tp_end = spp->tt_pgs - 1;
    }
    for (uint64_t page = tp_start; page <= tp_end; page++) {
        total_valid += model_bitmap_get(ssd, page) == 1;
    }

    if (!total_valid) {
        /* 无有效旧预测时直接建立本 TP 的基准，不能依赖初始化时的全零基准。 */
        next.start_lpn = tp_start;
        next.start_ppa = start_vppn;
        for (int j = 0; j < MAX_INTERVALS; j++) {
            next.brks[j].w = 1;
            next.brks[j].b = 0;
            next.brks[j].key = (j + 1) * spp->ents_per_pg / MAX_INTERVALS - 1;
        }
    } else {
        /* 新顺序区间使用小的相对截距，防止不同 line 的距离溢出 float16。
         * 旧段按有符号差换基准；不能表示的旧预测由下面的逐页评估清除。
         */
        int64_t shift = (int64_t)next.start_lpn - (int64_t)tp_start;
        uint64_t ppa_base = start_vppn >= (uint64_t)first ? start_vppn - first : 0;
        for (int j = 0; j < MAX_INTERVALS; j++) {
            next.brks[j].key += shift;
            next.brks[j].b = (double)next.brks[j].b - next.brks[j].w * (double)shift
                          + ((double)next.start_ppa - (double)ppa_base);
        }
        next.start_lpn = tp_start;
        next.start_ppa = ppa_base;
    }

    while (piece < MAX_INTERVALS - 1 && next.brks[piece].key < first) {
        piece++;
    }
    /* 替换整个起始段，并计入扩展边界将覆盖的后续段前缀；以 bitmap 为准。 */
    int left = piece == 0 ? 0 : next.brks[piece - 1].key + 1;
    int right = next.brks[piece].key > last ? next.brks[piece].key : last;
    for (uint64_t page = tp_start; page <= tp_end; page++) {
        int offset = (int)(page - tp_start);
        if (offset >= left && offset <= right) {
            old_valid += model_bitmap_get(ssd, page) == 1;
        }
    }
    bool updated = old_valid < last - first + 1;
    if (updated) {
        /* 绝对 VPPN=start_vppn+(LPN-start_lpn)，转换为现有预测器的相对 y=x+b。 */
        next.brks[piece].w = 1;
        next.brks[piece].b = (double)(start_vppn - next.start_ppa) - first;
        next.brks[piece].key = right;
        for (int j = piece + 1; j < MAX_INTERVALS; j++) {
            if (next.brks[j].key < right) {
                next.brks[j].key = right;
            }
        }
        *lrn = next;
    }

    /* 基准/边界变化可能影响旧预测；只保留经读路径相同舍入规则验证的候选。
     * 新区间两端均参与验证，未被替换的段也重新评估 bitmap。
     */
    total_valid = 0;
    for (uint64_t page = tp_start; page <= tp_end; page++) {
        bool candidate = model_bitmap_get(ssd, page) == 1 ||
                         (updated && page >= start_lpn && page <= end_lpn);
        model_bitmap_set(ssd, page, 0);
        if (!candidate || page < lrn->start_lpn) {
            continue;
        }
        uint64_t offset = page - lrn->start_lpn;
        for (int j = 0; j < MAX_INTERVALS; j++) {
            if (offset > (uint64_t)lrn->brks[j].key) {
                continue;
            }
            float predicted = model_value(offset, &lrn->brks[j]);
            /* 此比较同时排除负数、NaN、无穷及越界值，避免非法浮点转整数。 */
            if (predicted >= 0 && predicted < (double)(spp->tt_pgs - lrn->start_ppa)) {
                uint64_t rounded = (uint64_t)predicted;
                if (predicted - rounded >= 0.5) {
                    rounded++;
                }
                struct ppa actual = get_maptbl_ent(ssd, page);
                if (rounded < spp->tt_pgs - lrn->start_ppa &&
                    mapped_ppa(&actual) && valid_ppa(ssd, &actual) &&
                    rounded + lrn->start_ppa == ppa2vppn(ssd, &actual)) {
                    model_bitmap_set(ssd, page, 1);
                    total_valid++;
                }
            }
            break;
        }
    }
}

/**
 * 功能：按 LPN→TP→group 选择 WP，准备 CMT、失效旧页、分配新页并提交映射/写时序。
 * 参数 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。
 * 参数 req：外围 NVMe 请求；nlb 已转换为实际 sector 数，不是原始零基 NLB。
 * 作用与边界：DATA 除首写外先推进再分配；写入清旧 bitmap，尾部按 TP/VPPN 连续区间更新模型。
 */
static uint64_t ssd_write(struct ssd *ssd, NvmeRequest *req)
{
    // struct timespec time1, time2;
    /* 阶段 1：按 sector 计算 LPN 闭区间，并准备请求最大写延迟。 */
    /* 变量 lba：请求起始逻辑 sector，来自 req.slba。 */
    uint64_t lba = req->slba;
    if (!req->nlb || lba >= (uint64_t)ssd->sp.tt_secs ||
        req->nlb > (uint64_t)ssd->sp.tt_secs - lba) {
        ftl_err("FTL request outside physical geometry\n");
        return 0;
    }
    /* 变量 spp：ssdparams 的局部引用，读取几何/时延/映射尺寸，不复制设备参数。 */
    struct ssdparams *spp = &ssd->sp;
    /* 变量 len：写请求实际 sector 数，不是 byte 长度。 */
    int len = req->nlb;
    /* 变量 start_lpn：由 req.slba/secs_per_pg 得到的请求首 LPN。 */
    uint64_t start_lpn = lba / spp->secs_per_pg;
    /* 变量：
     * last_lpn：本 TP 的最后页或预取截断终点。
     * end_lpn：由 (req.slba+req.nlb-1)/secs_per_pg 得到的请求末 LPN，闭区间；请求可跨 TP。
     */
    uint64_t last_lpn, end_lpn = (lba + len - 1) / spp->secs_per_pg;
    /* 变量 cmt_entry：当前缓存映射槽，按 LPN 查找或从 free 池取得。 */
    struct cmt_entry *cmt_entry;
    // struct ppa ppa;
    /* 变量 ppa：本次物理地址；DATA/TP 及输入输出语义由本函数路径确定。 */
    struct ppa ppa;
    /* 变量 lpn：当前逻辑页号，DATA 映射索引。 */
    uint64_t lpn;
    /* 变量：
     * curlat：当前写页模拟完成延迟，ns。
     * maxlat：本请求所有页完成延迟的最大值，非逐页相加。
     */
    uint64_t curlat = 0, maxlat = 0;

    if (end_lpn >= spp->tt_pgs) {
        /* 调用 ftl_err：通过错误日志宏输出到 stderr；宏本身不终止函数或修复状态。 */
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    }

    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        curlat = 0;
        // clock_gettime(CLOCK_MONOTONIC, &time1);
        // 先通过 TP/group 索引定位数据 WP
        /* 阶段 2：每页由 TP 索引/group 下标选数据 WP，并校验 group 范围。 */
        /* 变量 gtd_index：逻辑页所属 TP 索引，通常为 LPN/ents_per_pg。 */
        int gtd_index = lpn/spp->ents_per_pg;
        /* 变量 wp_index：TP 索引/trans_per_line 得到的逻辑 group WP 下标。 */
        int wp_index = (int)(gtd_index/spp->trans_per_line);

        /* 调用 ftl_assert：只在 FEMU_DEBUG_FTL 下检查表达式；默认空宏，不求值也不提供错误恢复。 */
        ftl_assert(wp_index >= 0 && wp_index < spp->tt_line_wps);

        /* 调用 cmt_hit：查询 LPN 与所属 TP，并把命中的 TPnode/entry 移到各自热度队列头。 */
        cmt_entry = cmt_hit(ssd, lpn);
        /* 阶段 3：准备当前 CMT 项，缺失时加载旧 TP 或建立未映射槽。 */
        if (cmt_entry) {
            // ssd->stat.cmt_hit_cnt++;
        } else {
            last_lpn = (lpn / spp->ents_per_pg + 1) * spp->ents_per_pg - 1;
            last_lpn = (last_lpn < end_lpn) ? last_lpn : end_lpn;
            /* 调用 process_translation_page_write：处理写 CMT miss，为当前页取得缓存槽，并在 TP 已存在时加载/预取旧映射。 */
            process_translation_page_write(ssd, req, lpn, last_lpn);
            /* 调用 get_maptbl_ent：读取完整正向真值表中的 LPN→PPA。 */
            ppa = get_maptbl_ent(ssd, lpn);
        }

        /* 调用 get_maptbl_ent：读取完整正向真值表中的 LPN→PPA。 */
        ppa = get_maptbl_ent(ssd, lpn);

        /* 调用 find_hash_entry：按 LPN 遍历 CMT 哈希桶链，寻找已有缓存项。 */
        cmt_entry = find_hash_entry(&ssd->cm.ht, lpn);



        /* 阶段 4：旧 DATA 页失效并清旧 rmap，覆盖写不返还 rest。 */
        /* 调用 mapped_ppa：检查 packed PPA 是否不同于 UNMAPPED_PPA 哨兵。 */
        model_bitmap_set(ssd, lpn, 0);
        if (mapped_ppa(&ppa)) {
            /* 调用 mark_page_invalid：把有效页改为 INVALID，同时 block/line 的 ipc++、vpc--。 */
            mark_page_invalid(ssd, &ppa);
            /* 调用 set_rmap_ent：由 PPA 得到 PPN，将反向标识写入 rmap。 */
            set_rmap_ent(ssd, INVALID_LPN, &ppa);
            /* 随后的分配可能同步 GC；不要让 TP 写回保留这条已经失效的旧映射。 */
            struct ppa unmapped = { .ppa = UNMAPPED_PPA };
            set_maptbl_ent(ssd, lpn, &unmapped);
        }

        /* 阶段 5：首写分配 line，其他写先推进；随后取得当前坐标页。 */
        /* 变量 lwp：本页所属逻辑 group 的数据 WP 引用。 */
        struct write_pointer *lwp = &ssd->gtd_wps[wp_index];
        ppa = allocate_host_data_page(ssd, lwp);
        /* 写指针推进可能触发 GC/训练，须在此后清除即将失效的模型预测。 */
        model_bitmap_set(ssd, lpn, 0);
        /* 调用 set_maptbl_ent：把指定 PPA 按值写入完整 LPN 映射表。 */
        set_maptbl_ent(ssd, lpn, &ppa);
        /* 调用 ppa2pgidx：把物理地址展平为常规 PPN，供 rmap 和 CMT 使用。 */
        cmt_entry->ppn = ppa2pgidx(ssd, &ppa);
        cmt_entry->dirty = DIRTY;
        /* 调用 set_rmap_ent：由 PPA 得到 PPN，将反向标识写入 rmap。 */
        set_rmap_ent(ssd, lpn, &ppa);

        /* 调用 mark_page_valid：把已分配 FREE 页提交为 VALID，并使 block/line.vpc 各加 1。 */
        mark_page_valid(ssd, &ppa);

        /* 变量 swr：用于模拟 Host 数据 NAND_WRITE 的命令。 */
        struct nand_cmd swr;
        swr.type = USER_IO;
        swr.cmd = NAND_WRITE;
        swr.stime = cmt_entry->next_avail_time > req->stime ? cmt_entry->next_avail_time : req->stime;
        /* 模拟 Host DATA WRITE，取本请求各页最大完成延迟 */
        /* 调用 ssd_advance_status：模拟一个 NAND 读/写/擦命令的 LUN 排队，更新完成时间、写计数与估算能耗。 */
        curlat = ssd_advance_status(ssd, &ppa, &swr) + swr.stime - req->stime;
        maxlat = (curlat > maxlat) ? curlat : maxlat;
        // clock_gettime(CLOCK_MONOTONIC, &time2);
    }

    /* 阶段 7（论文 III-E1）：先清本请求的旧候选，再按 TP 和实际 VPPN 连续性切分。
     * 请求中途 GC 可能搬移早先写入的页，因此使用请求结束时的真实映射。
     */
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        model_bitmap_set(ssd, lpn, 0);
    }
    if (ssd->model_used) {
        uint64_t run_start = start_lpn;
        while (run_start <= end_lpn) {
            uint64_t tp_end = (run_start / spp->ents_per_pg + 1) * spp->ents_per_pg - 1;
            uint64_t limit = tp_end < end_lpn ? tp_end : end_lpn;
            uint64_t run_end = run_start;
            struct ppa first_ppa = get_maptbl_ent(ssd, run_start);
            uint64_t start_vppn = ppa2vppn(ssd, &first_ppa);

            while (run_end < limit) {
                struct ppa next_ppa = get_maptbl_ent(ssd, run_end + 1);
                if (ppa2vppn(ssd, &next_ppa) != start_vppn + run_end - run_start + 1) {
                    break;
                }
                run_end++;
            }
            if (run_end > run_start) {
                model_sequential_update(ssd, run_start, run_end, start_vppn);
                ssd->gtd_wps[run_start / spp->ents_per_pg / spp->trans_per_line].trained = true;
            }
            run_start = run_end + 1;
        }
    }

    // ssd->stat.write_time += (maxlat + (time2.tv_sec - time1.tv_sec)*1000000000 + (time2.tv_nsec - time1.tv_nsec));

    return maxlat;
}

/**
 * 功能：等待数据面启动，逐 poller 取请求，执行 FTL 读写并回送带延迟的完成请求。
 * 参数 arg：线程参数，实际为 FemuCtrl*。
 * 作用与边界：后台 GC 草案被注释；活跃 GC 同步发生在分配路径。默认 opcode 分支使用零延迟；ring 出队失败跳过、入队失败重试，线程没有退出/完整恢复流程。
 */
static void *ftl_thread(void *arg)
{
    /* 阶段 1：取得控制器/SSD，等待 dataplane_started 后绑定请求与完成 ring。 */
    /* 变量 n：外围 FemuCtrl 控制器引用；ssd 与请求 ring 的来源。 */
    FemuCtrl *n = (FemuCtrl *)arg;
    /* 变量 ssd：本设备的 FTL 总状态；映射、缓存、NAND 树、WP、模型与统计均挂在此对象。 */
    struct ssd *ssd = n->ssd;
    /* 变量 req：外围 NVMe 请求；nlb 已转换为实际 sector 数，不是原始零基 NLB。 */
    NvmeRequest *req = NULL;
    /* 变量 lat：上一次分支得到的请求延迟；每次处理请求前重置为零。 */
    uint64_t lat = 0;
    /* 变量 rc：请求 ring 出队/入队的成功数量，期望为 1。 */
    int rc;
    /* 变量 i：poller 编号，实际从 1 遍历到 num_poller。 */
    int i;

    // gc_fp = fopen("/home/astl/wsz/gc_frequency.txt", "w");

    while (!*(ssd->dataplane_started_ptr)) {
        /* 调用 usleep：暂睡给定微秒数，此处每次 100000 µs 等待数据面启动。 */
        usleep(100000);
    }

    /* 待修正：对 to_ftl/to_poller 的绑定和运行生命周期缺少完整安全处理 */
    ssd->to_ftl = n->to_ftl;
    ssd->to_poller = n->to_poller;

    while (1) {

        // tmp_counter++;
        // if (tmp_counter / 500000000 == 1) {
        //     counter++;
        //     // printf("%lld\n", (long long)counter);
        //     tmp_counter = 0;
        // }
        for (i = 1; i <= n->num_poller; i++) {
            /* 调用 femu_ring_count：读取请求 ring 当前元素数；为 0 时跳过该 poller。 */
            if (!ssd->to_ftl[i] || !femu_ring_count(ssd->to_ftl[i]))
                continue;

            /* 调用 femu_ring_dequeue：从当前 poller 的请求 ring 取 1 个 NvmeRequest 指针，rc 期望为 1。 */
            rc = femu_ring_dequeue(ssd->to_ftl[i], (void *)&req, 1);
            /* 阶段 2：按 poller 编号检查请求 ring，出队一项；错误仅记录，未完整恢复。 */
            if (rc != 1) {
                /* 调用 printf：输出原有诊断/计数信息；日志字符串保留，打印本身不会恢复异常状态。 */
                printf("FEMU: FTL to_ftl dequeue failed\n");
                continue;
            }

            

            /* 调用 ftl_assert：只在 FEMU_DEBUG_FTL 下检查表达式；默认空宏，不求值也不提供错误恢复。 */
            ftl_assert(req);
            lat = 0;
            switch (req->cmd.opcode) {
            case NVME_CMD_WRITE:
                /* 阶段 3：WRITE/READ 进入 FTL，DSM 设 0；未知 opcode 使用零延迟。 */
                /* 调用 ssd_write：按 LPN→TP→group 选择 WP，准备 CMT、失效旧页、分配新页并提交映射/写时序。 */
                lat = ssd_write(ssd, req);
                break;
            case NVME_CMD_READ:
                /* 调用 ssd_read：逐逻辑页先查 CMT，miss 后按 bitmap 尝试模型，失败再读 TP，最终模拟数据 READ。 */
                lat = ssd_read(ssd, req);
                break;
            case NVME_CMD_DSM:
                lat = 0;
                break;
            default:
                //ftl_err("FTL received unkown request type, ERROR\n");
                ;
            }
            /* 阶段 4：把模拟延迟加到 expire_time 并送回对应完成 ring。 */

            req->reqlat = lat;
            req->expire_time += lat;

            /* 调用 femu_ring_enqueue：把处理后的请求指针放入同一 poller 的完成 ring，rc 期望为 1。 */
            rc = femu_ring_enqueue(ssd->to_poller[i], (void *)&req, 1);
            if (rc != 1) {
                /* 调用 ftl_err：通过错误日志宏输出到 stderr；宏本身不终止函数或修复状态。 */
                ftl_err("FTL to_poller enqueue failed; retrying\n");
                while (femu_ring_enqueue(ssd->to_poller[i], (void *)&req, 1) != 1) {
                    usleep(1);
                }
            }

        /* 后台 GC 草案不执行；本线程内活跃 GC 在写指针分配/推进时同步触发。 */
            /* 旧后台回收草案：需要时清一条 line；下面调用被注释，当前不执行 */
            // if (should_gc(ssd)) {
            //     do_gc(ssd, false);
            // }
            
        }

        // struct line *vl = NULL;
        // struct write_pointer *wpp;
        // should_line_gc(ssd, vl, wpp);
        // if (vl) {
        //     line_do_gc(ssd, true, wpp, vl);
        // }
    }
    return NULL;
}

// #pragma GCC pop_options
