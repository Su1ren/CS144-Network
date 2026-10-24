Lab 0 Writeup
=============

My name: [your name here]

My SUNet ID: [your sunetid here]

This lab took me about [n] hours to do. I [did/did not] attend the lab session.

My secret code from section 2.1 was: [code here]

- Optional: I had unexpected difficulty with: [describe]

- Optional: I think you could make this lab better by: [describe]

- Optional: I was surprised by: [describe]

- Optional: I'm not sure about: [describe]

# Test Results

Test project /home/bhd/CS144-Network
      Start 26: t_byte_stream_construction
 1/10 Test #26: t_byte_stream_construction .......   Passed    0.00 sec
      Start 27: t_byte_stream_one_write
 2/10 Test #27: t_byte_stream_one_write ..........   Passed    0.00 sec
      Start 28: t_byte_stream_two_writes
 3/10 Test #28: t_byte_stream_two_writes .........   Passed    0.00 sec
      Start 29: t_byte_stream_capacity
 4/10 Test #29: t_byte_stream_capacity ...........   Passed    0.19 sec
      Start 30: t_byte_stream_many_writes
 5/10 Test #30: t_byte_stream_many_writes ........   Passed    0.00 sec
      Start 31: t_byte_stream_bench
 6/10 Test #31: t_byte_stream_bench ..............   Passed    0.02 sec
      Start 32: t_webget
 7/10 Test #32: t_webget .........................   Passed    4.50 sec
      Start 49: t_address_dt
 8/10 Test #49: t_address_dt .....................   Passed    0.00 sec
      Start 50: t_parser_dt
 9/10 Test #50: t_parser_dt ......................   Passed    0.00 sec
      Start 51: t_socket_dt
10/10 Test #51: t_socket_dt ......................   Passed    0.00 sec

100% tests passed, 0 tests failed out of 10

Total Test time (real) =   4.73 sec
[100%] Built target check_lab0

# Byte Stream：`std::deque<char>` vs `std::vector<char>` 环形缓冲

## 实现改动

- `libsponge/byte_stream.hh` / `libsponge/byte_stream.cc`：容器由 `std::deque<char>` 改为
  `std::vector<char>`，构造时一次性分配 `capacity` 字节，并增加两个下标：
  - `_head`：缓冲区中第一个有效字节的位置；
  - `_size`：当前缓冲的字节数（尾部位置为 `(_head + _size) % _capacity`）。
- `write()`：`tail = (_head + _size) % _capacity`，最多两段 `std::memcpy`，不再逐元素插入；
- `peek_output()`：最多两段拷贝进返回的 `std::string`，不会再逐字节走迭代器；
- `pop_output()`：只移动 `_head` 并减少 `_size`，**O(1)**，不搬运任何数据；
- 对外行为完全不变（包括容量截断、回绕、计数），由基准测试中的 `verify()` 对两种实现同时校验。

## 基准测试

新增 `tests/byte_stream_bench.cc`，注册为 CTest 用例 `t_byte_stream_bench`（`make check_lab0` 会自动执行，
Debug 下约 0.02 s）。要点：

- 同一个二进制内保留旧的 deque 实现（`DequeByteStream`）作为基线，用模板对两种实现跑完全相同的负载；
- 覆盖用户要求的三类操作以及一个稳态回环：
  - `write`：插入（追加）；
  - `peek`：访问（复制但不消费）；
  - `pop`：删除（消费）；
  - `read+write`：容量 64 KiB 的缓冲，写满后每次读 `chunk` 再写回 `chunk`，模拟 TCP 式收发；
- 每个测量搬运 `total` 字节、按 `chunk` 切分调用，重复 `reps` 次并取最小值，结果换算为 **ns/字节**；
- 数据量维度取 `chunk = 64 B / 256 B / 1 KiB / 4 KiB / 16 KiB / 64 KiB`；
- `check_lab0` 默认参数：`total = 512 KiB, reps = 3, capacity = 64 KiB`；
- 性能数据另用 Release 构建复测：`total = 8 MiB, reps = 5`。

测试环境：Intel Core i7-14700K，g++ 13.3.0，Linux。

### Debug（`-ggdb3 -Og`，`make check_lab0` 默认参数）

| chunk | op | deque ns/B | vector ns/B | 提升 |
|---:|---|---:|---:|---:|
| 64 B | write | 0.695 | 0.092 | 7.59x |
| 64 B | peek | 0.801 | 0.310 | 2.58x |
| 64 B | pop | 0.262 | 0.054 | 4.85x |
| 64 B | read+write | 1.388 | 0.369 | 3.77x |
| 256 B | write | 0.099 | 0.025 | 4.03x |
| 256 B | peek | 0.592 | 0.077 | 7.70x |
| 256 B | pop | 0.077 | 0.014 | 5.66x |
| 256 B | read+write | 0.746 | 0.098 | 7.61x |
| 1024 B | write | 0.046 | 0.012 | 3.89x |
| 1024 B | peek | 0.559 | 0.023 | 23.91x |
| 1024 B | pop | 0.029 | 0.003 | 8.50x |
| 1024 B | read+write | 0.626 | 0.033 | 19.11x |
| 4096 B | write | 0.038 | 0.013 | 2.92x |
| 4096 B | peek | 0.556 | 0.011 | 48.95x |
| 4096 B | pop | 0.022 | 0.001 | 25.32x |
| 4096 B | read+write | 0.617 | 0.023 | 27.18x |
| 16384 B | write | 0.034 | 0.012 | 2.82x |
| 16384 B | peek | 0.557 | 0.005 | 104.41x |
| 16384 B | pop | 0.020 | 0.000 | 90.34x |
| 16384 B | read+write | 0.647 | 0.025 | 26.24x |
| 65536 B | write | 0.034 | 0.014 | 2.45x |
| 65536 B | peek | 0.600 | 0.015 | 41.23x |
| 65536 B | pop | 0.016 | 0.000 | 252.82x |
| 65536 B | read+write | 0.655 | 0.028 | 23.13x |

### Release（`-O3`，total = 8 MiB，reps = 5）

| chunk | op | deque ns/B | vector ns/B | 提升 |
|---:|---|---:|---:|---:|
| 64 B | write | 0.233 | 0.076 | 3.07x |
| 64 B | peek | 0.400 | 0.194 | 2.07x |
| 64 B | pop | 0.062 | 0.054 | 1.15x |
| 64 B | read+write | 0.465 | 0.252 | 1.85x |
| 256 B | write | 0.050 | 0.033 | 1.50x |
| 256 B | peek | 0.242 | 0.046 | 5.31x |
| 256 B | pop | 0.028 | 0.014 | 2.09x |
| 256 B | read+write | 0.288 | 0.065 | 4.41x |
| 1024 B | write | 0.049 | 0.023 | 2.12x |
| 1024 B | peek | 0.206 | 0.015 | 13.79x |
| 1024 B | pop | 0.020 | 0.003 | 5.79x |
| 1024 B | read+write | 0.233 | 0.030 | 7.77x |
| 4096 B | write | 0.040 | 0.019 | 2.06x |
| 4096 B | peek | 0.202 | 0.011 | 18.01x |
| 4096 B | pop | 0.019 | 0.001 | 22.79x |
| 4096 B | read+write | 0.229 | 0.019 | 12.01x |
| 16384 B | write | 0.038 | 0.019 | 2.02x |
| 16384 B | peek | 0.196 | 0.006 | 32.89x |
| 16384 B | pop | 0.017 | 0.000 | 80.37x |
| 16384 B | read+write | 0.244 | 0.024 | 9.98x |
| 65536 B | write | 0.037 | 0.022 | 1.70x |
| 65536 B | peek | 0.196 | 0.014 | 13.74x |
| 65536 B | pop | 0.017 | 0.000 | 304.76x |
| 65536 B | read+write | 0.245 | 0.028 | 8.73x |

> 注：`pop` 的 vector 一列显示 0.000，表示小于 0.0005 ns/B——vector 的 pop 是 O(1)，成本被摊到整个
> `total` 上；deque 的 `erase(begin, begin+k)` 则是 O(k)，所以 chunk 越大差距越大。

## 分析

- **write（插入）**：vector 快 1.7–3.1 倍。deque 每跨一个 512 B 块就要 `malloc` 一次并逐元素插入，
  vector 只是一次 `memcpy`；chunk 越小，每次调用的固定成本占比越高，差距收窄。
- **peek（访问）**：64 B 时只有 2.1 倍，因为两种实现都被返回 `std::string` 的堆分配支配；
  chunk ≥ 1 KiB 后 deque 的逐字节迭代器循环成为瓶颈，差距扩大到 13–33 倍。
- **pop（删除）**：deque 每次 O(k) 并释放已清空的块，vector 只移动下标。64 KiB 块时按搬运字节摊薄
  仍有约 300 倍差距。
- **read+write（稳态）**：最接近后续实验（TCP 收发）的模式，Release 下从 1.9 倍（64 B）提升到
  8–12 倍（≥ 1 KiB）。
- 结论：所有负载下 vector 版本都不慢于 deque 版本；剩余开销主要来自 `peek_output`/`read` 每次返回
  新 `std::string` 的堆分配，这是接口决定的，与容器选择无关。

## 与 Lab1 的衔接

- Lab1 的 `StreamReassembler` 沿用同样的“固定容量 + 环形下标”思路：乱序字节存放在
  `capacity` 大小的环形缓冲区中，用 `index % capacity` 定位，并用位图标记哪些位置已收到。
- 为让 reassembler 的 `flush()` 和顺序流快速路径避免临时 `std::string`，给
  `ByteStream` 增加了零拷贝重载 `write(const char *data, size_t len)`；
  `write(const std::string &)` 直接转发到它，行为不变。
- Lab1 中两种 reassembler 数据结构（`map<start, string>` vs 环形缓冲区 + 位图）的吞吐对比见
  `lab1.md`。
