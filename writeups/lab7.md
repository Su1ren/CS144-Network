Lab 7 Writeup
=============

My name: [your name here]

My SUNet ID: [your sunetid here]

My lab partner's SUNet ID: [none]

I also worked with or collaborated with: [none]

This lab took me about [n] hours to do. I [did not] attend the lab session.

Solo portion:
-------------

- 成功用"自己与自己"的两个实例建立了端到端 TCP 连接：
  `./apps/lab7 server 127.0.0.1 47210` 与
  `./apps/lab7 client 127.0.0.1 47211`。
  server 打印 `Listening for incoming connection...`，client 打印
  `Successfully connected to 172.16.0.100:1234.`，server 打印
  `New connection from 192.168.0.50:4277.`，说明三次握手完成。
- 双向数据传输成功：client 的 stdin（`hello from client` 等两行）出现在
  server 的 stdout；server 的 stdin（`hello from server` 等两行）出现在
  client 的 stdout。
- 连接正常结束：两侧 stdin 关闭后各自结束 outbound ByteStream，继续接收
  对端剩余数据，随后两个进程都在 stderr 打印
  `TCP connection finished cleanly.` 和 `Exiting... done.` 并退出。
- 1 MiB 文件传输成功：`dd` 生成 /tmp/big.txt，server 从 stdin 发送，
  client 的 stdout 重定向到 /tmp/big-received.txt。两端文件均为
  1 048 576 字节，sha256 相同：
  `f96f6bee159c558c2f0134cc6eb03e7e15c866c5e212f6ecbf03470d9f4c9f15`
- 传输耗时：client 92.3 s、server 87.7 s，约 11–12 KiB/s。lab7 的模拟
  网络每 50 ms tick 一次且未做吞吐优化，本实验也不考察性能。
- 代码改动：lab0–6 的实现（ByteStream、TCP Sender/Receiver、
  NetworkInterface、Router）无需任何修改即可互通，lab7 程序本身也
  无需修改。唯一的构建修复是给 `apps/bouncer.cc` 补上
  `#include <optional>`（当前头文件不再传递包含它）。
- 测试环境说明：官方 relay `cs144.keithw.org`（171.64.65.14）从本机
  发出 UDP 后无任何回包，两端分别停在 `Listening` / `Connecting`。
  因此 solo 测试改用本地 UDP 中继（127.0.0.1:47210/47211），复现
  bouncer 的配对语义；两个实例仍然经过完整的 Ethernet/IPv4/ARP/TCP
  模拟栈和真实 UDP socket。

Group portion:
--------------

- 本实验中我没有 lab partner，因此 group portion 未完成。
- 跨实现互通（双方分别作为 client / server）与跨实现 1 MiB 传输
  这两项无法在本环境验证，如实记录，未用他人实现冒充结果。

Creative portion (optional):
----------------------------

- 由于官方 relay 不可达，写了一个单端口对的本地中继替代 bouncer：
  两个 UDP socket 分别绑定偶数/奇数端口，学习两端源地址后互相转发。
  该脚本只放在 /tmp 作为测试脚手架，不进入提交代码，也不影响
  libsponge 的任何实现。
- 该练习把 lab0–6 真正串成了一条链路：ByteStream → TCP Sender/Receiver
  → IPv4 数据报 → Ethernet/ARP → Router → NetworkInterface → UDP。

Other remarks:
--------------

- 复现命令（在 build 目录，先运行本地中继）：
  `python3 relay.py 47210`，
  `./apps/lab7 server 127.0.0.1 47210 < /tmp/big.txt`，
  `./apps/lab7 client 127.0.0.1 47211 < /dev/null > /tmp/big-received.txt`，
  最后 `sha256sum` 比较两端文件。
- 关闭过程符合预期：先结束本方向 ByteStream，继续接收对端数据，双方都
  FIN 后等待 linger 结束再退出，没有出现提前断开或数据截断。

- Optional: I had unexpected difficulty with: 官方 relay 无响应，需要自建
  本地中继才能完成 solo 测试。

- Optional: I think you could make this lab better by: 提供本地的 relay
  替代方案或让 relay 状态可查询，避免课程结束后无法复现。

- Optional: I was surprised by: 之前各 lab 的实现未经修改即可直接互通，
  说明分层接口（ByteStream / TCPConnection / NetworkInterface）抽象得
  足够干净。

- Optional: I'm not sure about: 在真实广域网 RTT 下，当前 TCP 实现的吞吐
  表现如何；本实验的 50 ms 轮询粒度无法反映这一点。

