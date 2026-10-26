Lab 2 Writeup
=============

My name: [your name here]

My SUNet ID: [your sunetid here]

This lab took me about [n] hours to do. I [did/did not] attend the lab session.

I worked with or talked about this assignment with: [please list other sunetids]

<!--
以下为根据 lab2.pdf 整理的实现前笔记（任务 + 挑战）。
完成实现后请把内容精简/翻译进下面三个小节，并删除本笔记区块；
最终 writeup 要求 20–50 行、每行不超过 80 字符。
-->

## Lab 2 任务与主要挑战（摘自 lab2.pdf）

### 一、需要完成的任务

1. `libsponge/wrapping_integers.cc`：64 位绝对序号与 32 位 seqno 互转
   - `WrappingInt32 wrap(uint64_t n, WrappingInt32 isn)`：
     绝对序号 → seqno，即相对 ISN 后对 2^32 取模。
   - `uint64_t unwrap(WrappingInt32 n, WrappingInt32 isn, uint64_t checkpoint)`：
     seqno → 绝对序号。同一个 seqno 对应无穷多个绝对序号
     （n、2^32+n、2^33+n …），返回离 checkpoint 最近的那个；
     checkpoint 保证落在正确答案的 ±2^31 范围内。
     实现提示：直接使用 `wrapping_integers.hh` 提供的辅助函数最简洁。
2. `libsponge/tcp_receiver.cc`（私有成员可加到 `.hh`，公共接口不可改）：
   - `segment_received(seg)`：首个带 SYN 的段记录 ISN；把 payload 和
     FIN（流结束标志）交给 StreamReassembler，索引从 0 开始，
     需通过 unwrap 把 seqno 转成 stream index。
   - `ackno()`：未收到 SYN 时返回 `nullopt`；否则返回窗口左沿
     （first unassembled）的 seqno，即 `wrap(ISN + 1 + bytes_written)`。
   - `window_size()`：first unassembled 到 first unacceptable 的距离，
     等价于 `capacity - stream_out().buffer_size()`。
   - 构造函数、`unassembled_bytes()`、`stream_out()` 已在 `.hh` 提供。
3. 连接生命周期状态机：LISTEN（ackno 为空）→ SYN_RECV（ackno 有值、
   输入未结束）→ FIN_RECV（`stream_out().input_ended()`）；
   ERROR / RST 留到 Lab 4。
4. 测试与提交：`ctest -R wrap`、`make check_lab2`；交作业前依次执行
   `make format`、`git status`（确认已提交干净）、`make`、`make check_lab2`。
   只修改 `libsponge` 顶层的 `.hh/.cc`（以及 Lab 0 的 `webget.cc`）。

### 二、主要挑战

1. 三套索引的区别与转换（最容易混淆的地方）：
   - seqno：32 位、会回绕、包含 SYN/FIN、从随机 ISN 开始；
   - absolute seqno：64 位、不回绕、包含 SYN/FIN、从 0 开始；
   - stream index：64 位、不回绕、不含 SYN/FIN、从 0 开始。
   PDF 的 `cat` 例子：ISN = 2^32−2 时，'c' 的 seqno 为 2^32−1，
   绝对序号为 1，流索引为 0。
2. 32 位回绕 + 随机 ISN：流长度没有上限（2^32 字节只有 4 GiB），
   seqno 加到 2^32−1 后归零；不能假设从 0 开始，也不能直接用 32 位
   整数比较先后。
3. unwrap 的歧义与边界：同一 seqno 有多个绝对序号候选，必须用
   checkpoint 选最近的；候选可能算出负数（checkpoint 在流的很前面
   时），要饱和到 0；wrap/unwrap 必须保持偏移（相差 17 的 seqno
   对应绝对序号也相差 17）。
4. SYN/FIN 各占一个序号，但不是数据：
   - 带 SYN 的段：其 seqno 就是 ISN，payload 第一个字节是 ISN+1；
   - 带 FIN 的段：payload 最后一个字节是整条流的最后一个字节，
     FIN 自己占它后面那个序号；
   - 同一个段可能同时带 SYN、数据和 FIN。
5. 到达顺序不确定：SYN 可能重传；数据可能乱序或重复；SYN 之前到达的
   数据段无法 unwrap，应丢弃；FIN 重复到达要幂等；窗口外（unacceptable）
   的段不能写入 reassembler，否则 ackno / window_size 会算错。
6. ackno 的时机：收到 SYN 之前必须为空；收到 SYN 之后即使尚无数据
   也有值（ISN+1）。ackno 是"下一个期望的字节"，随已重组字节数前移。
7. window_size 的口径：`capacity` 减去 ByteStream 中尚未被应用读走的
   字节数（`stream_out().buffer_size()`），不要和 `unassembled_bytes()`
   混淆；窗口滑动基准要与 Lab 1 reassembler 的
   `[bytes_read, bytes_read + capacity)` 窗口保持一致。
8. 调试与代码风格：可用 `-DCMAKE_BUILD_TYPE=RelASan`、valgrind、gdb；
   要求现代 C++（不用 malloc/new/裸指针），用异常做防御式检查。
   本仓库编译开了 `-Werror -Weffc++`，构造函数初始化列表要写全所有成员
   （仓库经验，非 PDF 内容）。

Program Structure and Design of the TCPReceiver and wrap/unwrap routines:
[]

Implementation Challenges:
[]

Remaining Bugs:
[]

- Optional: I had unexpected difficulty with: [describe]

- Optional: I think you could make this lab better by: [describe]

- Optional: I was surprised by: [describe]

- Optional: I'm not sure about: [describe]
