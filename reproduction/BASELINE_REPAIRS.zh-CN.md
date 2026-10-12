# 图 14 基线核对记录

DFTL、TPFTL、ideal、LeaFTL 均使用作者提供的 FEMU 实现，调整到论文 NAND 几何、缓存预算及统一统计输出。四个基线均在相同算法的小几何版本上完成六轮实际 CRC 写入/校验、随机读取及 GC 冒烟；这不等于逐项证明其全部算法正确。

LeaFTL 源码发现并修复了以下具体缺陷，差异保留在 `variants/leaftl/mechanism-repairs.patch`。不按期望性能调整参数。

- GC 刷新 TP 时错用 host buffer 中的 LPN，现使用实际迁移 LPN；用布尔标记代替 LPN=0 哨兵，避免漏刷第 0 页。
- 新数据页的 GC 编程误标为 NAND_READ，修正为 NAND_WRITE，恢复写入次数、时延及能耗记账。
- 训练循环漏掉最后样本和单样本，GC 训练还缺少映射组边界。统一半开区间遍历，完整覆盖所有样本且不跨组；单样本采用常数映射，避免 OLS 除零。
- 回归使用段内相对坐标，预测使用组内相对坐标，修正截距的坐标换算。截距可为负数，字段改为同为 8 字节的 double，不改变段结构的缓存预算。
- 非连续段 CRB 查找的局部 flag 遮蔽返回值，缓存段指针仅修改局部形参；修正返回和复制。段链插入补全空尾/重叠情况。
- 压缩时准确段的末端漏计，非连续段的有效记录误标为无效，修正活跃映射集合。
- host buffer 查重扫描包含旧批次的残留槽位，且命中标记未逐 LPN 复位；仅扫描当前批次，覆盖失效后清除旧 bitmap，避免遗漏后续写和重复失效。
- GC 次数改为选中真实 victim 后计数；去掉每次尝试的打印；模型组数量由物理页数推导，保持缩小几何的验证含义。

修改前的源码、二进制和验证记录在各 LeaFTL 构建目录的 `before-mechanism-repairs/` 中。旧冒烟的 CRC 通过不证明上述记账或模型逻辑正确。

额外原生测试直接包含修正后的 LeaFTL 源码，检查最后样本、单样本、映射组边界、CRB、段链插入及压缩后的映射；六轮 128-page 请求覆盖写逐页检查正反映射、全部有效物理页与当前 DATA/GTD 引用一致，以及 TP0 和 GC 编程次数。已通过 ASan/UBSan，记录在 `native-leaftl-final/`。这里的“128-page”是 512 KiB；测试输出中早期的“128KiB”标签写错，数值参数为 128 个 4KiB 页。

最终对照使用这些修正后的作者基线，并在结果说明中公开该差异。DFTL/TPFTL 仍沿用其作者原型的缓存与延迟实现，未重写成 LearnedFTL 的缓存策略；结果适用于当前 FEMU 软件实现，不应直接解读为硬件性能。

图 14(b) 的 LeaFTL 统计采用论文定义的“一次闪存读取”比例，而不是把缓存命中都算作模型命中。新增日志分别输出 single_read/double_read/triple_read；修正缓存段更新时遗漏的 len，以及已经执行翻译页读取却漏计 read_cnt 的分支。读请求的临时预测值逐 LPN 初始化，RAM buffer 查找仅覆盖当前有效槽位，RAM 命中不计入单次闪存读取。原生回归证明 `single + 2*double + 3*triple` 与真实 NAND 读取能耗计数逐次一致；结果在 `native-leaftl-read-counters/`。

随机覆盖验证又区分出两个问题：

- `native-leaftl-random/` 的 32-line 缩小几何仍使用 2048-page 缓冲区，一次刷新可占两条 DATA line，但 95% 阈值截断后只预留一条空闲 line。断点证明耗尽发生在 `do_gc -> GC_flush -> ssd_advance_write_pointer`。正式几何的缓冲区仅占 1/16 line，阈值预留 12 条 line。追加测试仅在原生 fixture 中将预留下限设为 4 条，六轮顺序覆盖、50000 次随机覆盖、排空缓冲区及逐页物理映射检查通过；这个额外 fixture 不算作“仅几何不同”的正式冒烟，也不改变正式参数。
- 空闲表复用可使同一训练批次的物理 VPPN 从较大值回绕到较小值。原实现 `ppns[i] - ppns[0]` 无符号下溢；`native-leaftl-wrap-overflow-before/` 对 `{1023,0}` 的明确输入触发 float-cast-overflow 并得到错误斜率。改以批次最小 PPN 为回归基准，截距用 double 运算；预测先检查有限值及物理范围，越界时查询 TP 后读取数据。修复后同一输入及负数/无穷预测检查通过，注入无效缓存预测确认执行两次 NAND 读取而非访问越界物理地址。

原生测试已显式加入 float-cast-overflow 检查。旧的 LeaFTL 顺序写窗口保留并标为 DIAGNOSTIC-ONLY，全部正式 LeaFTL 窗口使用修复后的同一冻结构建重跑。
