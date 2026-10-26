Lab 3 Writeup
=============

My name: [your name here]

My SUNet ID: [your sunetid here]

This lab took me about [n] hours to do. I [did/did not] attend the lab session.

I worked with or talked about this assignment with: [please list other sunetids]

<!--
以下为根据 lab3.pdf 整理的实现前笔记（任务 + 目标 + 易错点）。
完成实现后请把内容精简/翻译进下面两个小节，并删除本笔记区块；
最终 writeup 要求 20–50 行、每行不超过 80 字符。
-->

## Lab 3 任务与目标（摘自 lab3.pdf）

### 一、本实验目标

- 实现 TCP 的发送方：把出站 ByteStream 切分成 TCPSegment，
  处理 ACK 与接收方窗口，并重传未被确认的段（ARQ）。
- 发送方负责写段中的 seqno、SYN、payload、FIN；
  只读取接收方写的 ackno 和 window_size。
- 测试面向公开接口；CLOSED / SYN_SENT / SYN_ACKED / FIN_SENT /
  FIN_ACKED 等状态由 next_seqno_absolute 与 bytes_in_flight 推导，
  不需要额外的状态机。ERROR/RST 留到 Lab 4。
- Lab 4 将把 TCPSender 与 TCPReceiver 组合成 TCPConnection。

### 二、需要完成的任务

1. 在 `libsponge/tcp_sender.cc` 实现完整公开接口
   （可在 `.hh` 加私有成员或辅助类，公共接口不可改）：
   - `fill_window()`：从 ByteStream 读取数据，尽量填满接收方窗口；
     每段不超过 `TCPConfig::MAX_PAYLOAD_SIZE`（1452 字节）；
     SYN 和 FIN 各占一个序列号，也占窗口空间；
     窗口为 0 时按 1 字节处理，发送单字节探测段以促使对端更新窗口。
   - `ack_received(ackno, window_size)`：删除所有序列号都
     小于 ackno 的 outstanding 段；若窗口有新增空间则再次
     `fill_window()`。
   - `tick(ms_since_last_tick)`：推进重传定时器；超时后重传
     最早未确认的段，并在窗口非零时递增连续重传计数、
     RTO 翻倍（指数退避）、重启定时器。
   - `send_empty_segment()`：发送占 0 序列号的段（seqno 正确），
     不进入 outstanding，也不重传。
2. 重传定时器规则（PDF 3.1）：
   - 只能用 `tick` 计算时间，禁止调用系统时钟；
   - 发出占序列号的段时，若定时器未运行则启动（当前 RTO）；
   - 所有 outstanding 数据被确认后停止定时器；
   - 超时：重传最早的未确认段；若窗口非零，连续重传计数 +1、
     RTO 翻倍；然后按当前 RTO 重启定时器；
   - 收到确认了新数据的 ackno 时：RTO 复位为初始值；
     若仍有 outstanding 数据则重启定时器；连续重传计数清零。
3. 收到第一个 ACK 之前，把接收方窗口按 1 字节处理。
4. 部分确认按整段处理（不裁剪）；重传只发最早的一段，
   不把多个段合并重传。
5. 测试与提交：`make check_lab3`；交作业前依次执行
   `make format`、`git status`（确认已提交干净）、`make`、
   `make check_lab3`。只修改 libsponge 顶层的 `.hh/.cc`。

### 三、主要挑战与易错点

1. 序列号空间：next_seqno_absolute 包含 SYN/FIN；bytes_in_flight
   是所有未确认段占用的序列号总数；状态判定依赖两者关系。
2. 窗口与 0 窗口探测：窗口为 [ackno, ackno+window_size)，
   每段必须完整落在窗口内；窗口为 0 时仍允许发送 1 字节。
3. outstanding 管理：只为占序列号的段保存副本；
   payload 用引用计数的 Buffer 共享，复制段的代价很小。
4. 定时器：启动/停止/重启的时机，以及 RTO 仅在确认新数据时
   复位、在超时时翻倍，是最容易出错的部分。
5. 代码风格：现代 C++（不用 malloc/new/裸指针）、防御式检查、
   可读性与模块化；本仓库开启 `-Werror -Weffc++`，构造函数的
   初始化列表要写全所有成员（仓库经验，非 PDF 内容）。

Program Structure and Design of the TCPSender:
[]

Implementation Challenges:
- 发送、重传窗口记录更新
- 从 ByteStream 读取数据组装报文段
- 设置定时器定期检查 ACK 情况

#### 1. 测试超时：fill_window() 无进展死循环

- 现象：t_send_connect / t_send_transmit / t_send_retx 均 10s
  超时，进程 CPU 满载。
- 原因：流缓冲区为空且尚未 end_input() 时，read() 返回空串，
  段长度 0，available_window 不减少，循环条件恒真；同时不断
  压入空段，内存持续增长。
- 定位：用 timeout 3 ./tests/send_connect 观察 CPU/内存；
  gdb 中断后调用栈停在 fill_window() 的 while 循环。
- 解决：每轮循环必须保证有进展——读不到数据且不需要或放不下
  FIN 时立即 break；用 _next_seqno 严格递增来验证循环终止。

#### 2. 从未发送 SYN / FIN

- 原因：直接把 _next_seqno 当作 payload 序号，没有设置 SYN；
  也没有处理 stream.eof() 之后的 FIN。
- 解决：第一次 fill_window() 先发 SYN 段（seqno = ISN，占 1 个
  序列号）；读到 eof 且窗口剩余空间足够时发 FIN（占 1 个
  序列号），用 _syn_sent / _fin_sent 保证只发一次。

#### 3. 窗口减法导致 size_t 下溢

- 原因：_window_size 是 uint16_t、_bytes_in_flight 是 size_t；
  接收方收缩窗口时 bytes_in_flight > window_size，直接相减会
  得到接近 2^64 的“可用窗口”，导致持续发送。
- 解决：先比较再相减，可用窗口为
  window > bytes_in_flight ? window - bytes_in_flight : 0。

#### 4. 零窗口探测段用错

- 原因：窗口为 0 时调用 send_empty_segment()；零长度段不占
  序列号，接收方不会确认，也就无法探测窗口是否重新打开。
- 解决：把零窗口视为 1 字节，发送 1 字节数据（或 FIN）探测；
  探测段计入 outstanding，重传时不做指数退避。

#### 5. 重复 ACK 重置了重传定时器

- 原因：每次 ack_received() 都 reset 定时器并清零连续重传
  计数；重复 ACK（ackno 不变）也触发，导致重传被无限推迟。
- 解决：记录 _last_ackno，只有确认了新数据
  （abs_ackno > _last_ackno）才执行 RTO 复位、计数清零和
  定时器重启；同时忽略 ackno > _next_seqno 的无效 ACK。

#### 6. tick() 的指数退避未生效

- 原因：只把 _current_retransmission_timeout 乘 2，没有同步到
  RetransmissionTimer，定时器仍按旧 RTO 到期；窗口为 0 时也
  错误地翻倍并累计连续重传次数。
- 解决：翻倍后调用 set_timeout() 同步；仅窗口非零时计数和
  翻倍，但无论窗口是否为零都在重传后 reset 定时器，避免每个
  tick 都重传同一个段。

#### 7. 最终验证

- 根目录（g++）make check_lab3：35/35 通过（约 0.5s）。
- build/（clang++）中 t_send_*：7/7 通过。
- 结论：超时全部消失，SYN/FIN、窗口边界和重传退避行为符合
  lab3.pdf 的规则。

Remaining Bugs:
[]

- Optional: I had unexpected difficulty with: [describe]

- Optional: I think you could make this lab better by: [describe]

- Optional: I was surprised by: [describe]

- Optional: I'm not sure about: [describe]
