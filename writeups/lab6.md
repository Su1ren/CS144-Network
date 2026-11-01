Lab 6 Writeup
=============

My name: [your name here]

My SUNet ID: [your sunetid here]

This lab took me about [n] hours to do. I [did/did not] attend the lab session.

一、实验目标
------------

- 在 Lab 5 的 NetworkInterface 之上实现一个 Router 类。路由器包含多个
  网络接口，可以从任意接口收到 Internet 数据报，并按路由表把它从正确的
  出接口转发到正确的下一跳。
- 只实现"跟随路由表"的转发逻辑，不需要实现生成路由表的算法
  （RIP / OSPF / BGP / SDN 控制器）。
- 实现两个方法：
  1. add_route(route_prefix, prefix_length, next_hop, interface_num)
     - 把一条"匹配-动作"规则存进私有路由表数据结构。
     - 匹配：目的地址的最高 prefix_length 位（0~32）与 route_prefix
       的最高 prefix_length 位相同，即该地址落在 route_prefix/
       prefix_length 所表示的网络内。
     - 动作：next_hop 为空表示该网络与路由器直连，下一跳就是数据报
       自身的目的地址；非空时下一跳是下一台路由器的 IP。interface_num
       指定用哪个接口发送，可通过 interface(interface_num) 取得。
  2. route_one_datagram(dgram)
     - 在路由表中找出所有匹配目的地址的路由，取 prefix_length 最大者
       （最长前缀匹配）。
     - 没有匹配路由则丢弃数据报。
     - 将数据报 TTL 减 1；若减之前已是 0，或减之后为 0，则丢弃。
     - 否则调用 interface(n).send_datagram() 从正确接口发往正确下一跳。
- 体会分层设计：路由器只关心 IP 数据报，完全不关心 TCP、ARP、以太网
  帧格式与链路层地址解析，链路层只通过 NetworkInterface 抽象交互。
- 规模与验收：官方预计约 25~30 行代码；用 make check_lab6（2 个测试，
  在 apps/network_simulator 的模拟网络中运行），全量测试为 make check
  （161 个测试）。

二、手册提示的主要挑战（实现前的预期）
----------------------------------------

- 最长前缀匹配的位运算：需要比较两个 32 位 IP 地址的最高 N 位。
  边界 N = 0（0.0.0.0/0 默认路由，任何地址都匹配）和 N = 32（主机路由）
  最容易出错；还要注意 N = 0 时不能构造"全 1 再取反"之类的掩码。
- C++ 未定义行为：把 32 位整数左/右移 32 位是未定义行为，N = 0 或
  N = 32 的特判必须小心。建议用 RelASan 构建
  （cmake -DCMAKE_BUILD_TYPE=RelASan ..）跑一遍，提前发现 UB。
- TTL 的处理顺序：必须先判断原 TTL 是否为 0，再减 1 并判断是否归零，
  顺序写错会导致本应转发的数据报被丢弃（或反之）；丢弃时不需要回送
  ICMP 差错报文。
- next_hop 的 optional 语义：空值代表直连网络，下一跳要用数据报自己的
  目的地址；有值时才用记录中的下一跳地址。二者混用会导致直连网段
  转发失败。
- 地址与整数的互转：Address::ipv4_numeric() 把 Address 转成 32 位整数，
  Address::from_ipv4_numeric() 反向转换，路由表内部用整数比较更自然。
- 路由表数据结构与性能：手册明确允许每个数据报 O(N) 的线性扫描
  （N 为路由表条目数）；应在保证正确性后再考虑前缀树等优化，并做好
  注释与说明。
- 无匹配路由与接口索引等边界情况：需要保证路由表为空、目的地址
  不在任何网络、interface_num 越界等情形下行为正确（越界可直接
  在调试期暴露）。

Program Structure and Design of the Router:
-------------------------------------------

- 分层结构：Router 只负责接口管理和转发策略，具体的最长前缀匹配算法
  抽象为 RoutingTable 接口（insert / lookup），Router 持有一个
  std::unique_ptr<RoutingTable>，默认指向 TrieRoutingTable。切换实现
  只需改这一行，Router 的公开接口没有任何变化。
- 统一的 RouteEntry 表示一条路由（前缀、前缀长度、下一跳、出接口），
  位运算助手集中在 routing_detail 命名空间，所有实现共用同一套语义。
- 三种实现：
  1. VectorRoutingTable（基准）：追加插入 O(1)，查找线性扫描 O(N)；
     相同长度且掩码后相同的前缀视为重复，后插入者生效。
  2. TrieRoutingTable（默认，路径压缩/Patricia 树）：每个节点保存
     key、depth（代表的前缀长度）、是否挂有路由，以及 0/1 两个子节点。
     单分支链被压缩，节点数为 O(N)，查找步数只与分支深度有关。
     查找时沿目的地址的位向下走，校验压缩边"跳过"的位是否匹配，
     并记录沿途最后一个挂有路由的节点——最长匹配可能位于内部节点
     （例如 /8 路由覆盖了通往 /24 的路径）。根节点 depth = 0，
     天然承载 0.0.0.0/0 默认路由。
     插入分四种情形：同前缀覆盖、向下延伸、作为祖先插到当前节点之上、
     在分叉位新建内部节点把两棵子树挂为左右孩子。
  3. HashRoutingTable（对比）：按前缀长度分成 33 个哈希表，用 64 位
     位图记录哪些长度非空；查找从 /32 向 /0 探测，首个命中即最长匹配，
     最多 33 次哈希，与表大小无关。
- 转发流程：lookup 取最长匹配 → 无匹配则丢弃 → TTL <= 1 则丢弃，
  否则 TTL 减 1 → 下一跳为空时用数据报目的地址，否则用路由中的
  next_hop → interface(n).send_datagram()。

性能对比（apps/router_benchmark，1k / 100k 条随机前缀，取多次最小值）：

    实现           结构规模        构建(ms)   查找(ns)   Mlookup/s
    vector         1001 条             0.11      1127         0.89
    patricia-trie  1932 节点           0.20        71        14.1
    bucketed-hash  1001 条             0.12       298         3.35
    vector         100001 条           10.2    208000         0.005
    patricia-trie  178920 节点         40.3       217         4.60
    bucketed-hash  100001 条           13.5       408         2.45

  结论：线性扫描随表项数线性恶化；前缀树查找基本恒定，代价是构建更慢、
  节点内存约为表项数的 1.8 倍；分桶哈希介于两者之间，内存开销较大。
  基准在计时前会先对三种实现做逐查询结果对拍，保证比较的是同一语义。

Implementation Challenges:
--------------------------

- 位运算边界：/0（默认路由）与 /32（主机路由）最容易出错；直接移位
  32 位在 C++ 中是未定义行为。最终用掩码 prefix_mask(len) 统一实现，
  并用 RelASan 构建跑基准确认没有 UB。
- Patricia 树的正确性：三个容易写错的点——压缩边跳过的位必须在查找时
  校验；最长匹配可能落在内部节点，必须记录沿途最后一个带路由的节点；
  插入时"同长度不同前缀"需要新建分叉节点，"更短前缀"需要作为祖先
  插到当前节点之上（原本的子树成为它的一个孩子）。
- 三种实现的语义必须完全一致：重复前缀的覆盖规则、同长度同掩码前缀
  谁生效，都会影响对拍结果。为此向量实现改为追加插入并在查找时用
  "后插入者优先"，与哈希表/前缀树的覆盖语义对齐。
- 基准本身也踩了坑：向量实现最初在 insert 中做 O(N) 去重，使构建退化为
  O(N^2)（10 万条约 7.6 秒），改为追加后降到 10 毫秒级。
- 测试与调试：make check_lab6 只有两个用例，覆盖有限，因此额外写了
  随机路由表 + 随机查询的对拍程序，对每个查询比对三种实现的完整结果，
  再配合 ASan/UBSan 构建验证内存与位运算安全。

Remaining Bugs:
--------------

- 无已知功能缺陷：check_lab6 两个用例通过，三种实现在随机基准中结果
  完全一致，RelASan 构建无报错。
- 已知局限（非缺陷，本 lab 不需要）：前缀树与分桶哈希都不支持删除
  路由（路由表只增不删）；分桶哈希的内存开销明显大于线性表；基准
  使用随机前缀分布，真实路由表的命中模式可能不同。

- Optional: I had unexpected difficulty with: [describe]

- Optional: I think you could make this lab better by: [describe]

- Optional: I was surprised by: [describe]

- Optional: I'm not sure about: [describe]
