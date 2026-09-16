# halMemcpyBatch D2H benchmark

这个独立 demo 用于在相同 D2H 负载下对比多种 Host DRAM 分配方式，不需要先构建 MemFabric：

```text
aclrtMalloc(HBM) -> halMemcpyBatch -> 已通过 halHostRegister 注册的 Host DRAM
```

这是本机 HBM 到本机已注册 DRAM 的基线测试，用来隔离 mmap/页型/注册后的 D2H 性能；它不经过 URMA 网络，
因此不能替代 device URMA 端到端测试。驱动接口还明确不支持来自 `ipc_open`/共享内存导入的 VA，demo 只使用
本进程本地分配并注册的地址。

它和 `HybmConnBasedSegment` 的 device URMA 主机内存准备路径保持一致：先 `mmap` 或 `halMemAlloc`，再以
`HOST_MEM_MAP_DEV` 调用 `halHostRegister`。`halMemcpyBatch` 是 Host 侧发起的 D2H 接口，因此 `dst[]` 使用
原始 Host VA；注册返回的 DVA 仅用于设备/URMA 侧访问，并在日志中输出以便核对。对于部分 `halMemAlloc`
内存，HVA 与 DVA 可能相同，但不能据此把 mmap 注册返回的 DVA 当作 batch 的 Host 目标地址。

## 一键运行

先加载 CANN 环境，然后执行：

```bash
source /usr/local/Ascend/ascend-toolkit/set_env.sh
cd examples/hbm_share_memory/HalMemcpyBatchD2H
bash run.sh
```

默认参数是 device 0、总传输量 256 MiB、每批 16 段、预热 5 次、计时 20 次，并依次尝试以下后端：

- `mmap-4k`：普通匿名 mmap，并通过 `MADV_NOHUGEPAGE` 禁止 THP 合并
- `mmap-2m`：`MAP_HUGETLB | MAP_HUGE_2MB`
- `mmap-1g`：`MAP_HUGETLB | MAP_HUGE_1GB`
- `hal-normal`：`MEM_HOST | MEM_TYPE_DDR | MEM_PAGE_NORMAL`
- `hal-huge`：`MEM_HOST | MEM_TYPE_DDR | MEM_PAGE_HUGE`

系统不支持或没有预留对应 HugeTLB 页时会显示 `SKIPPED`，其他后端继续运行：

```bash
bash run.sh --device 0 --size-mb 256 --batch-count 16 --warmup 5 --iterations 20 --memory all
```

复现 MemCache benchmark 的 `data_dim=2、batch_size=32`：

```bash
bash run.sh --data-dim 2 --batch-size 32 --warmup 5 --iterations 20 --memory all
```

`data-dim=2` 固定使用与 MemCache `example/benchmark/mutil_process.py` 相同的 KV 布局：61 层，每层 K 为
128 KiB、V 为 16 KiB。此时 `--batch-size`（或 `--batch-count`）表示 key batch size，`--size-mb`
不参与数据量计算。batch size 32 时每次迭代共复制 274.5 MiB，包含 3904 个 descriptor。

为了复现 MemCache 默认 `aggregate.num=122` 的提交方式，demo 每个 key 发起一次
`halMemcpyBatch(count=122)`，因此 batch size 32 的一次计时迭代包含32次 HAL 调用。源端 K、V 分别按
`[61][batch_size][block_size]` 分配并进行2 MiB首地址对齐，目标端按每个 key 连续的 K/V 交替布局写入。

也可以单独测试一个后端：

```bash
bash run.sh --memory mmap-2m
bash run.sh --memory mmap-1g --size-mb 1024
bash run.sh --memory hal-normal
bash run.sh --memory hal-huge
```

只比较三种 mmap 页型可使用 `--memory mmap-all`。

## 2M 与 1G mmap

当前 MemFabric 中单独使用 `MAP_HUGETLB`，它选择 `/proc/meminfo` 中 `Hugepagesize` 表示的默认页型，不能保证
一定是 2M，也不能在同一次运行中显式切换到 1G。demo 将页大小编码进 mmap flags，明确请求 2M 或 1G。

查看机器支持和已预留的页型：

```bash
ls -d /sys/kernel/mm/hugepages/hugepages-*
grep -H . /sys/kernel/mm/hugepages/hugepages-*/{nr_hugepages,free_hugepages}
grep -i huge /proc/meminfo
```

1G 页需要硬件/内核支持并预留 1G HugeTLB 池。`--size-mb` 小于 1024 时，`mmap-1g` 会把实际映射和注册大小
向上取整到 1 GiB，但仍只拷贝用户指定的数据量；输出中的 `allocatedMiB` 会显示这项差异。

注意：demo 使用 `mmap(nullptr, ...)`，由内核选择满足页型对齐的地址。MemFabric 生产路径使用 `MAP_FIXED` 和
预留 GVA，目前切片大小、起始地址和偏移只保证 2M 对齐。因此生产代码不能只增加 `MAP_HUGE_1GB`：还必须让
预留基址、每个 `sliceAddr`、分配大小以及文件映射 offset 全部满足 1G 对齐，否则 mmap 会以 `EINVAL` 失败。

`hal-huge` 只能请求驱动定义的 huge page 类型，无法通过当前 `halMemAlloc` flag 明确指定 2M 或 1G，实际页型
由驱动和平台决定。

## 输出解读

结果示例：

```text
[RESULT] memory=mmap-2m dataDim=2 copyMiB=274.500 batchSize=32 halCalls=32 descriptors=3904 ...
```

- `bandwidthGiB/s` 按一次计时迭代的总字节数除以该迭代内全部 `halMemcpyBatch` 调用总耗时计算。
- `data-dim=1` 每次迭代只调用一次 `halMemcpyBatch`；`data-dim=2` 每个 key 调用一次。
- `halMemcpyBatch` 是同步接口，数组项数上限为 4096；`data-dim=2` 每次调用固定为122项。
- mmap、首次触页、`halHostRegister`、预热和结果校验均不计入带宽。
- 如果 `mmap-4k` 明显慢于 `mmap-2m`/`mmap-1g`，优先检查生产路径是否因大页不足回退到了 4K。
- 如果 mmap 三种页型都慢但 hal 后端快，重点检查 mmap 页池、NUMA 放置和 `halHostRegister` 路径。
- 如果所有后端都慢，再固定总大小改变 `--batch-count`，判断是否是小块数量带来的批处理开销。
- demo 会在计时后逐字节校验 D2H 结果；校验失败时不会输出成功结果。

`halMemcpyBatch` 是从 `libascend_hal.so` 动态解析的驱动接口。若当前驱动不导出该符号，demo 会在启动时直接
报告 `dlsym` 错误，这通常意味着驱动/CANN 版本不支持该接口或运行环境的驱动库路径未加载。

另外，驱动接口注释指出，较低版本 Linux 对 OS 分配内存的 pin/register 支持受限；如果
`halHostRegister` 直接失败，请同时记录 `uname -r`、驱动版本和返回码，不要把注册失败误判成 D2H 带宽问题。
