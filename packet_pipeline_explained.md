# 数据包传输流水线详解

## 用户疑问

```yaml
- src: 0           # 节点0
  dst: 5           # 节点5
  cycle: 0         # 在第0个周期注入
  size: 64         # 数据大小64字节
```

**问题**：
1. size如何转换成实际传输？
2. 是否分成多个packet？还是多个flit？
3. 论文中的流水线阶段（IQ, RC, VA, SA, ST, LT）如何建模？

---

## 1. Size → Flit数量转换

### 关键代码
**位置**: `src/traffic_manager.cpp:365`

```cpp
int packet_length = ceil((double)nt_get_packet_size(trace_packet) / 16);  // 16B Bus width
Packet* packet = new Packet(network->id2nodeid(src), network->id2nodeid(dest), packet_length);
```

### 转换公式

```
flit_count = ⌈size(字节) / 16⌉
```

**硬编码参数**：总线宽度 = 16字节/flit

### 示例

| Size (字节) | Flit数量 | 计算过程 |
|------------|---------|---------|
| 64         | 4       | ⌈64/16⌉ = 4 |
| 128        | 8       | ⌈128/16⌉ = 8 |
| 256        | 16      | ⌈256/16⌉ = 16 |
| 100        | 7       | ⌈100/16⌉ = 7 |

**重要**：
- **1个任务 = 1个Packet**
- **1个Packet = 多个Flit**
- Flit是网络中传输的最小单位

---

## 2. Packet结构与Flit追踪

### Packet类定义
**位置**: `src/packet.h:4-38`

```cpp
class Packet {
 public:
  Packet(NodeID src, NodeID dst, int length);  // length = flit数量

  NodeID source_;
  NodeID destination_;

  // 核心：每个flit的位置追踪
  std::vector<VCInfo> flit_trace_;  // 长度 = length

  int length_;           // flit总数
  int trans_timer_;      // 总传输时间
  int process_timer_;    // 源节点处理时间
  int SA_timer_;         // Switch Allocation时间
  int link_timer_;       // 链路传输时间
  int wait_timer_;       // 等待时间

  int internal_hops_;    // 芯粒内跳数
  int serial_hops_;      // 串行D2D跳数
  int parallel_hops_;    // 并行D2D跳数

  bool finished_;        // 是否到达目的地
  bool switch_allocated_; // 是否完成交换分配
};
```

### Flit追踪机制

**初始化**（`src/packet.cpp:8-11`）：
```cpp
flit_trace_.reserve(length_);
for (int i = 0; i < length_; i++) {
    flit_trace_.push_back(VCInfo(nullptr, 0, source_));  // 所有flit初始在源节点
}
```

**传输过程**：
- `flit_trace_[0]` = 头flit（Head Flit）
- `flit_trace_[length_-1]` = 尾flit（Tail Flit）
- 每个flit独立记录当前所在的节点、buffer、VC

---

## 3. NoC流水线阶段建模

### 论文中的流水线阶段

```
IQ  → RC  → VA  → SA  → ST  → LT
↓     ↓     ↓     ↓     ↓     ↓
输入  路由  虚拟  交换  交换  链路
队列  计算  通道  分配  穿越  穿越
           分配
```

### 模拟器实现映射

| 阶段 | 全称 | 模拟器函数 | 代码位置 | 说明 |
|------|------|-----------|---------|------|
| **IQ** | Input Queue | Buffer缓冲机制 | `src/buffer.h` | 通过buffer队列实现 |
| **RC** | Route Computation | `routing()` | `system.cpp:72-76` | 调用XY/自适应路由算法 |
| **VA** | VC Allocation | `vc_allocate()` | `system.cpp:78-99` | 分配虚拟通道 |
| **SA** | Switch Allocation | `switch_allocate()` | `system.cpp:101-115` | 分配交换机端口 |
| **ST** | Switch Traversal | `link_timer_` | `system.cpp:156` | 通过延迟计数器模拟 |
| **LT** | Link Traversal | `channel_.latency` | `config.h:34-36` | 链路延迟参数 |

---

## 4. 流水线详细实现

### 4.1 路由配置模式

**配置参数**：`router_stages`
- **OneStage**: RC、VA、SA在同一周期并行尝试
- **TwoStage**: RC在第一阶段，VA和SA并行在第二阶段
- **ThreeStage**: RC、VA、SA严格串行

**位置**: `src/system.cpp:47-70`

```cpp
void System::Threestage(Packet& p) {
  if (p.candidate_channels_.empty())         // 阶段1: 路由计算
    routing(p);
  else if (p.next_vc_.buffer == nullptr)     // 阶段2: VC分配
    vc_allocate(p);
  else if (!p.switch_allocated_)             // 阶段3: 交换分配
    switch_allocate(p);
}
```

### 4.2 路由计算（RC阶段）

**位置**: `src/system.cpp:72-76`

```cpp
void System::routing(Packet& p) const {
  assert(p.candidate_channels_.empty());
  routing_algorithm(p);  // 调用具体算法（XY路由等）
  assert(!p.candidate_channels_.empty());  // 计算出候选通道列表
}
```

**输出**：`p.candidate_channels_` - 候选输出端口列表

**XY路由示例**（`src/topologies/multiple_chip_mesh.cpp:104-131`）：
```cpp
// 优先X方向路由
if (cur_x < dst_x) {
  p.candidate_channels_.push_back(east_port);  // 向东
} else if (cur_x > dst_x) {
  p.candidate_channels_.push_back(west_port);  // 向西
} else if (cur_y < dst_y) {  // X到达后路由Y
  p.candidate_channels_.push_back(north_port); // 向北
} else if (cur_y > dst_y) {
  p.candidate_channels_.push_back(south_port); // 向南
}
```

### 4.3 虚拟通道分配（VA阶段）

**位置**: `src/system.cpp:78-99`

```cpp
void System::vc_allocate(Packet& p) const {
  VCInfo current_vc = p.head_trace();

  // 只有队列头部的数据包可以请求VC
  if (current_vc.head_packet() == &p) {
    for (auto& vc : p.candidate_channels_) {
      // 优先分配空闲VC
      if (vc.buffer->is_empty(vc.vcb)) {
        if (vc.buffer->allocate_buffer(vc.vcb, p.length_)) {
          p.next_vc_ = vc;  // 分配成功
          return;
        }
      }
    }

    // 尝试分配非空但有空间的VC
    for (auto& vc : p.candidate_channels_) {
      if (vc.buffer->allocate_buffer(vc.vcb, p.length_)) {
        p.next_vc_ = vc;
        return;
      }
    }
  }
}
```

**关键机制**：
- **Packet Switching**：整个数据包一次性预留buffer空间
- **空间检查**：`allocate_buffer(vcb, p.length_)` 检查是否有`length_`个flit的空间
- **Head-of-Line优先**：只有队头数据包可以竞争VC

### 4.4 交换分配（SA阶段）

**位置**: `src/system.cpp:101-115`

```cpp
void System::switch_allocate(Packet& p) {
  VCInfo current_vc = p.head_trace();

  if (current_vc.buffer == nullptr) {  // 源节点
    if (p.next_vc_.buffer->allocate_in_link(p)) {
      p.switch_allocated_ = true;  // 分配成功
    }
  } else if (current_vc.head_packet() == &p) {  // 中间节点
    if (current_vc.buffer->allocate_sw_link()) {      // 1. 分配交换机输出端口
      if (p.next_vc_.buffer->allocate_in_link(p)) {  // 2. 分配下一跳输入链路
        p.switch_allocated_ = true;
      } else {
        current_vc.buffer->release_sw_link();         // 失败则释放
      }
    }
  }
}
```

**关键机制**：
- **两级仲裁**：
  1. 交换机输出端口仲裁（`allocate_sw_link()`）
  2. 下一跳输入链路仲裁（`allocate_in_link()`）
- **原子性**：两个都成功才算分配成功

### 4.5 链路传输（ST + LT阶段）

**位置**: `src/system.cpp:153-209`

```cpp
if (p.switch_allocated_) {
  temp1 = p.next_vc_;
  p.link_timer_ = p.next_vc_.buffer->channel_.latency;  // 设置链路延迟

  // 跳数统计
  if (temp1.buffer->channel_ == on_chip_channel)
    p.internal_hops_++;
  else if (temp1.buffer->channel_ == off_chip_serial_channel)
    p.serial_hops_++;

  // 头flit移动
  p.flit_trace_[0] = temp1;

  // 尾部flit根据链路宽度逐步跟随
  int k = temp1.buffer->channel_.width;  // 链路宽度
  // 每周期传输k个flit
  for (int i = 0; i < p.length_ && i < k; i++) {
    p.flit_trace_[i] = temp1;
  }
}

// 链路延迟倒计时
if (p.link_timer_ > 0) {
  p.link_timer_--;
}
```

**链路参数**（`src/config.h:34-36`）：

```cpp
const Channel on_chip_channel(1, 1);            // width=1 flit/cycle, latency=1 cycle
const Channel off_chip_serial_channel(2, 4);    // width=2 flit/cycle, latency=4 cycles
const Channel off_chip_parallel_channel(1, 2);  // width=1 flit/cycle, latency=2 cycles
```

**传输机制**：
- **Width=1**: 每周期传输1个flit（串行化）
- **Width=2**: 每周期传输2个flit（更高带宽）
- **Latency**: 头flit从源到目的buffer的飞行时间

---

## 5. 完整传输示例

### 场景：节点0 → 节点5（芯粒内，2跳）

**配置**：
- size = 64字节 → 4个flit
- 路径：节点0 → 节点1 → 节点5
- 链路：on_chip_channel (width=1, latency=1)

### 周期级时间线

```
Cycle 0: 源节点处理
  - process_timer_ = 1（可配置）
  - 所有flit在节点0

Cycle 1: 第一跳 - 路由计算（RC）
  - routing() 计算出候选端口：East (节点0→1)
  - candidate_channels_ = [East]

Cycle 2: 第一跳 - VC分配（VA）
  - vc_allocate() 在East端口分配VC
  - next_vc_ = East.VC0

Cycle 3: 第一跳 - 交换分配（SA）
  - switch_allocate() 分配交换机和链路
  - switch_allocated_ = true
  - link_timer_ = 1（链路延迟）
  - internal_hops_++ → 1

Cycle 4: 第一跳 - 链路传输（LT）
  - link_timer_-- → 0
  - flit[0]到达节点1的buffer（头flit）
  - flit[1,2,3]还在节点0（width=1，每周期1个flit）

Cycle 5: 第一跳 - 尾部flit传输
  - flit[1]移动到节点1
  - flit[2,3]还在节点0

Cycle 6: 第一跳 - 尾部flit传输
  - flit[2]移动到节点1
  - flit[3]还在节点0

Cycle 7: 第一跳 - 尾部flit传输
  - flit[3]移动到节点1
  - 释放节点0的资源

Cycle 8: 第二跳 - 路由计算（RC）
  - routing() 计算出候选端口：South (节点1→5)
  - candidate_channels_ = [South]

Cycle 9: 第二跳 - VC分配（VA）
  - vc_allocate() 在South端口分配VC
  - next_vc_ = South.VC0

Cycle 10: 第二跳 - 交换分配（SA）
  - switch_allocate() 成功
  - switch_allocated_ = true
  - link_timer_ = 1
  - internal_hops_++ → 2

Cycle 11-14: 第二跳 - 链路传输
  - 4个flit逐个传输到节点5

Cycle 15: 到达目的地
  - 尾flit到达节点5
  - finished_ = true
  - trans_timer_ = 15
```

### 延迟计算

```
总延迟 = 15周期

分解：
  - 源节点处理: 1周期
  - 第一跳流水线: RC(1) + VA(1) + SA(1) + LT(1) = 4周期
  - 第一跳尾部flit: 3周期 (4个flit - 1)
  - 第二跳流水线: RC(1) + VA(1) + SA(1) + LT(1) = 4周期
  - 第二跳尾部flit: 3周期

实际延迟 = 1 + 4 + 3 + 4 + 3 = 15周期 ✓
```

---

## 6. 跨芯粒传输差异

### 芯粒间链路特性

**D2D串行接口**（`off_chip_serial_channel`）：
- **Width = 2**: 每周期2个flit（更高带宽补偿延迟）
- **Latency = 4**: 更长的飞行时间

### 示例：节点0 → 节点2（跨芯粒，2跳）

**路径**：节点0 → 节点1（芯粒内）→ 节点2（跨芯粒）

**第一跳**（节点0→1）：
- on_chip_channel: width=1, latency=1
- internal_hops_++

**第二跳**（节点1→2）：
- off_chip_serial_channel: width=2, latency=4
- serial_hops_++
- 链路延迟更长：4周期 vs 1周期
- 带宽更高：每周期2个flit vs 1个flit

**延迟对比**：
```
芯粒内跳: RC(1) + VA(1) + SA(1) + LT(1) + flit传输(length-1) = 4 + 3 = 7周期
芯粒间跳: RC(1) + VA(1) + SA(1) + LT(4) + flit传输((length-1)/2) = 7 + 1.5 ≈ 9周期

注意：width=2使得尾部flit传输时间减半
```

---

## 7. 关键代码位置总结

| 功能 | 文件 | 行号 | 说明 |
|------|------|------|------|
| **Size转Flit** | `traffic_manager.cpp` | 365 | `ceil(size/16)` |
| **Packet构造** | `packet.cpp` | 3-26 | 初始化flit_trace_ |
| **流水线控制** | `system.cpp` | 117-223 | update()主循环 |
| **路由计算** | `system.cpp` | 72-76 | routing() |
| **VC分配** | `system.cpp` | 78-99 | vc_allocate() |
| **交换分配** | `system.cpp` | 101-115 | switch_allocate() |
| **链路传输** | `system.cpp` | 153-209 | 基于link_timer_ |
| **跳数统计** | `system.cpp` | 160-167 | 基于channel类型 |
| **XY路由** | `multiple_chip_mesh.cpp` | 104-131 | 先X后Y |
| **链路参数** | `config.h` | 34-36 | Channel定义 |

---

## 8. 常见问题

### Q1: 一个任务会分成多个Packet吗？
**答**：不会。1个任务 = 1个Packet，但1个Packet = 多个Flit。

### Q2: Flit和Packet的区别？
**答**：
- **Packet（数据包）**：应用层概念，对应一个完整的通信任务
- **Flit（流量控制单元）**：网络层概念，网络传输的最小单位
- 关系：1 Packet = ⌈size/16⌉ Flits

### Q3: 为什么size=64只需要4个flit？
**答**：总线宽度16字节/flit，64字节 ÷ 16 = 4个flit

### Q4: 流水线阶段可以重叠吗？
**答**：取决于`router_stages`配置：
- **OneStage**: RC、VA、SA尝试在同一周期并行
- **ThreeStage**: 严格串行，每个阶段至少1周期

### Q5: 如何修改flit大小？
**答**：修改`traffic_manager.cpp:365`的除数（当前硬编码为16）

### Q6: Width=2时如何传输奇数个flit？
**答**：最后一个周期只传输剩余flit。例如5个flit：
- 第1周期：传输2个flit
- 第2周期：传输2个flit
- 第3周期：传输1个flit

### Q7: 延迟统计包含处理时间吗？
**答**：是的。`trans_timer_`包含：
- 源节点处理时间（`process_timer_`）
- 网络传输时间（路由+VC分配+交换分配+链路传输）
- 缓冲等待时间（`wait_timer_`）

---

## 9. 总结

### 传输流程概览

```
用户YAML（size=64）
    ↓
[task_to_netrace.py]
    ↓
Netrace文件（size=64）
    ↓
[traffic_manager.cpp:365]
    ↓
Packet对象（length=4 flits）
    ↓
[system.cpp - 流水线]
    ├─ RC: 路由计算 → candidate_channels_
    ├─ VA: VC分配 → next_vc_
    ├─ SA: 交换分配 → switch_allocated_=true
    └─ LT: 链路传输 → link_timer_倒计时
    ↓
逐跳传输（每跳重复RC→VA→SA→LT）
    ↓
到达目的地（finished_=true）
    ↓
延迟统计（trans_timer_, internal_hops_, serial_hops_）
```

### 关键参数

| 参数 | 值 | 说明 |
|------|---|------|
| **Flit大小** | 16字节 | 硬编码 |
| **芯粒内链路** | width=1, latency=1 | 每周期1 flit |
| **串行D2D** | width=2, latency=4 | 每周期2 flit |
| **Buffer大小** | 20 flit | 配置文件中设置 |
| **VC数量** | 2 | 配置文件中设置 |
| **Router阶段** | ThreeStage | 可配置为1/2/3 |

### 性能影响因素

1. **Flit数量**：size越大，尾部flit传输时间越长
2. **链路宽度**：width越大，尾部flit传输越快
3. **链路延迟**：D2D接口延迟高（4周期 vs 1周期）
4. **缓冲争用**：多个数据包竞争同一buffer导致wait_timer增加
5. **流水线深度**：ThreeStage延迟最高但更真实

### 下一步

- 如需修改flit大小，修改`traffic_manager.cpp:365`
- 如需调整链路参数，修改`config.h:34-36`
- 如需更改流水线模型，调整配置文件的`router_stages`参数
