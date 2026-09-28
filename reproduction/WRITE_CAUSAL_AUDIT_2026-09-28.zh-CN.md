# LearnedFTL 顺序写与随机写：因果审计

日期：2026-09-28。范围：已公开诊断提交 `78a9fef51f4800ad87e28fc6991346f99110e0f0` 及其四组证据；本次仅审计和修正文档，没有改变算法或重新运行实验。

**目前可确认存在实现缺口和极低效 GC，不能据此认定论文机制在 FEMU 上必然产生这些低速结果，也没有据此判断论文造假的证据。** 借用限制和未完成的联合 GC 包含我们新加入的代码，应由复现实现承担解释责任。

## 1. 顺序写：低效回收有直接计数证据

本轮条件是新盘、4 KiB、psync、64 任务、60 秒，预热次数为 0。第 60 秒带宽约 0.335 MiB/s；整段平均值受前面的新盘高速阶段影响，不能代表持续 GC 性能。见[实验条件](https://github.com/bigA151/learnedFTL/blob/78a9fef51f4800ad87e28fc6991346f99110e0f0/reproduction/evidence/seq64/method.json)、[原始日志](https://github.com/bigA151/learnedFTL/blob/78a9fef51f4800ad87e28fc6991346f99110e0f0/reproduction/evidence/seq64/qemu.log#L767)。

日志记录 `gc_writes=8158983`、`freed=249`，物理身份搬迁计数同为 8158983。每条 line 有 `8×8×512=32768` 页，每页 4 KiB（[几何参数](https://github.com/bigA151/learnedFTL/blob/78a9fef51f4800ad87e28fc6991346f99110e0f0/bbssd/ld-tpftl.c#L976)）。由这批回收的汇总计数得到：

```text
平均每次释放 line 需重写有效页 = 8158983 / 249 = 32767 页
平均释放的净页容量 = 32768 - 32767 = 1 页
```

即每次释放一条 128 MiB 的 line，平均要搬回约 127.996 MiB 有效数据，只留下约 4 KiB 净空位。这说明当前回收几乎在搬运整条 line，代价与收益严重失衡；它是持续写入停滞的直接证据。这里是汇总平均，**不是已逐条测得每次 GC 都恰好搬 32767 页**，也不是整段主机写入 WAF。仍需事件级计数确认为何总选择这种回收状态，以及空间策略各自贡献多少。

## 2. 零借用不能推导“论文条件下没有冷组”

我们新增的 [donor 选择器](https://github.com/bigA151/learnedFTL/blob/78a9fef51f4800ad87e28fc6991346f99110e0f0/bbssd/ld-tpftl.c#L2965) 在 `free_line_cnt<=32` 或 `>128` 时直接拒绝借用，甚至不扫描候选。这一门槛不是论文给出的数值。两项 64 任务测试结束时均只有 16 条 free line，因此此时借用功能被代码关闭；现有日志没有逐次拒绝原因，不能证明所有失败尝试都因为没有冷组。

随机写结束日志还报告 `untrained_lines=182`、`untrained_pages=3632777`（约 13.9 GiB）。但审计函数以预测成功计数判断“未训练”，实际选择器用另一个训练历史标记，定义不同。因此既不能说“没有未训练剩余页”，也不能把这 182 组直接认定为都满足完整 donor 条件。见[随机写计数](https://github.com/bigA151/learnedFTL/blob/78a9fef51f4800ad87e28fc6991346f99110e0f0/reproduction/evidence/rand64/qemu.log#L996)、[审计定义](https://github.com/bigA151/learnedFTL/blob/78a9fef51f4800ad87e28fc6991346f99110e0f0/bbssd/ld-tpftl.c#L60)、[训练历史标记](https://github.com/bigA151/learnedFTL/blob/78a9fef51f4800ad87e28fc6991346f99110e0f0/bbssd/ld-tpftl.c#L2552)。

## 3. 联合 GC 尚未实现完整逻辑组回收

目前 [pair 路径](https://github.com/bigA151/learnedFTL/blob/78a9fef51f4800ad87e28fc6991346f99110e0f0/bbssd/ld-tpftl.c#L3004) 依次执行 hot 和 donor 两次 batch GC。每次按物理 owner 的 line 链表收集，立即写回训练；尚未根据所有参与者扩展到完整关联组和共享 line，再合并收集。

这意味着“共享 line 中某页被正确归入完整 GTD”不等于“该逻辑组所有页一起训练”。同一 GTD 的不同子集可能先后覆盖模型，而旧子集的 bitmap 没有整体重新验证。当前预测函数还会用完整内存映射表校验预测并回退，因此有限读回通过不足以证明模型路径完整正确。见[模型覆盖](https://github.com/bigA151/learnedFTL/blob/78a9fef51f4800ad87e28fc6991346f99110e0f0/bbssd/ld-tpftl.c#L2559)、[预测兜底](https://github.com/bigA151/learnedFTL/blob/78a9fef51f4800ad87e28fc6991346f99110e0f0/bbssd/ld-tpftl.c#L2813)。尚未证明这些模型问题造成了本轮顺序写的数量级下降。

空间预算也未完成：入口仅检查至少两条 free line，未事先核算所有组的有效页和写回目标需求。不能直接删除借用限制后就把测试结果作为正式复现；需先保证低空闲时迁移可以完成。

## 4. 随机写、新盘短测与旧预热结果必须分开

本轮随机写也是新盘 60 秒，`warmup_passes=0`；正常退出和整段平均吞吐不说明六轮预热后的低速问题已经解决，也不能将旧预热低值描述成此次完整借用实现的稳态结果。两种初始状态需要各自保存逐秒吞吐、阶段主机写与 GC 写、借用拒绝原因、空闲页和身份检查，再作同条件对照。[本轮随机写条件](https://github.com/bigA151/learnedFTL/blob/78a9fef51f4800ad87e28fc6991346f99110e0f0/reproduction/evidence/rand64/method.json)。

旧预热批次的本地原始记录 `reproduction/results/paper_fig14/learnedftl/randwrite/run1-console.json` 只有 1290 个 4 KiB 主机写，约 20.46 IOPS，却记录 16,276,929 次 NAND 页写；这说明旧随机写也有严重内部写入开销（该计数包含翻译页及 GC，不能直接当纯数据 GC 次数）。新版新盘随机写 60 秒的平均约 354.48 MiB/s，GC 搬迁统计也与本轮顺序写不同。因此前述“平均净回收 1 页”只针对本轮顺序写，不能套用到随机写。

源码的顺序模型初始化局部副本和 GTD 边界问题确实需要修复，但目前没有对照实验证明它是 4 KiB 顺序写或随机写低速的直接原因。不能把发现的所有缺陷都当成本次性能问题的根因。

## 5. 对论文能下什么结论

论文自身是在 FEMU 原型上评估，不能用“因为在 FEMU 上跑”解释全部差距。论文 III-D/III-E 描述跨组借用、达到阈值后的热冷组回收和分组训练，但没有公开当前实现所需的这些阈值数值；IV-B 将约六轮预热明确写在读测试中，写测试初始状态未明确。[论文原文](https://ranger.uta.edu/~jiang/publication/Conferences/2024/HPCA24%28LearnedFTL%29.pdf)。

当前优先任务是补齐这些机制并验证低空闲正确性，再用公开的统一条件比较发布版与修复版。只有排除实现偏差和实验初始状态差异后，才能评估论文机制是否仍存在严重写入代价；修复后是否接近论文结果，目前尚无证据保证。
