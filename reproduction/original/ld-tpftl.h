/* LearnedFTL 类型与常量的中文导读：字段按当前 .c 的活跃读写解释。
 * 同名数值哨兵不等于地址格式互换；PPA、PPN、VPPN 必须区分。
 * DRAM 模型参数更新与掉电持久化不同；本原型未实现完整元数据保存/恢复。
 * 预留/禁用字段如实标出，声明它们不等于对应策略已经实现。
 */

#ifndef _L_FEMU_FTL_H
/* 头文件保护宏，防止同一编译单元重复声明类型与常量。 */
#define _L_FEMU_FTL_H
/* 引入 FemuCtrl/NvmeRequest、QEMU 线程与间接队列宏、ring 和 pqueue 等类型依赖。 */
#include "../nvme.h"


/* 常量 INVALID_PPA：全 1 的 64 位无效物理地址哨兵；数值同 UNMAPPED_PPA。 */
#define INVALID_PPA     (~(0ULL))
/* 常量 INVALID_LPN：全 1 的无效逻辑标识，用于 free CMT/无反向映射。 */
#define INVALID_LPN     (~(0ULL))
/* 常量 UNMAPPED_PPA：全 1 的未映射物理地址哨兵；不是合法页号。 */
#define UNMAPPED_PPA    (~(0ULL))


/* 常量 CMT_HASH_SIZE：LPN 哈希桶数 24593，冲突由 entry.next 单链解决。 */
#define CMT_HASH_SIZE (24593ULL)
/* 常量 TP_HASH_SIZE：TP 哈希桶数 24593，冲突由 TPnode.next 单链解决。 */
#define TP_HASH_SIZE (24593ULL)
/* 常量 ENT_PER_TP：TPnode.exist_ent 的声明容量 2048；运行映射页默认 512 条。 */
#define ENT_PER_TP (2048ULL)
/* 常量 GC_THRESH：遗留阈值宏 5；活跃 .c 用独立变量 gc_threshold。 */
#define GC_THRESH 5


enum {
    /* 常量 NAND_READ：NAND 读操作码 0。 */
    NAND_READ =  0,
    /* 常量 NAND_WRITE：NAND 页编程操作码 1。 */
    NAND_WRITE = 1,
    /* 常量 NAND_ERASE：NAND block 擦操作码 2。 */
    NAND_ERASE = 2,

    /* 常量 NAND_READ_LATENCY：默认基础读时延 40000 ns = 40 µs。 */
    NAND_READ_LATENCY = 40000,
    /* 常量 NAND_PROG_LATENCY：默认基础编程时延 200000 ns = 200 µs。 */
    NAND_PROG_LATENCY =  200000,
    /* 常量 NAND_ERASE_LATENCY：默认基础擦时延 2000000 ns = 2 ms。 */
    NAND_ERASE_LATENCY = 2000000,
};

enum {
    /* 常量 CLEAN：缓存映射干净状态 0；不证明真实元数据已持久化。 */
    CLEAN = 0,
    /* 常量 DIRTY：Host 修改映射的脏状态 1，淘汰时聚合更新 TP。 */
    DIRTY = 1
};

enum {
    /* 常量 HEAD：entry 插入 TP 热度队列头，最热位置。 */
    HEAD = 0,
    /* 常量 TAIL：entry 插入 TP 热度队列尾，较冷位置。 */
    TAIL = 1
};

enum {
    /* 常量 USER_IO：Host/翻译辅助命令来源标志 0。 */
    USER_IO = 0,
    /* 常量 GC_IO：GC 命令来源标志 1。 */
    GC_IO = 1,
};

enum {
    /* 常量 GTD：元数据 line 类型 0；其页 rmap 保存 TP 索引。 */
    GTD = 0,
    /* 常量 DATA：用户数据 line 类型 1；其页 rmap 保存 DATA LPN。 */
    DATA = 1,
    /* 常量 UNUSED：初始未使用类型 2；mark_line_free 不重置此类型。 */
    UNUSED = 2,
};

enum {
    /* 常量 SEC_FREE：sector 初始 FREE 状态 0。 */
    SEC_FREE = 0,
    /* 常量 SEC_INVALID：sector 无效状态 1；活跃页写主要维护 page.status。 */
    SEC_INVALID = 1,
    /* 常量 SEC_VALID：sector 有效状态 2；活跃页写主要维护 page.status。 */
    SEC_VALID = 2,

    /* 常量 PG_FREE：可编程页状态 0。 */
    PG_FREE = 0,
    /* 常量 PG_INVALID：旧映射失效的页状态 1；只有 block 清理才回到 FREE。 */
    PG_INVALID = 1,
    /* 常量 PG_VALID：当前有效页状态 2，vpc 统计这些页。 */
    PG_VALID = 2
};

enum {
    /* 常量 FEMU_ENABLE_GC_DELAY：管理子命令 1：开启 GC NAND 时序模拟。 */
    FEMU_ENABLE_GC_DELAY = 1,
    /* 常量 FEMU_DISABLE_GC_DELAY：管理子命令 2：关闭 GC 时序模拟，状态回收仍执行。 */
    FEMU_DISABLE_GC_DELAY = 2,

    /* 常量 FEMU_ENABLE_DELAY_EMU：管理子命令 3：开启基础 NAND 时延模拟。 */
    FEMU_ENABLE_DELAY_EMU = 3,
    /* 常量 FEMU_DISABLE_DELAY_EMU：管理子命令 4：关闭基础 NAND 时延模拟。 */
    FEMU_DISABLE_DELAY_EMU = 4,

    /* 常量 FEMU_RESET_ACCT：管理子命令 5：重置外围控制器 I/O 账户计数。 */
    FEMU_RESET_ACCT = 5,
    /* 常量 FEMU_ENABLE_LOG：管理子命令 6：开启外围日志。 */
    FEMU_ENABLE_LOG = 6,
    /* 常量 FEMU_DISABLE_LOG：管理子命令 7：关闭外围日志。 */
    FEMU_DISABLE_LOG = 7,
    /* 常量 FEMU_RESET_STAT：管理子命令 8：调用外围统计重置。 */
    FEMU_RESET_STAT = 8,
    /* 常量 FEMU_PRINT_STAT：管理子命令 9：输出外围统计。 */
    FEMU_PRINT_STAT = 9,
    /* 常量 FEMU_MODEL：模型开启子命令枚举 10；当前 bb_flip 无对应 case，不能当作可用开关。 */
    FEMU_MODEL = 10,
    /* 常量 FEMU_MODEL_UNUSE：模型关闭子命令枚举 11；当前 bb_flip 无对应 case。 */
    FEMU_MODEL_UNUSE = 11
};


/* 常量 BLK_BITS：PPA.blk 位宽 16。 */
#define BLK_BITS    (16)
/* 常量 PG_BITS：PPA.pg 位宽 16。 */
#define PG_BITS     (16)
/* 常量 SEC_BITS：PPA.sec 位宽 8。 */
#define SEC_BITS    (8)
/* 常量 PL_BITS：PPA.pl 位宽 8。 */
#define PL_BITS     (8)
/* 常量 LUN_BITS：PPA.lun 位宽 8。 */
#define LUN_BITS    (8)
/* 常量 CH_BITS：PPA.ch 位宽 7，另有 1 bit rsv，合计 64 bit。 */
#define CH_BITS     (7)

/* 禁用的旧缓存容量候选；当前 CMT 容量在 ssd_init_params 设置为 8192，未读取 CMT_NUM。 */
// #define CMT_NUM 16*1024
/* 禁用的旧缓存容量候选；当前 CMT 容量在 ssd_init_params 设置为 8192，未读取 CMT_NUM。 */
// #define CMT_NUM 4*1024
/* 常量 MAX_INTERVALS：每 TP 模型固定八段；不是误差自适应段数。 */
#define MAX_INTERVALS 8         // ! 模型参数：一个模型中包含几个段
/* 常量 INTERVAL_NUM：遗留宏 60，活跃训练未读取；实际每段数由 n/8 计算。 */
#define INTERVAL_NUM 60         // ! 模型参数：忘了，没啥用应该，后面没用到
/* 常量 TRAIN_THRESHOLD：拟合阈值 30，条件严格 n>30；n<=30 仍搬移有效页。 */
#define TRAIN_THRESHOLD 30      // ! 模型参数：对于整个模型，当有多少有效数据时进行模型训练

/* 结构 lr_breakpoint：一段线性模型：相对 VPPN=y=w*x+b，x 为相对 LPN；key 是该段右边界。 */
typedef struct lr_breakpoint {
    /* 字段 w：斜率；初始化 1，GC 拟合更新；预测读取。 */
    float w;
    /* 字段 b：截距；初始化 0，GC 拟合更新；预测读取。 */
    float b;
    /* 字段 key：相对 LPN 的右边界；预测取第一个 key>=x 的段。 */
    int key;
    /* 字段 valid_cnt：本段评估准确的样本数；初始化 0，非段跨度或总训练数。 */
    int valid_cnt;
}lr_breakpoint;

/* 结构 lr_node：一个 TP 对应的常驻 DRAM 八段模型；按 GTD 同一索引访问，不是按需载入的模型缓存。 */
typedef struct lr_node {

    /* 字段 brks：固定 MAX_INTERVALS=8 段；GC 原位更新，顺序写尾部只改局部副本。 */
    lr_breakpoint brks[MAX_INTERVALS];
    /* 字段 start_lpn：x 基准：GC 收集并排序后首个有效 LPN。 */
    uint64_t start_lpn;
    /* 字段 start_ppa：y 基准：首个新 VPPN，名字 ppa 不代表 packed 地址。 */
    uint64_t start_ppa;
    /* 字段 u：是否允许尝试的标志；初值 1，GC 也置 1，不能证明已准确训练。 */
    uint8_t u;
    /* 字段 less：预留模型标志；初始化/GC 置 0，活跃读取未使用。 */
    uint8_t less;
    /* 字段 success_ratio：本 TP 准确数/实际评估数；不以收集样本数或 512 为统一分母。 */
    float success_ratio;
}lr_node;

/* 结构 cmt_entry：一个固定 CMT 缓存槽，保存 LPN→普通 PPN、脏状态与预取依赖；读寻址仍用 maptbl 真值。 */
typedef struct cmt_entry {
    /* 字段 lpn：该槽逻辑页号；free 时 INVALID_LPN。 */
    uint64_t lpn;
    /* 字段 ppn：普通 PPN；Host 写更新，GC 未同步已有槽；free 时 UNMAPPED_PPA。 */
    uint64_t ppn;
    /* 字段 dirty：CLEAN/DIRTY；插入 CLEAN，Host 写 DIRTY，脏淘汰聚合写 TP。 */
    int dirty;
    // int hotness;
    /* 字段 entry：双向队列链接；空闲时在 free 槽队列，使用时在 TPnode.entry 队列。 */
    QTAILQ_ENTRY(cmt_entry) entry;
    /* 字段 prefetch：是否预取加载；读 hit 为 true 时等 next_avail_time。 */
    bool prefetch;
    /* 字段 next_avail_time：加载该映射的翻译读完成时间 ns；free 为 0。 */
    uint64_t next_avail_time;
    /* 字段 next：哈希桶冲突链的下一 entry；独立于 QTAILQ 链接。 */
    struct cmt_entry *next;    /* 哈希冲突单链链接；不同于 QTAILQ 热度链接 */
} cmt_entry;

/* 结构 TPnode：一个 TP 的缓存组织节点；全局 TP 热度队列与其内部 entry 热度队列构成两层组织。 */
typedef struct TPnode {
    /* 字段 tvpn：TP 索引 LPN/ents_per_pg；不同于原始 LPN/group id。 */
    uint64_t tvpn;
    /* 字段 cmt_entry_cnt：此 TP 已缓存 entry 数，插入加、淘汰减；0 时释放节点。 */
    int cmt_entry_cnt;
    // double hotness;  /* The paper didn't explain how to operate and set hotness */
    /* 字段 entry：全局 TP 热度队列链接，头热尾冷。 */
    QTAILQ_ENTRY(TPnode) entry;
    /* 校正原中文注释：实际 entry 队头为热端，队尾为冷端；HEAD/TAIL 插入由 .c 明确决定。 */
    //QTAIQ_HEAD 为热度最低的
    /* 字段 cmt_entry_list：此 TP 内 entry 的热度队列，头热尾冷。 */
    QTAILQ_HEAD(cmt_entry_list, cmt_entry) cmt_entry_list;
    /* 字段 next：TP 哈希冲突链下一节点。 */
    struct TPnode *next;   /* 哈希冲突单链链接；不同于 QTAILQ 热度链接 */
    /* 字段 exist_ent：缓存存在标记，声明 2048 个 short，默认只用前 512；不是模型准确性 bitmap。 */
    short exist_ent[ENT_PER_TP];
} TPnode;


/* 结构 hash_table：活跃 CMT 的双哈希表：一张按 LPN 查 entry，另一张按 TP 索引查 TPnode。 */
typedef struct hash_table {
    /* 字段 cmt_table：CMT_HASH_SIZE 个 LPN 桶头，entry.next 解决冲突。 */
    cmt_entry *cmt_table[CMT_HASH_SIZE];
    /* 字段 tp_table：TP_HASH_SIZE 个 TP 桶头，TPnode.next 解决冲突。 */
    TPnode *tp_table[TP_HASH_SIZE];
}hash_table;

/* 结构 cmt_mgmt：CMT 固定槽池、空闲槽队列、TP 热度队列、容量计数和哈希表的管理器。 */
struct cmt_mgmt {
    /* 字段 cmt_entries：固定数组槽池；entry 淘汰归还槽，不释放数组元素。 */
    cmt_entry *cmt_entries;
    /* 字段 free_cmt_entry_list：空闲缓存槽双向队列，初始化全部入队。 */
    QTAILQ_HEAD(free_cmt_entry_list, cmt_entry) free_cmt_entry_list;
    //QTAIQ_HEAD 为热度最高的
    /* 字段 TPnode_list：已缓存 TP 的全局热度队列；命中移头，淘汰取尾。 */
    QTAILQ_HEAD(TPnode_list, TPnode) TPnode_list;
    /* 字段 tt_TPnodes：当前已分配 TPnode 数，创建/删除时维护。 */
    int tt_TPnodes;
    /* 字段 tt_entries：CMT 总槽容量，从 sp.tt_cmt_size 设置。 */
    int tt_entries;
    /* 字段 free_cmt_entry_cnt：当前空闲槽数；插入减，淘汰加。 */
    int free_cmt_entry_cnt;
    /* 字段 used_cmt_entry_cnt：当前已用槽数；插入加，淘汰减。 */
    int used_cmt_entry_cnt;
    // 用于调节选择性预取的创建/删除事件计数
    /* 字段 counter：TPnode 创建加/删除减；到 +3 关闭选择预取，到 -3 开启并复位。 */
    int counter;
    /* 字段 ht：活跃双哈希表；不是 ssd.cmt 的遗留 ht。 */
    struct hash_table ht;
};

/* 物理页地址的字段视图与 64 位 packed 视图 */
/* 结构 ppa：64 位 packed 物理地址与几何位域共享存储；packed 整数不是常规 PPN/VPPN。 */
struct ppa {
    union {
        struct {
            /* 字段 blk：block 编号，默认等于 line id，16 bit。 */
            uint64_t blk : BLK_BITS;
            /* 字段 pg：block 内页编号，16 bit。 */
            uint64_t pg  : PG_BITS;
            /* 字段 sec：page 内 sector 编号，8 bit；页分配清为 0，PPN/VPPN 不计它。 */
            uint64_t sec : SEC_BITS;
            /* 字段 pl：plane 编号，8 bit；当前主要使用 plane 0。 */
            uint64_t pl  : PL_BITS;
            /* 字段 lun：channel 内 LUN 编号，8 bit。 */
            uint64_t lun : LUN_BITS;
            /* 字段 ch：channel 编号，7 bit。 */
            uint64_t ch  : CH_BITS;
            /* 字段 rsv：保留位，1 bit；get_new_line_page 清零，vppn2ppa 未显式初始化。 */
            uint64_t rsv : 1;
        /* 字段 g：物理几何字段视图。 */
        } g;

        /* 字段 ppa：64 位 packed 地址视图，UNMAPPED 用全 1 哨兵。 */
        uint64_t ppa;
    };
};

/* sector 状态类型别名，用 SEC_FREE/INVALID/VALID 枚举值；活跃页写主要维护 page.status。 */
typedef int nand_sec_status_t;

/* 结构 nand_page：NAND 页的状态元数据，不包含真实数据 payload。 */
struct nand_page {
    /* 字段 sec：sector 状态数组，初始化 SEC_FREE；活跃页写/块清理主要更新 page.status。 */
    nand_sec_status_t *sec;
    /* 字段 nsecs：该页 sector 个数，默认 8。 */
    int nsecs;
    /* 字段 status：PG_FREE/PG_INVALID/PG_VALID；分配后由 mark_page_valid 提交。 */
    int status;
};

/* 结构 nand_block：NAND 擦除单元的页状态数组及有效/无效页、擦除计数。 */
struct nand_block {
    /* 字段 pg：npgs 个页状态对象数组。 */
    struct nand_page *pg;
    /* 字段 npgs：每 block 页数，默认 512。 */
    int npgs;
    /* 字段 ipc：无效页数；旧页失效加，block 清理设 0。 */
    int ipc; /* 无效页数量 */
    /* 字段 vpc：有效页数；valid 加、invalid 减、block 清理设 0。 */
    int vpc; /* 有效页数量 */
    /* 字段 erase_cnt：每次 mark_block_free 加 1；可能和已模拟 NAND_ERASE 次数不同。 */
    int erase_cnt;
    /* 字段 wp：旧 block 内写游标，初始化 0；活跃分配用 group/trans WP。 */
    int wp; /* 旧 block 内写指针；实际业务使用 group/trans WP */
};

/* 结构 nand_plane：NAND plane 的 block 数组；当前活跃 line/WP 逻辑以单 plane 为主要假设。 */
struct nand_plane {
    /* 字段 blk：nblks 个 block 对象数组。 */
    struct nand_block *blk;
    /* 字段 nblks：每 plane block 数，默认 256。 */
    int nblks;
};

/* 结构 nand_lun：单个 LUN/Die 的 plane 树与模拟排队时间，是当前活跃 NAND 时序的串行单元。 */
struct nand_lun {
    /* 字段 pl：npls 个 plane 对象数组。 */
    struct nand_plane *pl;
    /* 字段 npls：每 LUN plane 数，默认 1。 */
    int npls;
    /* 字段 next_lun_avail_time：该 LUN 最早可开始下一命令的绝对 ns 时间；时序/翻译依赖更新。 */
    uint64_t next_lun_avail_time;
    /* 字段 busy：预留忙标志，初始化 false；当前时序不读写。 */
    bool busy;
    /* 字段 gc_endtime：记录该 LUN GC 操作后的可用时间 ns；不是独立调度器。 */
    uint64_t gc_endtime;
};

/* 结构 ssd_channel：一个 channel 的 LUN 树；channel 传输时序代码当前被 #if 0 禁用。 */
struct ssd_channel {
    /* 字段 lun：nluns 个 LUN 对象数组。 */
    struct nand_lun *lun;
    /* 字段 nluns：每 channel LUN 数，默认 8。 */
    int nluns;
    /* 字段 next_ch_avail_time：channel 最早可用 ns 时间；初始化 0，传输更新草案被禁用。 */
    uint64_t next_ch_avail_time;
    /* 字段 busy：预留 channel 忙标志，初始化 0；活跃时序未维护。 */
    bool busy;
    /* 字段 gc_endtime：预留 channel GC 完成时间，更新草案被 #if 0 禁用。 */
    uint64_t gc_endtime;
};

/* 结构 ssdparams：基础 NAND 几何、ns 时延、派生计数、映射/group/缓存参数和 VPPN 步长。 */
struct ssdparams {
    /* 字段 secsz：每 sector 字节数，默认 512。 */
    int secsz;        /* sector 字节数 */
    /* 字段 secs_per_pg：每 page 的 sector 数，默认 8。 */
    int secs_per_pg;  /* # of sectors per page */
    /* 字段 pgs_per_blk：每 block 页数，默认 512。 */
    int pgs_per_blk;  /* # of NAND pages per block */
    /* 字段 blks_per_pl：每 plane block 数，默认 256。 */
    int blks_per_pl;  /* # of blocks per plane */
    /* 字段 pls_per_lun：每 LUN plane 数，默认 1；改变时需校正单 plane 假设。 */
    int pls_per_lun;  /* # of planes per LUN (Die) */
    /* 字段 luns_per_ch：每 channel LUN 数，默认 8。 */
    int luns_per_ch;  /* # of LUNs per channel */
    /* 字段 nchs：SSD channel 数，默认 8。 */
    int nchs;         /* # of channels in the SSD */

    /* 字段 pg_rd_lat：NAND 页读基础时延 ns，默认 40000。 */
    int pg_rd_lat;    /* NAND 页读基础时延，ns */
    /* 字段 pg_wr_lat：NAND 页编程基础时延 ns，默认 200000。 */
    int pg_wr_lat;    /* NAND 页编程基础时延，ns */
    /* 字段 blk_er_lat：block 擦除基础时延 ns，默认 2000000。 */
    int blk_er_lat;   /* NAND block 擦除基础时延，ns */
    /* 字段 ch_xfer_lat：单页 channel 传输基础时延 ns，默认 0；传输分支被禁用。 */
    int ch_xfer_lat;  /* 单页 channel 传输基础时延，ns，用于表示 channel 带宽；当前传输模拟分支被禁用。 */

    /* 字段 gc_thres_pcent：旧全局 GC 使用比例 0.75；活跃 WP 调度不用此比例。 */
    double gc_thres_pcent;
    /* 字段 gc_thres_lines：(1-gc_thres_pcent)*tt_lines，默认 64；旧 should_gc 检查。 */
    int gc_thres_lines;
    /* 字段 gc_thres_pcent_high：旧高压力使用比例 0.95；活跃调度未读取。 */
    double gc_thres_pcent_high;
    /* 字段 gc_thres_lines_high：旧高压力 free line 数阈值，默认 12；活跃路径未读取。 */
    int gc_thres_lines_high;
    /* 字段 enable_gc_delay：是否模拟活跃 GC 读/写/擦的 NAND 时序；关掉不停止状态搬移。 */
    bool enable_gc_delay;

    /* 以下为根据基础几何推导的数量 */
    /* 字段 secs_per_blk：secs_per_pg*pgs_per_blk。 */
    int secs_per_blk; /* # of sectors per block */
    /* 字段 secs_per_pl：secs_per_blk*blks_per_pl。 */
    int secs_per_pl;  /* # of sectors per plane */
    /* 字段 secs_per_lun：secs_per_pl*pls_per_lun。 */
    int secs_per_lun; /* # of sectors per LUN */
    /* 字段 secs_per_ch：secs_per_lun*luns_per_ch。 */
    int secs_per_ch;  /* # of sectors per channel */
    /* 字段 tt_secs：secs_per_ch*nchs，按物理几何总量计算。 */
    int tt_secs;      /* # of sectors in the SSD */

    /* 字段 pgs_per_pl：pgs_per_blk*blks_per_pl。 */
    int pgs_per_pl;   /* # of pages per plane */
    /* 字段 pgs_per_lun：pgs_per_pl*pls_per_lun。 */
    int pgs_per_lun;  /* # of pages per LUN (Die) */
    /* 字段 pgs_per_ch：pgs_per_lun*luns_per_ch。 */
    int pgs_per_ch;   /* # of pages per channel */
    /* 字段 tt_pgs：pgs_per_ch*nchs，默认 8388608 页，完整表的数组长度。 */
    int tt_pgs;       /* SSD 物理总页数 */

    /* 字段 blks_per_lun：blks_per_pl*pls_per_lun。 */
    int blks_per_lun; /* # of blocks per LUN */
    /* 字段 blks_per_ch：blks_per_lun*luns_per_ch。 */
    int blks_per_ch;  /* # of blocks per channel */
    /* 字段 tt_blks：blks_per_ch*nchs，默认 16384 blocks。 */
    int tt_blks;      /* SSD 总 block 数 */

    /* 字段 secs_per_line：pgs_per_line*secs_per_pg。 */
    int secs_per_line;
    /* 字段 pgs_per_line：blks_per_line*pgs_per_blk，默认 32768 页。 */
    int pgs_per_line;
    /* 字段 blks_per_line：当前取 tt_luns，默认 64；多 plane 待适配。 */
    int blks_per_line;
    /* 字段 tt_lines：当前取 blks_per_lun，默认 256；多 plane 时与 init_lines 的 blks_per_pl 不一致。 */
    int tt_lines;

    /* 字段 pls_per_ch：pls_per_lun*luns_per_ch。 */
    int pls_per_ch;   /* # of planes per channel */
    /* 字段 tt_pls：pls_per_ch*nchs。 */
    int tt_pls;       /* SSD 总 plane 数 */

    /* 字段 tt_luns：luns_per_ch*nchs，默认 64；GC/训练第一维也使用它。 */
    int tt_luns;      /* SSD 总 LUN 数 */


    //LearnedFTL 扩展部分
    /* 字段 pg_size：secsz*secs_per_pg，默认 4096 字节。 */
    int pg_size;
    /* 字段 addr_size：单条映射地址字节数，当前 8。 */
    int addr_size;
    /* 字段 addr_per_pg：遗留参数声明，当前初始化和业务均未使用。 */
    int addr_per_pg;
    /* 字段 tt_trans_pgs：tt_pgs/ents_per_pg，默认 16384 张 TP。 */
    int tt_trans_pgs; 
    /* 字段 tt_line_wps：tt_trans_pgs/trans_per_line，默认 256 个数据 WP；实际只初始化 240。 */
    int tt_line_wps;
    /* 字段 trans_per_line：每个逻辑 group 的 TP 数=pgs_per_line/ents_per_pg，默认 64，不是物理 TP 写入页数。 */
    int trans_per_line;

    /* 字段 ents_per_pg：pg_size/addr_size，默认每 TP 512 条映射。 */
    int ents_per_pg;
    /* 字段 tt_cmt_size：固定 CMT 槽数，当前 8192，约物理页数的 0.0977%。 */
    int tt_cmt_size;
    /* 字段 tt_gtd_size：tt_pgs/ents_per_pg，默认 16384 个 GTD/模型项。 */
    int tt_gtd_size;


    // VPPN 展平的维度步长
    /* 字段 chn_per_lun：VPPN 的 lun 权重=nchs。 */
    int chn_per_lun;
    /* 字段 chn_per_pl：VPPN 的 plane 权重=nchs*luns_per_ch。 */
    int chn_per_pl;
    /* 字段 chn_per_pg：VPPN 的 page 权重=nchs*luns_per_ch*pls_per_lun。 */
    int chn_per_pg;
    /* 字段 chn_per_blk：VPPN 的 block 权重=chn_per_pg*pgs_per_blk。 */
    int chn_per_blk;

    /* 字段 enable_request_prefetch：同一 TP/request 范围的映射预取开关，默认 true。 */
    bool enable_request_prefetch;
    /* 字段 enable_select_prefetch：基于前方连续缓存槽计数的额外预取开关，TPnode 创建/删除事件可切换。 */
    bool enable_select_prefetch;

};

/* 结构 line：跨 LUN 的分配/回收单元；默认同 block id 的 64 个 block，共 32768 页，非一个 NAND block。 */
typedef struct line {
    /* 字段 id：共同 block id，数组索引固定不变。 */
    int id;  /* line id 等于构成它的各 LUN block id */
    /* 字段 ipc：整 line 无效页数，invalid 加、free 清零。 */
    int ipc; /* 该 line 的无效页数 */
    /* 字段 vpc：整 line 有效页数，valid 加、invalid 减、free 清零。 */
    int vpc; /* 该 line 的有效页数 */
    /* 字段 type：GTD/DATA/UNUSED；分配时设，mark_line_free 未重置 UNUSED。 */
    int type;

    // 该 line 还剩多少可分配物理页槽
    /* 字段 rest：尚未分配的页槽数；get_new_line_page 减，free 恢复；覆盖失效不加。 */
    int rest;
    
    /* 字段 entry：一个全局双向队列链接，只能属于 free/victim/full 之一。 */
    QTAILQ_ENTRY(line) entry; /* 全局 free/victim/full 链接，一个 entry 不能同时加入多条这些队列 */
    /* 旧 victim 优先队列堆位置；活跃路径未维护 */
    /* 字段 pos：旧 victim pqueue 堆位置；活跃 FIFO victim 路径未维护。 */
    size_t                  pos;
} line;

// *  modify ====================
/* 结构 wp_lines：独立所有权单链节点，引用 line 实体；哨兵 line=NULL，历史/current 都在哨兵后。 */
struct wp_lines{
    /* 字段 line：所属物理 line 的实体引用，哨兵为 NULL。 */
    struct line *line;
    /* 字段 next：下一所有权引用，新节点插哨兵后；GC 清理时仅释放引用节点。 */
    struct wp_lines *next;
};

/* WP 保存分配坐标；DATA 常停在最后写页，translation 常指下一空页 */
/* 结构 write_pointer：DATA group 或独立 translation 的分配游标及多 line 所有权；不等同于物理区域本身。 */
struct write_pointer {
    /* 字段 curline：当前物理分配 line；数据 WP 初始 NULL、首次按需取 free line。 */
    struct line *curline;
    /* 字段 wpl：空哨兵的 ownership 单链，含 current 和历史 line。 */
    struct wp_lines *wpl;
    /* 字段 vic_cnt：所属实际 line 节点数，含 current；分配加、GC 减、批量清链设 1。 */
    int vic_cnt;
    /* 字段 ch：WP 当前 channel 坐标，最先轮转。 */
    int ch;
    /* 字段 lun：channel 轮转完后增加的 LUN 坐标。 */
    int lun;
    /* 字段 pg：所有 LUN 轮转完后增加的 block 内页层。 */
    int pg;
    /* 字段 blk：当前 line.id；不通过 blk++ 取得后续 line。 */
    int blk;
    /* 字段 pl：当前 plane 坐标，当前固定 0。 */
    int pl;
    /* 字段 id：数据 group 下标；trans_wp 设置 tt_lines，默认 256。 */
    int id;

    // 待完成：跨组借用 line 数；当前活跃代码未维护
    /* 字段 invade_lines：跨组借用 line 数的待办字段，当前无活跃维护。 */
    int invade_lines;
};

/* 结构 line_statistic：遗留 line 统计声明；本文件活跃路径未使用此结构。 */
struct line_statistic {
    /* 字段 rest：预留剩余页槽统计，当前此结构未使用。 */
    int rest;
};

/* 结构 line_mgmt：设备范围的 line 实体数组和全局 free/victim 队列；与每 WP 的 ownership 单链独立。 */
struct line_mgmt {
    /* 字段 lines：tt_lines 个稳定 line 实体数组，队列只链接这些实体。 */
    struct line *lines;
    /* free line 队列，line 实体通过共同 block id 标识 */
    /* 字段 free_line_list：尚未分配/已回收的 line 队列，分配取头、回收加尾。 */
    QTAILQ_HEAD(free_line_list, line) free_line_list;
    /* 字段 victim_line_pq：初始化的旧优先队列；活跃插入/更新/选择代码已注释。 */
    pqueue_t *victim_line_pq;
    //QTAILQ_HEAD(victim_line_list, line) victim_line_list;
    /* 字段 full_line_list：初始化的旧 full 队列；活跃 line 写满进入 victim_list。 */
    QTAILQ_HEAD(full_line_list, line) full_line_list;
    /* 字段 victim_list：活跃 FIFO 候选队列，full 或主动封存 current 可入队。 */
    QTAILQ_HEAD(victim_list, line) victim_list;
    /* 字段 tt_lines：全局 line 实体数，当前从 blks_per_pl 设置。 */
    int tt_lines;
    /* 字段 free_line_cnt：free 队列数量，分配减、回收加。 */
    int free_line_cnt;
    /* 字段 victim_line_cnt：全局 victim 队列数量，封存加、mark_line_free 减；非 WP.vic_cnt。 */
    int victim_line_cnt;
    /* 字段 full_line_cnt：旧 full 队列计数，初始化 0，活跃路径未维护。 */
    int full_line_cnt;
};

/* 结构 nand_cmd：一次 NAND 时序命令：USER/GC 来源、读/写/擦操作及起点 ns。 */
struct nand_cmd {
    /* 字段 type：命令来源 USER_IO/GC_IO；当前两类 WRITE 基础时延相同。 */
    int type;
    /* 字段 cmd：操作码 NAND_READ/NAND_WRITE/NAND_ERASE。 */
    int cmd;
    /* 字段 stime：命令起点 ns，0 取当前 QEMU 时钟；Host 数据请求用 req.stime。 */
    int64_t stime; /* 命令起点，ns；0 表示使用当前 QEMU 时钟 */
};


// 遗留 DFTL 哈希结构；当前 LearnedFTL 使用 ssd.cm.ht，非此结构
/* 结构 ht：遗留 DFTL 哈希结构；ssd.cmt 字段使用此类型，但当前 LearnedFTL 未读写其中成员。 */
struct ht {
    /* 字段 max_depth：遗留 DFTL 字段 最大深度；当前 LearnedFTL 未读写此成员。 */
    int max_depth;
    /* 字段 left_num：遗留 DFTL 字段 剩余数量；当前 LearnedFTL 未读写此成员。 */
    int left_num;
    /* 字段 global_version：遗留 DFTL 字段 全局版本；当前 LearnedFTL 未读写此成员。 */
    int global_version;
    /* 字段 entry：遗留 DFTL 字段 映射 PPA 数组；当前 LearnedFTL 未读写此成员。 */
    struct ppa *entry;
    /* 字段 lpns：遗留 DFTL 字段 LPN 数组；当前 LearnedFTL 未读写此成员。 */
    uint64_t *lpns;
    /* 字段 version：遗留 DFTL 字段 项版本数组；当前 LearnedFTL 未读写此成员。 */
    uint32_t *version;
    /* 字段 dirty：遗留 DFTL 字段 脏标志数组；当前 LearnedFTL 未读写此成员。 */
    uint8_t *dirty;

};

/* 结构 statistics：模拟事件统计；变量名不能替代计数定义。未维护字段标为预留，能耗常量未校准单位。 */
struct statistics {
    /* 字段 cmt_hit_cnt：有效地址的读 CMT hit 页数；写 lookup 不计，读地址无效会撤回。 */
    uint64_t cmt_hit_cnt;
    /* 字段 cmt_miss_cnt：读模型未被接受后进入 fallback 的页数；不含模型成功的 CMT miss，无效地址会撤回。 */
    uint64_t cmt_miss_cnt;
    /* 字段 cmt_hit_ratio：预留命中比例字段，初始 0；当前核心未实时计算。 */
    double cmt_hit_ratio;
    /* 字段 access_cnt：逐页读入口加，跳过无效地址减；不是 Host 请求数/写访问数。 */
    uint64_t access_cnt;
    /* 字段 model_hit_num：读 CMT miss 且 bitmap=1 的候选数，发生在 u/model_used 检查前；非成功数。 */
    uint64_t model_hit_num;
    /* 字段 model_use_num：model_predict 调用次数；非成功数。 */
    uint64_t model_use_num;
    /* 字段 model_out_range：预测不符且候选 rmap 为 INVALID_LPN，或 CMT hit 真值地址无效；非全部预测失败。 */
    uint64_t model_out_range;
    /* 字段 max_lpn：预留最大 LPN 统计，当前未维护。 */
    uint64_t max_lpn;
    /* 字段 min_lpn：预留最小 LPN 统计，当前未维护。 */
    uint64_t min_lpn;
    /* 字段 req_num：预留请求数，当前未维护。 */
    uint64_t req_num;
    /* 字段 average_lat：预留平均时延，当前未维护。 */
    long double average_lat;
    /* 字段 gc_times：batch_line_do_gc 每回收一条历史 DATA line 加 1；遗漏 single DATA/GTD，不是 GC 调用数。 */
    uint64_t gc_times;
    /* 字段 write_num：ssd_advance_status 中实际 NAND_WRITE 分支次数；翻译写时序被注释而漏计。 */
    uint64_t write_num;
    /* 字段 line_gc_times：512 槽的按物理 line.id 批量 DATA 回收计数，默认实际只需 256。 */
    uint64_t line_gc_times[512];
    /* 字段 wp_victims：512 槽的按 DATA WP.id 批量回收历史 line 数。 */
    uint64_t wp_victims[512];
    /* 字段 trans_wp_gc_times：预留 GTD GC 计数，当前活跃 GTD GC 未更新。 */
    uint64_t trans_wp_gc_times;
    /* 字段 line_wp_gc_times：批量 DATA GC 每回收一条历史 line 加 1，非 WP/调用数。 */
    uint64_t line_wp_gc_times;
    /* 字段 calculate_time：预留拟合 CPU 时间 ns，计时代码被注释。 */
    long long calculate_time;
    /* 字段 sort_time：预留排序 CPU 时间 ns，计时代码被注释。 */
    long long sort_time;
    /* 字段 predict_time：预留预测 CPU 时间，当前未计时。 */
    long long predict_time;
    /* 字段 GC_time：收集函数每 block 累加运行最大模拟擦延迟 ns；非真实 GC 墙钟。 */
    long long GC_time;
    /* 字段 write_time：预留 Host 写时间，累计代码被注释。 */
    long long write_time;
    /* 字段 read_time：预留 Host 读时间，累计代码被注释。 */
    long long read_time;
    /* 字段 model_training_nums：model_training 调用次数，包括只搬移未拟合；非已训练 TP/段数。 */
    long long model_training_nums;
    /* 字段 gc_cnt：预留旧 GC 计数，当前未维护。 */
    int gc_cnt;
    // uint64_t max_read_lpn;
    // uint64_t min_read_lpn;
    // uint64_t max_write_lpn;
    // uint64_t min_write_lpn;
    /* 字段 read_joule：模拟 NAND_READ 累加源码常量 3.5；单位/校准未说明，非实测能耗。 */
    long double read_joule;
    /* 字段 write_joule：模拟 NAND_WRITE 累加常量 16.7；TP 漏计，不是完整实测能耗。 */
    long double write_joule;
    /* 字段 erase_joule：模拟 NAND_ERASE 累加常量 132；与状态 erase_cnt 可不同。 */
    long double erase_joule;
    /* 字段 joule：预留总估算能耗，初始化 0；当前核心未对三项求和。 */
    long double joule;
};

/* 结构 ssd：单设备完整 FTL 状态，由 bb_init 零分配，ssd_init 建树与启动工作线程。 */
struct ssd {
    /* 字段 ssdname：外围设备名引用，日志使用。 */
    char *ssdname;
    /* 字段 sp：几何/时延/映射参数，初始化后被各路径读取。 */
    struct ssdparams sp;
    /* 字段 ch：完整 NAND 元数据树的 channel 根数组。 */
    struct ssd_channel *ch;
    /* 字段 maptbl：每物理总页槽对应一个 LPN→PPA 真值；全表常驻 DRAM，默认 64 MiB。 */
    struct ppa *maptbl; /* 常驻 DRAM 的完整 LPN→PPA 真值表 */
    /* 字段 rmap：普通 PPN→反向逻辑标识，DATA=LPN/GTD=TP 索引；DRAM 数组，默认 64 MiB。 */
    uint64_t *rmap;     /* 常驻 DRAM 的反向表；原型假设 OOB，未实现真实 OOB I/O */
    /* 字段 bitmaps：每 LPN 一字节的候选资格，默认 8 MiB；覆盖清零缺失，需真值校验。 */
    uint8_t *bitmaps;

    /* 字段 lm：全局 line 实体/队列/计数管理器。 */
    struct line_mgmt lm;

    //LearnedFTL 扩展部分
    /* 字段 gtd：按 TP 索引保存翻译页 PPA，常驻 DRAM，默认 128 KiB。 */
    struct ppa *gtd;    // GTD 保存各 Translation Page 的 PPA，不是 block 地址数组
    /* 字段 gtd_wps：按 TP group 索引的数据 WP 数组，默认 256 项但前 240 初始化。 */
    struct write_pointer *gtd_wps;  // 原注释称每 32 张 TP 一个 WP；实际 trans_per_line 默认 64，按 group 配数据 WP
    /* 字段 gtd_usage：预留每 TP 使用量，初值 512；当前无后续维护。 */
    uint64_t *gtd_usage;

    // 活跃的 CMT 管理器
    /* 字段 cm：活跃 TPFTL 两层 CMT 与双哈希管理器。 */
    struct cmt_mgmt cm;

    /* 字段 trans_wp：独立元数据分配 WP，所有 TP 共用；写后推进。 */
    struct write_pointer trans_wp; // 独立 Translation Page 分配 WP，和数据 group WP 分开
    /* 字段 lr_nodes：每 TP 的常驻八段线性模型，与 gtd 平行数组组织。 */
    struct lr_node *lr_nodes;  // 每 TP 一个常驻分段线性模型
    /* 字段 cmt：遗留 ht 字段，当前实际缓存使用 cm。 */
    struct ht cmt;    // 遗留映射哈希字段；当前实际使用 cm
    /* 字段 num_trans_write：遗留翻译写计数，当前未维护。 */
    uint64_t num_trans_write;
    /* 字段 num_data_write：遗留数据写计数，当前未维护。 */
    uint64_t num_data_write;
    /* 字段 num_data_read：遗留数据读计数，当前未维护。 */
    uint64_t num_data_read;

    /* 与 NVMe I/O 线程通信的无锁请求/完成 ring */
    /* 字段 to_ftl：按 poller 索引接收 NVMe 请求的 ring 数组，FTL 线程出队。 */
    struct rte_ring **to_ftl;
    /* 字段 to_poller：按 poller 索引返回完成请求的 ring 数组，FTL 线程入队。 */
    struct rte_ring **to_poller;
    /* 字段 dataplane_started_ptr：外围共享的数据面启动标志指针，线程启动时轮询。 */
    bool *dataplane_started_ptr;
    
    /* 字段 model_used：全局模型尝试开关，初始化 true；header MODEL 枚举未在 bb_flip 实现。 */
    bool model_used;
    /* 字段 valid_lines：预留 line 有效标志数组，当前只初始化清零。 */
    uint8_t *valid_lines;

    /* 字段 stat：统计事件与预留测量字段。 */
    struct statistics stat;
    /* 字段 ftl_thread：FTL 的 QemuThread 句柄，单工作线程执行读写及同步分配 GC。 */
    QemuThread ftl_thread;

    // 按 line id 反查唯一所属 WP
    /* 字段 line2write_pointer：按物理 line id 反查唯一 owner；分配设置、line 回收清 NULL。 */
    struct write_pointer **line2write_pointer;

};

/* 接口：参数 n 为已分配 n->ssd 的控制器；建立状态树、映射和线程，无返回值。 */
void ssd_init(FemuCtrl *n);
/* 接口：打印 access/CMT hit/模型候选并局部清零；参数 ssd 为设备，不实际数模型段。 */
void count_segments(struct ssd* ssd);



/* 编译期开关：只有定义 FEMU_DEBUG_FTL 才启用本组调试日志/断言。 */
#ifdef FEMU_DEBUG_FTL
/* 宏 ftl_debug：可变参数调试日志：fmt 为格式串，可选 __VA_ARGS__ 为格式参数；FEMU_DEBUG_FTL 下 printf，其他构建为空；do/while(0) 使宏像单语句。## __VA_ARGS__ 兼容无额外参数。 */
#define ftl_debug(fmt, ...) \
    do { printf("[FEMU] FTL-Dbg: " fmt, ## __VA_ARGS__); } while (0)
#else
/* 宏 ftl_debug：可变参数调试日志：fmt 为格式串，可选 __VA_ARGS__ 为格式参数；FEMU_DEBUG_FTL 下 printf，其他构建为空；do/while(0) 使宏像单语句。## __VA_ARGS__ 兼容无额外参数。 */
#define ftl_debug(fmt, ...) \
    do { } while (0)
#endif

/* 宏 ftl_err：可变参数错误日志：fmt 为格式串，可选 __VA_ARGS__ 为格式参数；输出 stderr，宏本身不退出。do/while(0) 保护条件语句；## __VA_ARGS__ 处理可选参数。 */
#define ftl_err(fmt, ...) \
    do { fprintf(stderr, "[FEMU] FTL-Err: " fmt, ## __VA_ARGS__); } while (0)

/* 宏 ftl_log：可变参数普通日志：fmt 为格式串，可选 __VA_ARGS__ 为格式参数；输出 stdout，宏本身不改状态。do/while(0) 使多行宏作为单语句。 */
#define ftl_log(fmt, ...) \
    do { printf("[FEMU] FTL-Log: " fmt, ## __VA_ARGS__); } while (0)


/* FTL 调试断言，只有 FEMU_DEBUG_FTL 时才执行表达式 */
/* 编译期开关：只有定义 FEMU_DEBUG_FTL 才启用本组调试日志/断言。 */
#ifdef FEMU_DEBUG_FTL
/* 宏 ftl_assert：调试断言：FEMU_DEBUG_FTL 时调用 assert；默认为空且不求值表达式，不能承担业务校验。 */
#define ftl_assert(expression) assert(expression)
#else
/* 宏 ftl_assert：调试断言：FEMU_DEBUG_FTL 时调用 assert；默认为空且不求值表达式，不能承担业务校验。 */
#define ftl_assert(expression)
#endif
#endif