Lab 4 Writeup
=============

My name: [your name here]

My SUNet ID: [your sunetid here]

This lab took me about [n] hours to do. I [did/did not] attend the lab session.

I worked with or talked about this assignment with: [please list other sunetids]

<!--
以下为根据 lab4.pdf 整理的实现前笔记（目标 + 任务 + 主要挑战）。
完成实现后请把内容精简/翻译进下面三个小节，并删除本笔记区块；
最终 writeup 要求 20–50 行、每行不超过 80 字符。
-->

## Lab 4 任务与目标（摘自 lab4.pdf）

### 一、本实验目标

- 把 Lab 2/3 实现的 TCPReceiver 与 TCPSender 组装成 TCPConnection，
  完成一个可用的完整 TCP 实现（手册称 lab4 为 “the summit”）。
- TCPConnection 主要做“接线”与连接级簿记：把收发段中的字段分发给
  sender/receiver，处理 RST、纯 ACK、tick，以及最难的
  “连接何时真正结束”。预期只需约 100–150 行代码。
- 测试：make check 共 159 个用例，覆盖自带实现互连、与 Linux 内核
  TCP 互操作、双向传输、双向丢包等场景。
- 性能：tcp_benchmark 两行指标（含/不含重排）均需 ≥ 0.10 Gbit/s，
  并把结果写进 writeup。
- webget 改用 CS144TCPSocket（自己的 TCP）后仍能访问真实 webserver。

### 二、需要完成的任务

1. 在 libsponge/tcp_connection.cc 实现全部公开接口（可在 .hh 加私有
   成员，公共接口不可改）。
2. segment_received(seg)：
   - 若 rst 置位：把出站和入站 ByteStream 都置为 error，连接永久结束
     （此后 active() 恒为 false）。
   - 把段交给 receiver（seqno、syn、payload、fin）。
   - 若 ack 置位：把 ackno 与 window_size 交给 sender。
   - 若来段占用了序列号，必须至少回发一个段，以反映新的 ackno 与窗口。
3. 发段：sender 的 segments_out() 非空时逐段取出，用 receiver 的
   ackno()/window_size() 填写：有 ackno 就设 ack 位并写字段；窗口大于
   header().win 可表示范围时写入 uint16_t 的最大值。
4. tick(ms)：
   - 把时间流逝告诉 sender（只能用 tick 计时，禁止系统时钟）；
   - 连续重传次数 > TCPConfig::MAX_RETX_ATTEMPTS（8）时中止连接，
     并向对端发 RST（空段 + rst 置位）；
   - 判断能否干净结束连接（见第 7 项）。
5. writer 方法：connect()/write()/end_input_stream() 操作 sender 的
   出站 ByteStream 并通知 sender（connect 后要让 sender 发出 SYN）。
   只读访问 capacity()、bytes_in_flight()、unassembled_bytes() 直接
   转发；inbound_stream() 头文件中已实现。
6. RST 的两个发送时机：连续重传超限；析构时连接仍 active()。用 sender
   的 send_empty_segment() 取得正确 seqno 再置 rst，同时把双向
   ByteStream 置 error。
7. 干净关闭（active()=false）的四个先决条件：
   - #1 入站流已完整重组并结束；
   - #2 出站流已被本地应用结束，且 FIN 已全部发出；
   - #3 出站流已被对端全部确认；
   - #4 本地确信对端能完成 #3：Option A 为 linger——满足 #1–#3 后，
     自最后一次收到对端段起已过 10 × cfg.rt_timeout；Option B 为
     passive close——入站流在本地发出 FIN 前就已 EOF，则无需 linger。
   - 实现：成员 linger_after_streams_finish 初值 true；入站流先于出站
     EOF 结束时置 false；#1–#3 满足且（!linger 或已 linger 满
     10 × rt_timeout）时 active() 返回 false。
8. webget 复赛：include "tcp_sponge_socket.hh"、TCPSocket 改为
   CS144TCPSocket、get_URL() 末尾调用 socket.wait_until_closed()，
   然后 make check_webget。
9. 提交：只改 libsponge 顶层 .hh/.cc；依次 make format、git status
   （有未提交改动先提交）、make、make check。

### 三、主要挑战与易错点

1. 连接终止判定（手册明确说最难）：active() 何时为 false——四个
   先决条件、linger 计时、passive close 分支都要正确；结束太早会
   丢掉对端仍需的 ACK，太晚则端口一直被占用。
2. 纯 ACK 的发送：sender 队列没有数据、但来段占用了序列号时，
   仍必须回一个段；用 send_empty_segment() 取得正确 seqno。
3. ack 位语义：出站段有 ackno 就设 ack；入站段只有 ack 置位才把
   ackno/window 交给 sender；连接最开始可能还没有 ackno。
4. RST 的收发两条路径，以及析构函数中发 RST（注意不能抛异常导致
   崩溃）；两者都要把双向 ByteStream 置 error。
5. tick 与重传上限：MAX_RETX_ATTEMPTS=8，超限即发 RST，注意不要
   与 sender 内部的重传计数重复计算。
6. 窗口字段截断：receiver 的窗口可能超过 uint16_t，写 header().win
   时取 std::numeric_limits 的最大值。
7. 小容量/零窗口：用 tcp_udp -w 1 手工测试，任意长度字符串都必须
   能发完，不能卡在窗口探测上。
8. 手工联调：两个终端分别跑 tcp_udp -l 与客户端，观察 Ctrl-D 后
   client 是否 linger 10 秒、server 是否立即干净退出。
9. 代码风格：Modern C++（不用 malloc/new/裸指针）、防御式编程；
   本仓库开启 -Werror -Weffc++，构造函数初始化列表要写全所有成员
   （仓库经验，非 PDF 内容）。

Program Structure and Design of the TCPConnection:
[]

Implementation Challenges:
[]

Remaining Bugs:
[]

## 手册第 6/7/8 节测试记录（2026-10-29）

构建：第 6、8 节用 `build/`（Debug，本机 x86_64 Linux）；
第 7 节用 `build-release/`（Release）。

### 第 6 节 Testing（手工联调）

1) 基本连通与关闭流程（`./apps/tcp_udp -l/-c 127.0.0.1 9090`）

- 双向各发送一行文本，双方都能正确收到对方内容。
- 客户端先关闭 stdin（Ctrl-D）：客户端打印 outbound finished/acked，
  服务端打印 `Inbound stream ... finished cleanly`。
- 服务端随后关闭 stdin：立即打印 `TCP connection finished cleanly`
  并退出（被动关闭、无 linger），实测服务端总时长 4.00 s。
- 客户端收到 FIN 后打印 `Waiting for lingering segments...`，
  linger 10 s 后打印 `TCP connection finished cleanly` 退出；
  实测客户端总时长 13.49 s（3.49 s 传输 + 10.00 s linger）。

2) 1 字节接收窗口（双方均加 `-w 1`，端口 9091）

- 双向发送 `hello how are you doing`（21 字节），在 1 字节窗口下
  两端均完整收到，未卡在零窗口探测。
- 关闭流程与上一项一致，双方均 cleanly 结束，客户端 linger 10 s。

### 第 7 节 Performance（`./apps/tcp_benchmark`，Release）

```
CPU-limited throughput                : 13.64 Gbit/s
CPU-limited throughput with reordering: 12.96 Gbit/s
```

两项均远高于手册要求的 0.10 Gbit/s（100 Mbit/s）。

### 第 8 节 webget revisited

- `get_URL()` 已改用 `CS144TCPSocket`，并在末尾调用
  `socket.wait_until_closed()`。
- `make check_webget`（等价于
  `cmake --build build --target check_webget`）：`t_webget` 通过，
  用时 0.99 s。
- 手动运行 `./apps/webget cs144.keithw.org /nph-hasher/xyzzy`，
  stdout 末行为
  `7SmXqWkrLKzVBCEalbSPqBcvs11Pw263K7x4Wv3JckI`，与测试期望一致；
  DEBUG 显示连接建立、入站流 cleanly 结束、出站 FIN 全部确认、
  TCP 连接 cleanly 结束。
- 手册示例中的 `/hasher/xyzzy` 当前在服务器上返回 404（TCP 本身
  工作正常）；测试脚本使用的是 `/nph-hasher/xyzzy`。

环境备注：本机 Docker 把 `FORWARD` 链默认策略设为 `DROP`，tun144
的报文会被丢弃，sponge 栈无法访问外网。跑第 8 节前需放行（重启或
Docker 重新配置后需重新添加）：

```bash
sudo iptables -I FORWARD 1 -s 169.254.144.0/24 -j ACCEPT
sudo iptables -I FORWARD 1 -d 169.254.144.0/24 -j ACCEPT
```

## 问题记录：零窗口探测未被响应导致的偶发超时

### 1. 现象与范围

`make check_lab4` 有 4 个用例超时（10 s）：

- `t_icnR_128K_8K_l`、`t_icnR_128K_8K_lL`
- `t_icnD_128K_8K_l`、`t_isnD_128K_8K_l`

共同点：`128K` 传输、`8K` 窗口、带**下行丢包**（`-l 0.1`）。
同类但只带上行丢包（`-L`）的用例全部通过，说明问题出在
“接收方向遇到丢包/乱序”的处理上。

### 2. 定位过程

1) 单独运行 `t_icnR_128K_8K_l` 20 次，全部通过（每次约 0.4 s），
   说明不是必然失败；再单独重复 `t_isnD_128K_8K_l` 10 次，
   有 1 次超时（约 10%），确认是偶发问题。
2) 串行运行这 4 个用例，`t_isnD_128K_8K_l` 稳定复现超时，日志停在：

   ```
   DEBUG: Outbound stream ... finished (56000 bytes still in flight).
   DEBUG: Outbound stream ... has been fully acknowledged.
   ```

   即出站数据（含 FIN）已被对端全部确认，连接卡在“继续接收数据”。
3) 在 tun144 上抓包（`tcpdump -i tun144 -w ...`），用 tshark 查看
   失败那次连接末尾的报文：

   ```
   服务器 -> 客户端   ACK  ack=38513  win=0
   客户端 -> 服务器   ACK  seq=38512  ack=131074  len=0   # 0.2 s 后
   客户端 -> 服务器   ACK  seq=38512  ack=131074  len=0   # 0.6 s
   客户端 -> 服务器   ACK  seq=38512  ack=131074  len=0   # 1.5 s
   客户端 -> 服务器   ACK  seq=38512  ack=131074  len=0   # 3.1 s
   客户端 -> 服务器   ACK  seq=38512  ack=131074  len=0   # 6.4 s
   ```

   之后我们的程序不再发出任何报文，直到测试超时。
   这些 `seq = rcv_nxt-1、len = 0` 的报文是 Linux 的**零窗口探测**
   （persist timer 退避）。

### 3. 根因

1. 下行 10% 丢包使接收端出现空洞，乱序数据在缓冲区积压；
   空洞被补上后，8K 数据一次性交付给应用，接收缓冲区瞬间满，
   于是我们通告 `win=0`。
2. 应用随后把数据读走，接收窗口重新打开。但我们的 TCPConnection
   只在“收到的段占用了序列号”时才发送报文，应用读数据本身
   不会触发任何发送，因此**窗口更新永远发不出去**。
3. 对端进入 persist 状态，用长度为 0 的探测段询问窗口；
   我们的 `segment_received` 只处理
   `length_in_sequence_space() > 0` 的段，对这种探测段直接忽略。
4. 结果：对端等窗口更新，我们等数据，双方死锁到超时。

这也解释了为什么只有“下行丢包 + 大窗口传输”会偶发失败：
只有丢包才会造成窗口瞬间归零并触发 persist 探测；上行丢包
由本地 sender 重传即可，不经过这条路径。

### 4. 修复

在 `TCPConnection::segment_received()` 中，对不占序列号的段补充
一条规则：若对端序号不等于当前 ackno（即零窗口探测或过期的
重复纯 ACK）且接收窗口已重新打开，则回一个空 ACK 通告新窗口。

```cpp
    if (seg.length_in_sequence_space() > 0) {
        _sender.fill_window();
        if (_sender.segments_out().empty()) {
            _sender.send_empty_segment();
        }
    } else if (_receiver.ackno().has_value() && seg.header().seqno != _receiver.ackno().value() &&
               _receiver.window_size() > 0) {
        // 零窗口探测（或过期的重复纯 ACK）：窗口已重新打开，回 ACK 通告新窗口
        _sender.send_empty_segment();
    }
    _flush_sender();
```

`send_empty_segment()` 负责取得正确 seqno，`_flush_sender()` 负责
填写 ackno / win。正常对端发来的纯 ACK，其 seqno 恰好等于我们的
ackno，不会命中该分支，因此不会产生额外 ACK 或 ACK 循环。

### 5. 验证

| 项目 | 修复前 | 修复后 |
| --- | --- | --- |
| `t_isnD_128K_8K_l` 单独重复 | 1/10 超时 | 0/30 |
| 4 个原失败用例 × 10 轮（40 次） | 会超时 | 0/40 |
| 完整 `^t_` 测试集 | 4 个超时 | 164/164 通过（46.07 s） |
| `tcp_benchmark`（Release） | 13.64 / 12.96 Gbit/s | 8.08–13.60 / 7.28–12.65 Gbit/s |

性能数值随机器负载/CPU 频率在 8–13 Gbit/s 间波动，均远高于
0.10 Gbit/s 的要求；该补丁只在探测段路径上多一次分支判断，
对吞吐无实质影响。

### 6. 复盘

- 自动化用例全过不代表边界情况正确：这类“纯 ACK 不响应”的问题
  只在 persist（零窗口探测）场景下暴露，必须靠带丢包的用例发现。
- 抓包时要重点看 `win=0` 之后的报文：`seq = rcv_nxt-1 且 len = 0`
  是识别零窗口探测的关键特征。
- 先做可复现性统计（20 次 / 10 次）再抓包，避免被偶发失败误导。

- Optional: I had unexpected difficulty with: [describe]

- Optional: I think you could make this lab better by: [describe]

- Optional: I was surprised by: [describe]

- Optional: I'm not sure about: [describe]
