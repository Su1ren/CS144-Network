Lab 1 Writeup
=============

My name: [your name here]

My SUNet ID: [your sunetid here]

This lab took me about [n] hours to do. I [did/did not] attend the lab session.

Program Structure and Design of the StreamReassembler:
见第 2 节：窗口 `[bytes_read(), bytes_read()+capacity)` 上的环形缓冲区 + bitset，
顺序流零拷贝直写，乱序数据按位图去重后由 `flush()` 批量写出。

# Lab 1：Stream Reassembler 实现与数据结构对比

## 1. 实现目标

`StreamReassembler` 接收任意顺序、可能重叠、可能重复的 TCP 段，去重后按序写入
`ByteStream`，并遵守 `capacity` 的流控窗口限制：

- 已组装但未被应用读走的字节 + 尚未组装的字节总数不能超过 `capacity`；
- 重复字节只计一次；
- 收到携带 EOF 的段且所有字节都已组装后，关闭输出流。

## 2. 数据结构的两次迭代

### 2.1 第一版：`std::map<start, std::string>`

以段的起始绝对下标为 key，段内容为 value，配合 `lower_bound` 查找相邻段做合并。

优点：

- 有序，便于找到与当前段重叠/相邻的片段；
- 内存只随实际缓存的段增长。

缺点：

- 每个段至少一次红黑树节点分配 + 一次 `std::string` 堆分配；堆对象分散，
  L1d 局部性较差（实测见 4.5）；
- 每次合并要做 `it->second + data.substr(...)` 的字符串拼接：**从后往前逐段推送时
  最坏 O(n²) 字节拷贝**；
- `merge`/`flush` 需要手工处理前驱重叠、后继重叠、包含、相邻等分支，容易出错
  （开发过程中就出现过窗口边界、计数重复、偏移错误三个 bug）。

### 2.2 第二版：环形缓冲区 + 接收位图（最终实现）

关键观察：未组装字节的绝对下标一定落在窗口
`[bytes_read(), bytes_read() + capacity)` 内，长度有上界，因此不需要有序容器。

```cpp
ByteStream _output;
std::vector<char> _buffer;  // 大小 = capacity，环形
Bits _received;             // capacity 个 bit，1 表示该槽位已收到（64 位字打包）
size_t _unassembled_bytes;
bool _eof;
size_t _eof_index;
```

- **下标映射**：`slot(i) = i % capacity`。窗口长度恰好是 `capacity`，所以窗口内
  的绝对下标取模不会冲突。
- **`push_substring`**：
  - 顺序流快速路径：若 `start == first_unassembled` 且没有缓存数据，直接调用
    `_output.write(ptr, len)` 零拷贝写入输出流；
  - 乱序路径：先按 64 位字扫描位图，检查目标区间是否已有数据（无重叠则整段
    `memcpy` 两次以内完成），否则逐字节写入并跳过已收到的位置；
  - 重复/重叠字节通过位图天然去重，不需要字符串合并。
- **`flush`**：从 `first_unassembled` 开始按 64 位字找连续置位的 run（`~word` +
  `__builtin_ctzll`），直接以 `_output.write(ptr, len)` 写出，再用字级 mask 清位。
  全程无临时 string。
- **EOF**：记录 `_eof_index`，当 `first_unassembled() >= _eof_index` 时关闭输出。

### 2.3 位图表示：byte-per-slot vs bitset

位图经历了两次实现：

1. `std::vector<uint8_t>`：每个位置 1 字节，可用 `memchr`/`memset`（glibc 中是
   SIMD 实现）；
2. 自定义 `Bits`：`std::vector<uint64_t>` 按位打包，每个位置 1 bit，扫描一个
   64 位字即可判断 64 个位置。

没有使用 `std::vector<bool>`：它的 proxy reference 让逐位访问很慢，而且无法按字扫描。

两种位图的内存差距是 8 倍（capacity 为 C 时：C 字节 vs C/8 字节）。性能上，
bitset 的扫描是标量字循环，而 byte 位图的 `memchr` 是 SIMD；在本实验的段长下两者
互有胜负，Release 下差距在 ±15% 以内（见 4.3 的最后一列）。

### 2.4 零拷贝写出

为配合零拷贝写出，给 `ByteStream` 增加了一个重载：

```cpp
size_t write(const char *data, size_t len);  // write(std::string) 转发到它
```

复杂度对比：

| | `map<start, string>` | 环形 buffer + 位图 |
|---|---|---|
| push 一个 L 字节段 | O(L log n) + 字符串拼接 | O(L)（无重叠时是 memcpy） |
| flush | O(k log n) + 每段一次分配 | O(k) 扫描 + 一次 memcpy |
| 去重 | merge 分支处理 | 位图已置位即重复 |
| 额外分配 | 每段节点 + string | 0（构造时一次性分配） |
| 最坏情况 | 反序推送 O(n²) 拷贝 | O(n) |
| 内存 | 每段约 80 B 额外开销 + 数据 | `capacity` 字节数据 + `capacity/8` 位图 |

## 3. 实现过程中遇到的问题

1. **窗口左边界错误**：最初用 `bytes_written()`（即 `first_unassembled`）作为窗口
   起点，忽略了输出缓冲区中未读字节也占用容量，导致 `t_strm_reassem_many` /
   `t_strm_reassem_win` 在流尾部随机丢数据。正确窗口是
   `[bytes_read(), bytes_read() + capacity)`。
2. **`flush` 部分写入时重复计数**：未写出的剩余字节本来就在 `_unassembled_bytes`
   中，不能再加一次；否则窗口被错误压缩，后续数据被丢弃。
3. **EOF 判定用 `==`**：过期的/重复的 EOF 可能让 `_eof_index < first_unassembled`，
   应使用 `>=`。
4. **`store_range` 偏移重复**：传入的指针已经偏移到 `start`，又叠加了一次
   `src_offset`，导致内容错位（`abcefggh`）。改为指针只偏移一次。
5. **`-Weffc++` 要求所有成员出现在构造函数初始化列表**：VS Code 的 clang 构建
   不报错，in-source 的 g++ `-Werror` 会直接失败。

## 4. 性能基准：map vs 环形缓冲区

### 4.1 方法

`tests/stream_reassembler_bench.cc`，注册为 CTest 用例 `t_strm_reassem_bench`
（`make check_lab1` 会自动执行）。同一个二进制内保留三个实现——map 基线、环形
缓冲区 + byte 位图、环形缓冲区 + bitset——用模板跑完全相同的负载：

- `sequential`：按序推送；
- `shuffled`：段顺序随机打乱；
- `overlap`：每段向前多覆盖半个 chunk（模拟重叠重传）；
- `retransmit`：同样的段推送两遍（模拟重传）；
- `reverse`：段从后往前推送（map 字符串拼接的最坏情况）；
- `streaming`：64 KiB 窗口，边推送 8 MiB 数据边读走输出，模拟真实 TCP 稳态；
  构造开销被长流摊薄，适合 perf 这类统计整个进程的工具。

每个配置重复 `reps` 次取最小值，结果换算为 **ns/字节**；计时结束后会校验输出内容
与原始数据完全一致、`unassembled_bytes() == 0`、`bytes_written() == total`。

另外提供单配置模式，便于用外部工具只测一个场景：

```
stream_reassembler_bench <total> <reps> <workload> <chunk> [map|byte|bits]
```

该模式还会输出本次运行中 `push/flush` 期间的堆分配次数与字节数（通过重载全局
`operator new/delete` 统计，重置点位于 reassembler 构造之后，因此构造期的缓冲区
分配不计入）。

- Debug（`-ggdb3 -Og`，`make check_lab1` 默认）：total = 256 KiB（reverse
  为 64 KiB），reps = 3；
- Release（`-O3`，`build-release`）：total = 4 MiB（reverse 为 64 KiB），reps = 3。

测试环境：Intel Core i7-14700K，g++ 13.3.0，Linux。

### 4.2 Debug（`-ggdb3 -Og`，256 KiB）

| workload | chunk | map ns/B | ring(byte) ns/B | ring(bitset) ns/B | bitset vs byte |
|---|---:|---:|---:|---:|---:|
| sequential | 64 B | 1.694 | 0.261 | 0.309 | 0.84x |
| sequential | 256 B | 0.443 | 0.078 | 0.086 | 0.91x |
| sequential | 1024 B | 0.136 | 0.045 | 0.025 | 1.77x |
| sequential | 4096 B | 0.045 | 0.018 | 0.018 | 1.00x |
| sequential | 16384 B | 0.056 | 0.015 | 0.016 | 0.94x |
| sequential | 65536 B | 0.117 | 0.015 | 0.016 | 0.97x |
| shuffled | 64 B | 2.932 | 0.235 | 0.301 | 0.78x |
| shuffled | 256 B | 0.666 | 0.093 | 0.105 | 0.89x |
| shuffled | 1024 B | 0.212 | 0.069 | 0.070 | 0.98x |
| shuffled | 4096 B | 0.077 | 0.062 | 0.063 | 0.98x |
| shuffled | 16384 B | 0.044 | 0.062 | 0.062 | 1.00x |
| shuffled | 65536 B | 0.044 | 0.063 | 0.063 | 0.99x |
| overlap | 64 B | 1.450 | 0.161 | 0.211 | 0.76x |
| overlap | 256 B | 0.371 | 0.050 | 0.059 | 0.85x |
| overlap | 1024 B | 0.123 | 0.020 | 0.023 | 0.88x |
| overlap | 4096 B | 0.058 | 0.018 | 0.018 | 1.01x |
| overlap | 16384 B | 0.050 | 0.016 | 0.016 | 0.99x |
| overlap | 65536 B | 0.060 | 0.016 | 0.016 | 1.00x |
| retransmit | 64 B | 3.351 | 0.282 | 0.359 | 0.79x |
| retransmit | 256 B | 0.781 | 0.103 | 0.119 | 0.86x |
| retransmit | 1024 B | 0.243 | 0.071 | 0.073 | 0.98x |
| retransmit | 4096 B | 0.092 | 0.062 | 0.064 | 0.97x |
| retransmit | 16384 B | 0.054 | 0.062 | 0.063 | 1.00x |
| retransmit | 65536 B | 0.059 | 0.065 | 0.065 | 1.00x |
| reverse | 64 B | 1.550 | 0.265 | 0.324 | 0.82x |
| reverse | 256 B | 0.431 | 0.094 | 0.113 | 0.84x |
| reverse | 1024 B | 0.155 | 0.061 | 0.062 | 0.98x |
| reverse | 4096 B | 0.071 | 0.062 | 0.063 | 0.99x |
| reverse | 16384 B | 0.048 | 0.060 | 0.059 | 1.00x |
| reverse | 65536 B | 0.047 | 0.017 | 0.017 | 1.00x |

> Debug 下数据量小、段数少时测量抖动较大（例如 1024 B sequential 的 1.77x），
> 以 Release 表为准。

### 4.3 Release（`-O3`，4 MiB）

运行方式：`./build-release/tests/stream_reassembler_bench 4194304 3`

| workload | chunk | map ns/B | ring(byte) ns/B | ring(bitset) ns/B | bitset vs byte |
|---|---:|---:|---:|---:|---:|
| sequential | 64 B | 0.612 | 0.151 | 0.172 | 0.88x |
| sequential | 256 B | 0.163 | 0.060 | 0.064 | 0.94x |
| sequential | 1024 B | 0.064 | 0.045 | 0.044 | 1.02x |
| sequential | 4096 B | 0.057 | 0.041 | 0.040 | 1.02x |
| sequential | 16384 B | 0.057 | 0.036 | 0.037 | 0.97x |
| sequential | 65536 B | 0.063 | 0.036 | 0.036 | 1.00x |
| shuffled | 64 B | 3.633 | 0.394 | 0.439 | 0.90x |
| shuffled | 256 B | 0.799 | 0.194 | 0.201 | 0.97x |
| shuffled | 1024 B | 0.241 | 0.154 | 0.155 | 0.99x |
| shuffled | 4096 B | 0.126 | 0.136 | 0.133 | 1.02x |
| shuffled | 16384 B | 0.086 | 0.128 | 0.128 | 1.00x |
| shuffled | 65536 B | 0.080 | 0.129 | 0.128 | 1.00x |
| overlap | 64 B | 0.770 | 0.153 | 0.177 | 0.86x |
| overlap | 256 B | 0.224 | 0.070 | 0.073 | 0.95x |
| overlap | 1024 B | 0.108 | 0.051 | 0.051 | 1.00x |
| overlap | 4096 B | 0.081 | 0.041 | 0.041 | 1.00x |
| overlap | 16384 B | 0.081 | 0.036 | 0.037 | 0.99x |
| overlap | 65536 B | 0.087 | 0.036 | 0.036 | 1.01x |
| retransmit | 64 B | 3.827 | 0.427 | 0.479 | 0.89x |
| retransmit | 256 B | 0.865 | 0.198 | 0.210 | 0.94x |
| retransmit | 1024 B | 0.278 | 0.163 | 0.160 | 1.02x |
| retransmit | 4096 B | 0.149 | 0.135 | 0.138 | 0.98x |
| retransmit | 16384 B | 0.104 | 0.128 | 0.128 | 1.00x |
| retransmit | 65536 B | 0.102 | 0.131 | 0.128 | 1.02x |
| reverse | 64 B | 1.129 | 0.208 | 0.227 | 0.92x |
| reverse | 256 B | 0.303 | 0.080 | 0.086 | 0.93x |
| reverse | 1024 B | 0.129 | 0.055 | 0.058 | 0.95x |
| reverse | 4096 B | 0.062 | 0.055 | 0.055 | 1.00x |
| reverse | 16384 B | 0.047 | 0.059 | 0.059 | 0.99x |
| reverse | 65536 B | 0.050 | 0.018 | 0.018 | 1.01x |

### 4.4 内存分配实测

在单配置模式下统计 `push/flush` 期间的堆分配（重载 `operator new` 计数），
total = 256 KiB：

| chunk | 段数 | map 分配次数/run | map 分配字节/run | ring 分配次数/run |
|---|---:|---:|---:|---:|
| 64 B | 4096 | 12288 | 827,392 | 0 |
| 256 B | 1024 | 3072 | 600,064 | 0 |
| 1024 B | 256 | 768 | 543,232 | 0 |
| 4096 B | 64 | 192 | 529,024 | 0 |

（`retransmit` 场景 map 的分配更多：64 B 时 16,384 次 / 1,093,632 B；
256 B 时 4,096 次 / 863,232 B。）

`streaming` 配置（8 MiB 数据、1 KiB 段、64 KiB 窗口）下：map 每 run **24,576 次 /
17,383,424 B**（≈ 流长度的 2.07 倍），ring(byte/bitset) 均为 **0 次 / 0 B**。

- map 平均每个段做 3 次分配（`new_data` 拷贝、红黑树节点、`flush` 时的 string
  拷贝），分配字节数约为流长度的 2.3–3.2 倍；
- 环形缓冲区在构造时只分配 3 次（数据环形区、位图 words、`ByteStream` 的环形区），
  之后 `push/flush` **0 次分配**。
- 这是 map 版本在小段长下慢 3–9 倍的主要原因：分配/释放、堆元数据以及不可预测的
  内存布局都会直接吃掉时间，但在纯吞吐数字里看不出来源。

### 4.5 缓存与 CPU 实测（perf）

perf 权限开放后（`perf_event_paranoid = -1`），在 P-core 上用真实 PMU 采集
（`taskset -c 0`）。负载：`streaming`，8 MiB 数据、1 KiB 段、64 KiB 窗口、
reps = 200（共 1.6 GiB），事件为 cycles / instructions / L1-dcache-loads &
misses / l2_rqsts.references & miss / LLC-loads & misses：

| impl | ns/B | cycles/B | instr/B | IPC | L1d miss 率 | L1d misses/B | L2 hit | LLC miss 率 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| map | 0.053 | 0.324 | 1.325 | 4.09 | 4.06% | 0.0147 | 70.5% | 1.07% |
| ring(byte) | 0.022 | 0.154 | 0.383 | 2.49 | 11.59% | 0.0106 | 66.6% | 0.81% |
| ring(bitset) | 0.022 | 0.153 | 0.400 | 2.63 | 10.74% | 0.0104 | 66.5% | 0.83% |

- map 的 **指令数约为 ring 的 3.3–3.5 倍**，cycles 约为 2.1 倍；它的 IPC 反而更高
  （4.09 vs 2.5），说明差距不是 cache miss 造成的停顿，而是**做了更多工作**：
  每段 3 次堆分配 + map/string 操作（见 4.4）；
- L1d miss **率** map 更低（4.06% vs 10.7–11.6%），但它的 load 数量是 ring 的 4 倍，
  按字节摊薄的 L1 miss 反而更高（0.0147 vs 0.0104–0.0106，约 +40%）；
- L2 命中率两者接近（66–70%），LLC miss 都在 1% 左右；
- byte 位图与 bitset 的差异在测量误差内（cycles 0.153 vs 0.154），bitset 的收益是
  位图内存 8×（64 KiB → 8 KiB），而非命中率。

> 计数口径：perf 统计整个进程（含一次性的 pattern/segment 构造），但 200 次重复
> 使推送循环占绝对主导；全部为用户态事件，无 multiplexing。

### 4.6 分析

- **小/中等段长（64 B – 1 KiB，最接近真实 TCP MSS）**：环形缓冲区较 map 领先
  Release 下约 1.4–9.2 倍，Debug 下 2–10 倍；`streaming` 稳态下为 2.4 倍（0.022 vs
  0.053 ns/B）。收益主要来自：每段零分配（4.4）、位图去重代替字符串合并、顺序流
  快速路径直接零拷贝写输出；perf 也显示 map 的指令数是 ring 的 3.3–3.5 倍。
- **overlap**：环形缓冲区在 Release 下稳定快 2–5 倍，重叠字节只需跳过已置位位置，
  而 map 版本要走 segment 合并和字符串拼接。
- **reverse（map 的最坏情况）**：map 每来一个新段都要和已有大段拼接，呈现 O(n²)
  特征；环形缓冲区代价与到达顺序无关。
- **超大块（16–64 KiB）且段数很少时**：map 略快（ring 的 0.6–0.9 倍）。此时分配
  开销被摊薄，而 ring 需要额外扫描位图并多一遍内存访问。真实 TCP 段长通常 ≤ 1.4 KiB，
  不属于这个区间。
- **byte 位图 vs bitset**：Release 下吞吐在 ±15% 以内（0.86–1.02x），Debug 小段长下
  bitset 慢 10–25%（标量字扫描 vs glibc 的 SIMD `memchr`），但位图内存从 C 降到
  C/8。当前实现选择 bitset，换取可预测的内存占用。
- **内存**：环形缓冲区固定 `capacity + capacity/8` 字节并一次性分配；map 的内存随
  段数增长，每段约 80 B 额外开销且碎片化。

## 5. 结论

- 对 Lab1 的典型负载（乱序、重叠、重传、小段长），环形缓冲区 + 位图在时间、空间和
  实现复杂度上都优于 `map<start, string>`；最直接的证据是 4.4 的分配统计：map 每个
  段约 3 次堆分配，而 ring 在构造后 **0 次**；
- perf 实测（4.5）给出了更精确的结论：map 的指令数是 ring 的 3.3–3.5 倍，cycles 约
  2.1 倍；map 的 L1d miss 率反而更低，但绝对 L1 miss/字节高约 40%，L2 命中率接近。
  差距的主要来源是分配与每段工作量，而不是缓存命中率；
- byte 位图 vs bitset：后者位图内存降低 8 倍，Release 下吞吐差异在 ±15% 以内；
  Debug 小段长下 bitset 略慢（标量字扫描 vs SIMD `memchr`）。当前选择 bitset；
- map 版本仅在大段长、段数极少的场景略占优势，而这并非 TCP 重组的典型场景；
- 位图方案的实现也更简单：不需要处理区间合并的各种边界情况。

## 6. Test Results

```
18/18 Test #18: t_strm_reassem_single ............   Passed
...
15/18 Test #32: t_strm_reassem_bench .............   Passed    0.03 sec
18/18 Test #52: t_socket_dt ......................   Passed    0.00 sec

100% tests passed, 0 tests failed out of 18

Total Test time (real) =   0.36 sec
```

Remaining Bugs:
None.

- Optional: I had unexpected difficulty with: 窗口边界（`bytes_read` vs
  `bytes_written`）和 `flush` 的部分写入计数。
- Optional: I think you could make this lab better by: [describe]
- Optional: I was surprised by: 数据集较小时 map 的实现反而更快，说明数据结构的选择
  强依赖于段的大小和工作集。
- Optional: I'm not sure about: [describe]

