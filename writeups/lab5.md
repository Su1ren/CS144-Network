Lab 5 Writeup
=============

My name: [your name here]

My SUNet ID: [your sunetid here]

This lab took me about [n] hours to do. I [did/did not] attend the lab session.

<!--
以下为根据 lab5.pdf 整理的实现前笔记（目标 + 任务 + 主要挑战）。
完成实现后请把内容精简/翻译进下面三个小节，并删除本笔记区块；
最终 writeup 要求 20–50 行、每行不超过 80 字符。
-->

## Lab 5 任务与目标（摘自 lab5.pdf）

### 一、本实验目标

- 实现网络接口 NetworkInterface：Internet 数据报（IP）与链路上的
  以太网帧之间的双向转换，位于 TCP/IP 栈最底层（经 TAP 设备与
  Linux 交换原始以太网帧）。
- 核心是 ARP（RFC 826）：按下一跳 IP 查询并缓存其以太网地址；
  该映射只是 "soft state"，丢失后可自然重建，不影响正确性。
- 预期代码量约 100–150 行（network_interface.cc）。
- 测试：make check_lab5（ARP 单元测试 + webget）；整套为 make check。
- 复用：Lab 6 会把同一组件放进 IP 路由器，在多个接口间转发数据报。
- webget 全栈化：把 CS144TCPSocket 换成 FullStackSocket，使
  "应用 → TCPConnection → TCP-in-IP → NetworkInterface" 全部由自己
  实现，仍能访问真实 web 服务器。

### 二、需要完成的任务

1. send_datagram(dgram, next_hop)（network_interface.cc）
   - 已知 next_hop 的以太网地址：构造 EthernetFrame，
     header.type = EthernetHeader::TYPE_IPv4，
     payload = dgram.serialize()，src = 本机以太网地址，
     dst = 下一跳以太网地址，push 进 frames_out。
   - 未知：广播 ARP 请求（dst = ETHERNET_BROADCAST，
     opcode = OPCODE_REQUEST，sender 填本机 MAC/IP，
     target_ip = next_hop），并把该数据报排队，学到映射后再发。
   - ARP 抑制：同一 IP 在 5 秒内只广播一次请求；这段时间内到达的
     数据报继续排队（既不重复发请求，也不丢弃）。
2. recv_frame(frame)（返回 optional<InternetDatagram>）
   - 过滤：目的 MAC 既不是本机也不是广播地址 → 丢弃（return nullopt）。
   - IPv4 帧：把 payload 解析成 InternetDatagram，parse() 返回
     ParseResult::NoError 才返回，否则 nullopt。
   - ARP 帧：解析 ARPMessage；成功就学习 sender_ip → sender_ethernet，
     保留 30 秒（请求与应答都要学）；若 target_ip 是本机，则回一个
     ARP 应答（opcode = OPCODE_REPLY，sender 填本机，
     target 填提问者）。
3. tick(ms_since_last_tick)
   - 负责全部计时：30 秒映射过期、5 秒 ARP 请求抑制窗口；
     只能用 tick 累计的时间，不能用系统时钟。
4. webget 复赛：apps/webget.cc 中 CS144TCPSocket → FullStackSocket，
   其余逻辑不动；make check_lab5 应通过。
   手工验证：./apps/webget cs144.keithw.org /hasher/xyzzy；
   抓包：sudo tcpdump -i tap10 -w /tmp/packets.tap。
5. 提交：只改 libsponge 顶层 .hh/.cc 与 apps/webget.cc（可加私有成员，
   不改公共接口，不加额外文件）；依次 make format、git status
   （有改动先提交）、make、make check_lab5；写 20–50 行 writeup
   （每行不超过 80 字符）。

### 三、主要挑战与易错点

1. 两个计时器别混：IP→MAC 映射保留 30 秒；ARP 请求抑制窗口 5 秒。
   都从事件发生时刻起算，建议记录"绝对到期时间"而不是剩余时间。
   记录方式可用 unordered_map<uint32_t, pair<EthernetAddress, uint32_t>>，
   平均 O(1) 查找；到期扫描为 O(n)。
2. 待发数据报排队：每个 next_hop 各有等待队列（或队列元素带
   next_hop 字段），学到映射后只发送属于该 next_hop 的数据报；
   手册允许本实验不做超时丢弃，但应在 writeup 中说明。
3. 学习时机：ARP 请求和应答都要学习（先学 sender 字段，再决定是否
   回复），回复时 target 字段用请求里 sender 的信息。
4. 只接受发给自己的帧：目的 MAC 必须是本机或广播；IPv4 解析失败返回
   nullopt，不要抛异常。
5. 序列化：EthernetFrame::payload() 是 BufferList，可用
   payload().append(dgram.serialize())；ARP 用 ARPMessage::serialize()；
   构造 ARPMessage 时 hardware_type / protocol_type / 地址长度等
   保持默认值即可。
6. 手册与仓库版本差异：PDF（2020 秋）写 Address::ipv4 numeric()，
   本仓库实际是 Address::ipv4_numeric()；ARP/Ethernet 类位于
   libsponge/tcp_helpers/，而网络接口在 libsponge/ 顶层。
7. 代码风格：Modern C++（不 new/delete、不用裸指针）；仓库开启
   -Werror -Weffc++，给 NetworkInterface 加成员时构造函数初始化列表
   必须写全（仓库经验，非 PDF 内容）。
8. 边界情况：重复学到的映射应刷新到期时间而非新增；映射过期后
   要重新发起 ARP；空队列、多次抑制窗口内的连续发送都值得单测。

### 四、ARP 缓存结构：A 方案与 B 方案（取舍 + 对比测试安排）

#### 需求

- 按 next_hop IP 查 / 学 / 刷新映射：每次发送数据报、每次收到 ARP
  帧都要做，希望平均 O(1)。
- 按到期时间批量删除：tick 每次调用都要做，希望只碰真正过期的条目，
  而不是每 tick 都全表扫描。
- 30 秒映射 TTL 与 5 秒 ARP 请求抑制窗口是两个独立的计时器。

#### A 方案（采用）

- 数据结构：`unordered_map<uint32_t, ArpEntry>`，
  `ArpEntry = {EthernetAddress, expires_at}`，`expires_at` 为绝对
  毫秒（由 tick 累加得到，不读系统时钟）。
- 查 / 学 / 刷新 O(1)；tick 遍历全表删除 `expires_at <= now`，O(n)。
- 理由：同链路对端数 n 是个位数（本实验用例最多 3 个），全表扫描是
  几十纳秒级，而 ARP 往返是毫秒级，差 5 个数量级。A 无额外不变量，
  bug 面最小，也符合手册 100–150 行的预期。

#### B 方案（对比基线）

- 数据结构：`list<ArpEntry>` 按到期时间递增 +
  `unordered_map<ip, list::iterator>`。
- 关键观察：TTL 固定 30 秒，所以"最后学习时间"的先后就是"到期时间"
  的先后，插入序即到期序。
- 查询 O(1)；刷新用 `splice` 移到尾部保持不变量，O(1) 且不分配；
  tick 从表头弹出过期项，摊销 O(k)（k 为本次真正过期的条目数）。
- 代价：每条目多一个链表节点与一份迭代器；多一条必须始终成立的
  不变量（链表顺序 == 到期顺序），刷新漏写 `splice` 会误删有效条目。

#### 对比表

| 指标 | A：map + 全表扫描 | B：有序 list + 索引 |
|---|---|---|
| tick 空转（无过期） | O(n)，每 tick 都付 | O(1)，只看表头 |
| 批量过期 k 条 | O(n) | O(k) 摊销 |
| 学习 / 刷新 | O(1)，1 次分配 | O(1)，新条目多 1 次分配 |
| 单条内存 | 1 个 map 节点 | map 节点 + list 节点 + 迭代器 |
| 不变量 / bug 面 | 少 | 多一条顺序不变量 |

#### 对比测试程序（`tests/arp_cache_bench.cc`，结果见第六节）

- 把 A、B 写成同接口的两个缓存类（`learn / lookup / tick / size`），
  放在同一个二进制里：A = `ArpCacheScan`，B = `ArpCacheLru`，
  用整型模拟时钟驱动，不依赖 `NetworkInterface`。
- 工作负载：
  1. `tick-only`：n = 1/8/64/512/4096，模拟 1 小时、1 ms 一次 tick，
     报 ns/tick；
  2. `expiry-burst`：n 条同时到期，报 ns/过期条目；
  3. `learn-churn`：每 10 s 刷新一遍，报 ns/learn 与分配次数；
  4. `realistic`：8 个邻居、30 s TTL、1 ms tick、1 小时，报总耗时。
- 计时区只包被测操作；内容一致性、`size()` 相等、A/B 结果逐条相等
  在计时区外校验；每项取 N 次最小值；用全局 `operator new` 计数
  统计分配量。
- Debug（`build/`）跑小规模并注册进 `ctest`；Release
  （`build-release/`）跑大规模取正式数据。
- 预期：纯 tick 负载下 B 自 n≈8 起领先（A 遍历 `unordered_map` 是
  跨节点随机访问，每 tick 都有 cache miss）；学习 / 刷新负载下 A 略优。
  结论按"交叉点在哪 + 实验室真实 n 与 tick 频率落在哪一侧"来写。

### 五、learn_arp_mapping 的引入经过

- 初版：`recv_frame` 学到映射只更新 `_arp_cache_map`；"把该 IP 等待
  队列里的数据报发出去"的代码写在 `tick()` 里。
- 现象：`arp_network_interface` 的 "typical ARP workflow" 在
  `ReceiveFrame{ARP reply}` 之后直接 `ExpectFrame{IPv4}`，中间没有
  tick，于是报"应发送帧但没有帧"。测试 harness 在首个断言失败即退出，
  后面的用例其实没有执行（容易误读成"只有某一个用例坏"）。
- 根因：把"学习映射"与"映射可用后的动作"拆在两个函数里，正确性依赖
  调用方何时 tick——测试不保证，真实协议栈也会白等一个 tick。
- 修正：抽出 `learn_arp_mapping(ip, mac)`：写入 `{mac, now+30000}` 后
  立刻扫描等待表，把已可解析的等待数据报按到达顺序发出并清空对应队列；
  `recv_frame` 的 ARP 分支解析成功后统一调用它（请求、应答都学习），
  `tick` 只负责过期清理。
- 同一轮修掉的另外三个问题：请求也要学习（"learn from ARP request"）；
  5 秒抑制要用 `find` 区分"从未发过"与"刚发过"（`operator[]` 默认值 0
  会把 t=0 的首个请求误判成 5 秒内发过）；过期扫描不能包在
  `ms_since_last_tick > 30s` 里，必须每次 tick 都逐条比较。
- 引入后的不变量：等待队列里的 IP 一定没有有效映射；一旦学到映射，该
  IP 的队列立即清空。发送路径不再依赖 tick 的调用时机。
- 复杂度：learn 为 O(W + D)（W = 等待中的下一跳数，D = 本次发出的
  数据报数），本实验 W 极小。更紧的写法是只清理刚学到的 IP 的队列
  （O(1) 查找），因为其它 IP 的队列必然仍无映射。
- 另：Release（g++）的 `-Weffc++` 比 Debug（clang++）严格，要求
  `ArpEntry` 与三个 map 成员写默认初始化（`{}`），否则 build-release
  编译失败；Debug 的 check_lab5 看不出来。

### 六、A/B 对比实验结果（`tests/arp_cache_bench.cc`）

- 程序：A = `ArpCacheScan`（本实验实现），B = `ArpCacheLru`（有序
  链表 + 迭代器索引），同一二进制、整型模拟时钟、相同 30 秒 TTL；
  计时区只包被测操作，正确性校验（200 秒事件序列逐条比对成员与 MAC）
  在计时区外，A/B 完全一致；每项取 5 次最小值；堆分配用全局
  `operator new` 计数（下表 allocs 均为 0，刷新复用节点）。
- 环境：Release（build-release，g++ -O3）；tick 类负载 20000 次，
  realistic 为 8 个邻居、1 小时、360 万次 tick。
- ctest 已注册 `t_arp_cache_bench`（Debug 小规模，不进 check_lab5）。

#### tick 空转（无过期，ns/tick）

| n | A | B |
|---|---|---|
| 1 | 1.2 | 1.2 |
| 8 | 3.3 | 1.2 |
| 64 | 39.4 | 1.2 |
| 512 | 375.6 | 1.2 |
| 4096 | 10040.6 | 1.2 |

#### 其它负载

| 负载（Release） | n | A | B |
|---|---|---|---|
| 批量过期，ns/条目 | 64 | 9.6 | 21.2 |
| 批量过期，ns/条目 | 4096 | 8.8 | 18.7 |
| 刷新，ns/次 | 64 | 4.1 | 6.2 |
| 刷新，ns/次 | 4096 | 3.8 | 6.2 |
| realistic，ns/tick | 8 | 25.3 | 25.3 |

- realistic：8 邻居、每 tick 8 次查找、每 10 秒刷新一遍、30 秒 TTL、
  1 小时 360 万 tick；两者都约 25.3 ns/tick，总计约 91 ms/小时。
- 分析：
  - tick 空转 A 随 n 线性增长，且每条目成本从约 0.6 ns 涨到 2.4 ns
    （`unordered_map` 遍历是跨节点随机访问，n 大时每步都 cache miss）；
    B 恒定 1.2 ns。
  - 批量过期反而是 A 快一倍：B 每个条目要 list 摘除 + 哈希删除，常数
    更大；B 的优势是"空转 tick 与 n 无关"，不是"一次删很多"。
  - 刷新/学习 A 快约 50%：B 每次要 splice 并多一次间接寻址。
  - 真实负载下差异小于 1%：25.3 ns/tick 里 8 次查找占大头，缓存扫描
    在 n=8 时只有 3.3 ns；一小时 91 ms 相对毫秒级的 ARP 往返可忽略。
- 结论：保持 A 作为提交实现（简单、少一条顺序不变量、刷新更快）。
  B 只在"n 很大且 tick 极频繁"时才划算：n=4096 时 A 每 tick 10 µs，
  每秒 1000 次 tick 就占 1% CPU；若 Lab 6 某个路由器接口真有上千
  邻居，可只替换私有成员切到 B，公共接口不变。当前实验规模
  （n=1~8）落在交叉点的另一侧，A 更合适。

Program Structure and Design of the NetworkInterface:
[]

Implementation Challenges:
[]

Remaining Bugs:
[]

- Optional: I had unexpected difficulty with: [describe]

- Optional: I think you could make this lab better by: [describe]

- Optional: I was surprised by: [describe]

- Optional: I'm not sure about: [describe]
