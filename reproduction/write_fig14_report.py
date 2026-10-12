#!/usr/bin/env python3
"""Describe only a complete, validated Fig.14 matrix and its limits."""
from collections import defaultdict
import datetime
import json
from pathlib import Path
import statistics

HERE = Path(__file__).resolve().parent
summary = json.loads((HERE / 'paper-summary.json').read_text())
variants = [('dftl', 'DFTL'), ('tpftl', 'TPFTL'), ('leaftl', 'LeaFTL'),
            ('learnedftl', 'LearnedFTL'), ('ideal', 'ideal')]
patterns = [('randread', '随机读'), ('read', '顺序读'),
            ('randwrite', '随机写'), ('write', '顺序写')]
groups = defaultdict(list)
for row in summary['measurements']:
    if not row['diagnostic_only']:
        groups[row['variant'], row['pattern']].append(row)
assert len(groups) == 20
for variant, _ in variants:
    hashes = set()
    for pattern, _ in patterns:
        rows = groups[variant, pattern]
        assert len(rows) == 3 and {r['repeat'] for r in rows} == {1,2,3}
        hashes.update(r['binary_sha256'] for r in rows)
    assert len(hashes) == 1, 'Do not mix revisions across workloads'
assert not any(c.get('validation_error') for c in summary['cases'] if not c['diagnostic_only'])

def avg(v, p, field):
    return statistics.mean(r[field] for r in groups[v,p])

def spread(v, p, field):
    values = [r[field] for r in groups[v,p]]
    return f'{statistics.mean(values):.2f} ± {statistics.stdev(values):.2f}'

now = datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=8)))
lines = ['# LearnedFTL 图 14 本地复现', '',
         f'生成时间：{now:%Y-%m-%d %H:%M}（北京时间）。', '',
         '完成 4 种 FIO 负载 × 5 种 FTL × 3 次测量，共 60 个有效窗口。下列数值来自原始 FIO JSON 和 FEMU 统计，失败、旧源码及并发干扰的诊断窗口已排除。完成测量不等于复现了论文的性能结论。', '',
         '![图 14 形式的实测结果](figures/fig14-measured.png)', '',
         '## 吞吐量', '',
         '单位为十进制 MB/s，表内为均值 ± 样本标准差。', '',
         '| FTL | 随机读 | 顺序读 | 随机写 | 顺序写 |',
         '| --- | ---: | ---: | ---: | ---: |']
for v, name in variants:
    lines.append('| ' + name + ' | ' + ' | '.join(spread(v,p,'throughput_MB_s') for p,_ in patterns) + ' |')
lines += ['', '## 命中率与写放大', '',
          'LeaFTL 采用论文定义的一次闪存读取比例，未把所有模型缓存命中算作准确预测。ideal 的映射覆盖率为设计上的 100%，没有伪造测量计数。', '',
          '| FTL | 随机读 CMT / 模型（%） | 顺序读 CMT / 模型（%） | 随机写 WAF | 顺序写 WAF |',
          '| --- | ---: | ---: | ---: | ---: |']
for v,name in variants:
    hit_cells = []
    for p in ('randread','read'):
        if v == 'ideal': hit_cells.append('100（设计值） / 0')
        elif v == 'leaftl': hit_cells.append(f"0 / {avg(v,p,'single_flash_read_ratio')*100:.2f}")
        else: hit_cells.append(f"{avg(v,p,'cmt_hit_ratio')*100:.2f} / {avg(v,p,'model_hit_ratio')*100:.2f}")
    lines.append('| ' + name + ' | ' + ' | '.join(hit_cells) + ' | ' +
                 ' | '.join(spread(v,p,'write_amplification') for p in ('randwrite','write')) + ' |')
lines += ['', 'WAF = 测量窗口内 NAND 编程次数 / 主机写入页数，包含 DATA、TP 和 GC 编程。三个窗口的 WAF 分别计算后取均值。', '',
          '## 与论文的差异', '',
          '论文报告 LearnedFTL 的随机读与顺序读吞吐量分别达到 ideal 的 89.2% 和 96.8%。本地实测分别为 ' +
          f"{avg('learnedftl','randread','throughput_MB_s')/avg('ideal','randread','throughput_MB_s')*100:.2f}% 和 " +
          f"{avg('learnedftl','read','throughput_MB_s')/avg('ideal','read','throughput_MB_s')*100:.2f}%。", '',
          '论文随机读模型命中为 55.5%、CMT 与模型合计约 90%。本地实测模型命中为 ' +
          f"{avg('learnedftl','randread','model_hit_ratio')*100:.2f}%，合计 " +
          f"{(avg('learnedftl','randread','model_hit_ratio')+avg('learnedftl','randread','cmt_hit_ratio'))*100:.2f}%。", '',
          '实验保留以下差异，未按论文期望的柱高调整代码或数据：', '',
          '- 论文未公开 FIO 作业地址范围、随机种子、具体测量时长及客体资源。本地使用 64 个互不重叠的 476 MiB 区域，顺序读另加每作业 4 KiB 位移；每窗口 60 秒，固定种子 20261011，4 vCPU / 2 GiB 客体。地址布局会改变共享命中和 NAND 并行度。',
          '- 读负载先用对应的 512 KiB 写入连续预热 6 个逻辑盘容量；三次读测量共享该静态设备。写负载每次从全新 VM 开始，无预热。论文只明确规定读预热，未说明写窗口的初始状态或时长；短的新设备写窗口可能尚未进入 GC 稳态。',
          '- 本机为 i7-12700 / 约 45.5 GiB 内存，论文为双 Xeon Gold 5318Y / 128 GB。FEMU 软件开销和主机调度不同。每阶段 RSS/swap 记录保存在 progress.jsonl；未锁定全部后端内存。',
          '- 物理容量按 8×8×256×512×4096 得到 34,359,738,368 B；逻辑容量为 30518 MiB（32,000,442,368 B）。该组合与论文口述的“32 GB + 2 GB”并不完全相同。',
          '- LearnedFTL 的分段误差阈值、借页阈值及配对管理采用公开记录的实现选择；论文未给出这些具体参数。基线基于作者源码，LeaFTL 的已复现缺陷及修复见 BASELINE_REPAIRS.zh-CN.md。DFTL/TPFTL 原型的内部计时与缓存实现未被全部重写或证明。', '',
          '## 预热后写入的补充观测', '',
          '![预热后写入的单独对照](figures/fig14-prewarmed-writes.png)', '',
          '为检查冷启动窗口未充分触发数据 GC 的影响，在每个读组合的正式读测量及布局诊断结束后，再进行对应的 3 个配置为 60 秒的写窗口。地址范围与冷启动写完全相同。三个写窗口共享设备并改变其状态，属于单独的补充观测；六遍预处理并非已达到稳态的证明。图中读柱沿用原正式读结果。', '',
          '补充窗口与正式窗口均保留客体有效队列的 30 秒请求超时。旧版本 GC 搬移使用了会被目标写更新的源 LUN 时间，产生多余串行依赖；修正为逐页保存读完成时间后，完整几何原生并发用例的最大模拟排队延迟从 180.63 秒降到 9.55 秒，所有共同检查点的 GC 与 NAND 编程计数保持一致。旧崩溃窗口及延长至 600 秒的排查窗口均只作诊断。原型的 NVMe 队列重置生命周期未因此被证明正确。配置的 60 秒到达后，FIO 仍会等待在途请求；吞吐量使用原始 JSON 的实际运行时间，逐次 CSV 同时保留实际时长及最大 I/O 延迟。', '',
          '| FTL | 预热后随机写（MB/s） | 预热后顺序写（MB/s） | 随机写 WAF | 顺序写 WAF |',
          '| --- | ---: | ---: | ---: | ---: |']
supplement = json.loads((HERE/'paper-prewarmed-summary.json').read_text())
for v,name in variants:
    cells=[]
    for field in ('throughput_MB_s','write_amplification'):
        for p in ('randwrite','write'):
            rr=[r for r in supplement['measurements'] if r['variant']==v and r['pattern']==p]
            assert len(rr)==3 and all(r['cohort'].startswith('supplemental') for r in rr)
            vv=[r[field] for r in rr]
            cells.append(f'{statistics.mean(vv):.2f} ± {statistics.stdev(vv):.2f}')
    lines.append('| '+name+' | '+' | '.join(cells)+' |')
if max(r['write_amplification'] for r in supplement['measurements']
       if r['pattern'].endswith('write')) > 100:
    lines += ['', '补充图的 WAF 面板采用明确标注的对数坐标，便于同时看清数万量级与接近 1 的值；正式冷启动图采用线性坐标。表格和 CSV 保留原始 WAF 数值。']
learned_prewarmed_seq = [r for r in supplement['measurements']
                         if r['variant']=='learnedftl' and r['pattern']=='write']
if statistics.mean(r['write_amplification'] for r in learned_prewarmed_seq) > 100:
    lines += ['',
              '预热后 LearnedFTL 顺序写出现极高的实测 WAF，不能据此声称复现了论文的写入优势。论文 III-D 的借页对象要求空页充裕且模型未训练；本实现把顺序初始化也视为已训练，因此六遍覆盖后的组不再满足该条件。64 个互不重叠的作业同时推进多个逻辑组，而 GC 搬移整个组；这一组合可能造成少量主机覆盖写伴随大量有效页搬移。该解释基于机制和计数，尚未通过保持其他条件不变的地址布局对照证明因果。借页与训练状态的定义见 MECHANISM_AUDIT.zh-CN.md。']
if all(r['gc_counter']==0 for p in ('write','randwrite') for r in groups['learnedftl',p]):
    lines += ['', 'LearnedFTL 的所有冷启动写窗口数据组 GC 计数为 0，因此冷启动图的低 WAF 不能单独证明论文的稳态 GC 优势。该计数不含独立的 TP GC，不能直接与基线的总 GC 计数比较。']
lines += ['', '| FTL | 写模式 | 实际窗口时长范围（秒） | 最大单次 I/O（秒） |',
          '| --- | --- | ---: | ---: |']
for v,name in variants:
    for p,label in patterns[2:]:
        rr=[r for r in supplement['measurements'] if r['variant']==v and r['pattern']==p]
        runtimes=[r['fio_runtime_seconds'] for r in rr]
        longest=max(r['max_io_latency_ms'] for r in rr)/1000
        lines.append(f'| {name} | {label} | {min(runtimes):.2f}–{max(runtimes):.2f} | {longest:.2f} |')
lines += ['',
          '## 读前预热记录', '',
          '每个读组合均实际写入 192,002,654,208 B，未用短预热替代六盘覆盖。下表时长来自 FIO，不纳入正式吞吐量。swap 是阶段采样到的 QEMU VmSwap 最大值，不保证捕获瞬时峰值。', '',
          '| FTL | 读负载 | 预热时长（分钟） | QEMU swap 采样最大值（MiB） |',
          '| --- | --- | ---: | ---: |']
for v,name in variants:
    for p,label in patterns[:2]:
        folder = Path(groups[v,p][0]['directory'])
        warm = json.loads((folder/'prewarm.json').read_text())
        seconds = max(j['write']['runtime'] for j in warm['jobs']) / 1000
        progress = [json.loads(s) for s in (folder/'progress.jsonl').read_text().splitlines()]
        swap = max((r['qemu_swap_kib'] for r in progress if r['stage']=='prewarm'), default=0) / 1024
        lines.append(f'| {name} | {label} | {seconds/60:.2f} | {swap:.2f} |')
lines += ['',
          '## 地址布局诊断（不进入图 14）', '',
          '在每种 FTL 的正式顺序读窗口结束后，额外进行 10 秒共享全盘地址范围的读取，单独保存原始 JSON。该实验用于观察布局敏感性，时长和初始缓存状态不同，不能把差异全部归因于布局。', '',
          '| FTL | 共享范围吞吐量（MB/s） | CMT 命中（%） | 模型 / 单次闪存读取（%） |',
          '| --- | ---: | ---: | ---: |']
for v,name in variants:
    directories = {r['directory'] for r in groups[v,'read']}
    assert len(directories) == 1
    folder = Path(next(iter(directories)))
    method = json.loads((folder/'method.json').read_text())
    probe = method['shared_read_probe']
    assert probe['diagnostic_only']
    raw = json.loads((folder/'shared-read-probe.json').read_text())
    assert all(j['error']==0 for j in raw['jobs'])
    rate = sum(j['read']['io_bytes']*1000/j['read']['runtime'] for j in raw['jobs'])/1e6
    metrics = probe['metrics']; access=int(metrics['access'])
    cmt = '100（设计值）' if v=='ideal' else f"{int(metrics['cmt_hit'])/access*100:.2f}"
    model = '0' if v=='ideal' else f"{int(metrics['single_read'] if v=='leaftl' else metrics['model_hit'])/access*100:.2f}"
    lines.append(f'| {name} | {rate:.2f} | {cmt} | {model} |')
lines += ['', '## 源码、验证与数据', '',
          '各 FTL 的全部负载使用同一冻结二进制：', '',
          '| FTL | SHA-256 |', '| --- | --- |']
for v,name in variants:
    lines.append(f"| {name} | `{groups[v,'read'][0]['binary_sha256']}` |")
lines += ['', '- `MECHANISM_AUDIT.zh-CN.md`：论文机制逐项核对、实现选择和验证范围。',
          '- `native-gc-read-dependency-regression/` 与 `native-gc-read-dependency-multigroup/`：当前 LearnedFTL 的 ASan/UBSan/浮点转换越界检查，包括跨组借页、完整成对 GC、近容量覆盖和 64 路部分写入。早期 `native-current-float-check/` 与 `native-multigroup-float-check/` 保留为历史修订的证据。',
          '- `native-parallel-gc-fixed/`：当前源码完整几何、六遍顺序预热及 64 个在途请求；`gc-read-dependency-verification.json` 对比时序修正前后 64 个共同检查点。`native-parallel-gc-random-preflight/` 另做一遍随机全盘覆盖、1024 次 512 KiB 覆盖与 4096 个并发 4 KiB 请求，不能替代正式六遍随机预热。',
          '- `native-leaftl-wrap-final/` 与基线 validation.json：LeaFTL 回绕/回退/计数及六轮实际 CRC 冒烟。原生缩小几何的额外随机覆盖测试使用单独披露的预留空间下限。发布仓库的上述原生证据位于 `evidence/current-native/`；基线验证位于 `baseline-evidence/`。',
          '- `paper-summary.csv`：60 个窗口的逐次指标；`figures/fig14-measured.csv`：绘图用聚合数据；PNG/PDF/SVG 可导出。',
          '- 每个实验目录的 `method.json`、`run*.json`、`measure.fio`、`qemu.log` 和源码快照可追溯窗口、版本与计数。发布目录为 `evidence/fig14/`；其中 `index.json` 把原始绝对路径映射到发布目录，并记录每个文件的 SHA-256。', '',
          'FEMU 的逻辑 payload CRC 不能单独证明物理闪存搬移，原生逐页身份检查提供补充证据。本次未验证硬件掉电恢复或 retention rewrite；论文 IV-A 也说明 FEMU 未实现 retention rewrite。能耗字段为源码模型计数，不是主机实测焦耳。', '']
(HERE/'FIG14_REPRODUCTION.zh-CN.md').write_text('\n'.join(lines))
print(HERE/'FIG14_REPRODUCTION.zh-CN.md')
