# LearnedFTL 机制补全与图 14 复现

当前实现为 `bbssd/ld-tpftl.c`、`bbssd/ld-tpftl.h` 和统计入口 `bbssd/bb.c`。工作区中的对应源码位于 `learnedFTL-codex-learnedftl-chinese-comments/`；发布仓库直接采用 FEMU 设备子树布局。

实验范围为图 14：DFTL、TPFTL、LeaFTL、LearnedFTL、ideal，随机读、顺序读、随机写、顺序写，每组合三次。完整交付要求 60 个有效正式窗口。预热后写入另有 30 个补充窗口，单独绘图，不替换冷启动写入。当前状态见 [reproduction-state.json](reproduction-state.json)；完整报告仅在严格检查全部组合后生成。

## 机制与证据

- [MECHANISM_AUDIT.zh-CN.md](MECHANISM_AUDIT.zh-CN.md)：逐项对照论文、实现选择、验证范围及旧缺陷的处理。
- [BASELINE_REPAIRS.zh-CN.md](BASELINE_REPAIRS.zh-CN.md)：LeaFTL 基线修复和缩小几何测试的额外预留说明。
- `learnedftl-completion.patch` 与 `original/`：累计补丁及原始 C/H；`patch-verification.json` 记录实际应用后逐字节核对结果。
- `verify_gc_read_dependencies.py`：验证 GC 时序修正前后的完整几何源码快照、sanitizer 参数及全部共同检查点。其最大模拟排队延迟从 180.63 秒降至 9.55 秒，GC 与 NAND 编程计数一致；这不是吞吐量测量。
- `native_parallel_gc_test.c`：可选择完整几何、64 个在途请求和随机压力用例。随机预检查是定量回归，不能替代正式六遍随机预热。

FEMU payload 按逻辑地址保存在内存，因此 CRC 通过不能单独证明物理页搬移；原生映射、反向映射、页身份与队列检查提供补充证据。原型未验证硬件掉电恢复或 retention rewrite。能耗字段为源码模型计数。

## 环境与构建

本仓库是作者 QEMU 7/FEMU 的设备子树，不能单独构建 QEMU。脚本依赖已准备好的完整 SDK：`FEMU_ROOT`（本机默认为 `/home/zheng/FEMU-ext4`）中需要作者 QEMU 7 的 `reproduction/upstream/build-femu`、编译数据库、链接对象、`femu-local-env.sh`、原基线变换脚本 `reproduction/build_paper.py`，以及 `reproduction/images/` 下的客体基础盘、seed ISO 和 SSH 身份文件。发布仓库保留变换脚本及源代码，但不包含 VM 镜像、密钥、二进制或依赖缓存。QEMU 10 的 FEMU 接口与这里不同。

从本仓库根目录执行：

```bash
python3 reproduction/build_femu.py
python3 reproduction/build_baselines.py
python3 reproduction/validate_baselines.py
python3 reproduction/run_vm_smoke.py
```

构建生成不可变二进制名称及 `build-manifest.json`；测量前核对二进制和源码哈希。基线修复后使用 `--rebuild` 保持已审阅源码。原生完整一致性回归可运行：

```bash
SANITIZE=1 python3 reproduction/run_native_gc.py
NATIVE_HARNESS="$PWD/reproduction/native_parallel_gc_test.c" \
NATIVE_OUTPUT="$PWD/reproduction/native-parallel-local-check" \
NATIVE_EXTRA_CFLAGS='-DNATIVE_FULL_GEOMETRY -DNATIVE_CONTINUE_AFTER_30S -DNATIVE_EXPECT_BELOW_TIMEOUT' \
SANITIZE=1 python3 reproduction/run_native_gc.py
python3 reproduction/verify_gc_read_dependencies.py
```

原生回归与正式测量分开运行。单个 VM 使用本地 2233 端口；启动前要求至少 35 GiB 可用内存。当前环境与测量选择见 [EXPERIMENT_ENVIRONMENT.json](EXPERIMENT_ENVIRONMENT.json)。

## 当前测量入口

```bash
python3 reproduction/run_fig14_matrix.py
```

调度器串行运行并恢复已完成且版本相符的组合。写窗口各使用新 VM、无预热；读组合先用对应的 512 KiB 写实际覆盖六个逻辑盘容量，再测三个读窗口。64 个 FIO 作业各有明确的 `offset/filesize/size`，4 KiB、direct、psync，配置每窗口 60 秒。地址范围、随机种子、时长和客体资源是论文未公开的选择，均保存在 `method.json`。

顺序读的共享全盘范围短诊断和三次对应写入补充观测均排在正式读窗口之后，分别保存。有效设备队列超时保持 30 秒；600 秒仅出现在排查用的诊断目录。实际 FIO 运行时间包括在途请求返回，不能用配置的 60 秒强行替代实际耗时。

每个实验新建目录，保存原始 JSON、FIO 配置、QEMU 统计、阶段进度、runner 和源码快照。`DIAGNOSTIC-ONLY.txt` 排除旧版本、失败、干扰和特定诊断。六遍预热必须写满 192,002,654,208 字节；持续推进的长预热需要等待，不能用短预热冒充。`progress.jsonl` 记录扇区、RSS 与 swap。

## 汇总、绘图和发布

```bash
python3 reproduction/summarize_results.py
python3 reproduction/plot_measured_fig14.py
python3 reproduction/summarize_prewarmed_writes.py
python3 reproduction/plot_measured_fig14.py --prewarmed-writes --summary reproduction/paper-prewarmed-summary.json
python3 reproduction/write_fig14_report.py
```

绘图需要可用的 Matplotlib；本机调度器使用已记录的 bundled Python 和 `plot-libs`。默认拒绝缺项、重复不足或混合版本。`--partial` 只能产生明确标为不完整的图，不能当成最终结果。PNG、PDF、SVG 和绘图 CSV 输出在 `figures/`；`FIG14_REPRODUCTION.zh-CN.md` 汇总数据和论文差异。图中 MB/s 为十进制，误差线为三次样本标准差；ideal 映射覆盖率为设计上的 100%。

正式图的 WAF 面板采用线性坐标。单独的预热后写入补充图若出现 WAF 大于 100，则采用明确标注的对数坐标，以便同时看清极高写放大与基线值；CSV 数值保持原值，坐标设置及数据/绘图脚本哈希保存在 provenance 文件中。

发布原始证据时运行：

```bash
python3 reproduction/export_fig14_evidence.py reproduction/evidence/fig14
python3 reproduction/verify_fig14_export.py reproduction/evidence/fig14
```

该导出要求完整的 40 个有效 VM 目录，生成原始绝对路径到发布目录的索引及逐文件 SHA-256。源码、配置和原始日志保留；二进制只保留本地验证哈希。下载发布数据后，可独立运行第二条命令检查归档文件完整性、版本一致性及 60+30 个窗口的覆盖；这不能替代导出前的完整原始测量校验。发布后的 `evidence/current-native/` 同样带源码快照索引。

`README.legacy*` 和带早期日期的操作手册是历史记录；其中旧端口、旧 runner、全局 FIO 地址配置和写预热流程不作为当前测量入口。完整矩阵和图完成，只证明本方法的实测结果完整，不自动证明论文的性能优势已复现。
