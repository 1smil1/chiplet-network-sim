# 芯粒网络模拟器完全指南

**版本**: 1.0
**更新日期**: 2024
**适用场景**: 16节点芯粒网络（2×2芯粒，每芯粒2×2节点）

---

## 📋 目录

- [您问的核心问题](#您问的核心问题)
- [快速导航](#快速导航)
- [核心概念速查](#核心概念速查)
- [详细主题文档](#详细主题文档)
- [完整工作流程](#完整工作流程)
- [常见问题FAQ](#常见问题faq)
- [关键代码索引](#关键代码索引)
- [配置参数参考](#配置参数参考)

---

## 您问的核心问题

在本次对话中，您提出了以下关键问题，所有答案都已整理在本指南中：

### 问题1：数据包传输机制 🔄

**您的问题**：
> "我们的这些sizes的延迟计算代码，size=64是如何传输的？是把size分成几个package？还是一个packet一个packet传输？论文中的IQ、RC、VA、SA、ST、LT等流水线阶段如何建模？"

**答案概要**：
- ✅ 1个任务 = 1个Packet（不是多个package）
- ✅ 1个Packet = 多个Flit（size=64 → 4个flit）
- ✅ 所有流水线阶段都已实现

**详细文档**：[📄 数据包流水线详解](./packet_pipeline_explained.md)

---

### 问题2：Cycle消耗与数据量关系 ⏰

**您的问题**：
> "哪些阶段跟数据量有关？哪些跟跳数有关？每个阶段需要多少个cycle？"

**答案概要**：
- ✅ **只有LT阶段**与数据量（flit数）有关
- ✅ RC、VA、SA都与数据量无关，每个1 cycle
- ✅ IQ等待时间取决于网络拥塞（0-N cycles）

**详细文档**：[📄 流水线Cycle消耗分析](./pipeline_cycles_analysis.md)

---

### 问题3：并行化处理机制 🔀

**您的问题**：
> "假设2个任务同时进行，一个在芯粒1内部，一个在芯粒2内部，是否可以并行？如果有3个任务，其中一个跨芯粒，它是否会考虑其他任务的资源占用？"

**答案概要**：
- ✅ 所有子任务在同一cycle中**并行更新**
- ✅ 不同芯粒/节点的任务**完全并行**
- ✅ 同一节点的任务通过**原子操作（CAS）**自动处理资源竞争
- ✅ 跨芯粒任务**自动考虑**其他任务的资源占用

**详细文档**：[📄 并行模拟机制详解](./parallel_simulation_explained.md)

---

### 问题4：坐标系统与节点映射 🗺️

**您的问题**：
> "模拟器怎么知道节点的位置？芯粒坐标存在哪里？是自定义的还是自动计算的？"

**答案概要**：
- ✅ 芯粒坐标**自动计算**：`chip_x = chip_id % k_chip`
- ✅ 节点坐标**自动计算**：`node_x = node_id % k_node`
- ✅ 全局坐标 = 芯粒坐标 × k_node + 节点坐标

**详细文档**：[📄 坐标系统详解](./coordinate_explanation.md)

---

### 问题5：跳数计算机制 🎯

**您的问题**：
> "什么是芯粒内跳数？什么是芯粒间跳数？节点0→2是如何计算的？是先到节点1（芯粒内+1），再到节点2（芯粒间+1）吗？"

**答案概要**：
- ✅ 您的理解**100%正确**！
- ✅ 跳数基于**Channel类型**，不是节点位置
- ✅ on_chip_channel → internal_hops++
- ✅ off_chip_serial_channel → serial_hops++

**详细文档**：[📄 跳数分析详解](./hop_analysis.md)

---

### 问题6：依赖模式支持 🔗

**您的问题**：
> "我们只知道任务的先后顺序，不知道具体cycle时间，可以吗？能否修改源代码支持依赖模式？"

**答案概要**：
- ✅ Cycle可以简单使用0、1、2、3...顺序
- ✅ 依赖功能已内置，只需修改6处代码即可启用
- ✅ 完整实现指南已提供

**详细文档**：[📄 依赖模式实现指南](./ENABLE_DEPENDENCIES_GUIDE.md)

---

## 快速导航

### 🎯 我想了解...

| 主题 | 文档 | 关键内容 |
|------|------|---------|
| **如何从size生成flit** | [数据包流水线详解](./packet_pipeline_explained.md#1-size--flit数量转换) | `flit_count = ⌈size/16⌉` |
| **每个流水线阶段的功能** | [数据包流水线详解](./packet_pipeline_explained.md#3-noc流水线阶段建模) | IQ、RC、VA、SA、ST、LT详解 |
| **哪些阶段消耗多个cycle** | [Cycle消耗分析](./pipeline_cycles_analysis.md#1-流水线阶段与cycle消耗分析) | 只有LT与数据量有关 |
| **一跳需要多少cycle** | [Cycle消耗分析](./pipeline_cycles_analysis.md#4-一跳的完整cycle计算) | 理想情况：7 cycles（芯粒内） |
| **多个任务如何并行** | [并行模拟详解](./parallel_simulation_explained.md#1-主模拟循环) | 所有任务同时更新 |
| **资源竞争如何处理** | [并行模拟详解](./parallel_simulation_explained.md#32-资源竞争与原子操作) | CAS原子操作 |
| **节点ID如何映射** | [坐标系统详解](./coordinate_explanation.md#4-16节点完整映射表) | 完整映射表 |
| **跳数如何统计** | [跳数分析详解](./hop_analysis.md#关键结论) | 基于Channel类型 |
| **如何启用依赖功能** | [依赖模式指南](./ENABLE_DEPENDENCIES_GUIDE.md#必需的6处修改) | 6处代码修改 |

---

## 核心概念速查

### 1. 数据传输单位层次 📦

```
用户任务 (YAML: size=64字节)
    ↓
1个 Packet (模拟器对象)
    ↓
4个 Flit (网络传输单元)
    ├─ Flit[0] - 头flit (Head)
    ├─ Flit[1] - 中间flit
    ├─ Flit[2] - 中间flit
    └─ Flit[3] - 尾flit (Tail)
```

**转换公式**：`flit_count = ⌈size(字节) / 16⌉`

**关键**：
- 1个任务 = 1个Packet
- Packet不会分成多个package
- Flit是网络中的最小传输单位

---

### 2. 流水线阶段总览 🔄

| 阶段 | 全称 | 与数据量 | 与跳数 | Cycle消耗 |
|------|------|---------|--------|----------|
| **IQ** | Input Queue | ❌ 无关 | ✅ 每跳 | 0-N（争用） |
| **RC** | Route Computation | ❌ 无关 | ✅ 每跳 | 1 |
| **VA** | VC Allocation | ❌ 无关 | ✅ 每跳 | 1 |
| **SA** | Switch Allocation | ❌ 无关 | ✅ 每跳 | 1 |
| **LT** | Link Traversal | ✅ **相关** | ✅ 每跳 | latency + ⌈length/width⌉ - 1 |

**关键发现**：
- ✅ **只有LT与数据量线性相关**
- ✅ **每一跳都要执行完整流水线**
- ✅ **一跳 = 一次完整的RC→VA→SA→LT流程**

---

### 3. 链路类型对比 🔗

| 链路类型 | Width | Latency | 用途 | 4 flit传输时间 |
|---------|-------|---------|------|---------------|
| **芯粒内** | 1 flit/cycle | 1 cycle | 同芯粒内节点间 | 1 + 4 - 1 = **4 cycles** |
| **串行D2D** | 2 flit/cycle | 4 cycle | 芯粒间互连 | 4 + 2 - 1 = **5 cycles** |
| **并行D2D** | 1 flit/cycle | 2 cycle | 芯粒间互连 | 2 + 4 - 1 = **5 cycles** |

**计算公式**：`LT_cycles = latency + ⌈packet_length / width⌉ - 1`

**性能影响**：
- Width越大 → 尾部flit传输越快
- Latency越大 → 头flit飞行时间越长

---

### 4. 并行化核心机制 🔀

```
Cycle N: 所有数据包同时更新
┌─────────────────────────────────────────┐
│ Packet1 (芯粒1): RC → VA → SA → LT     │  ← 并行
│ Packet2 (芯粒2): RC → VA → SA → LT     │  ← 并行
│ Packet3 (芯粒1): RC → VA → SA → LT     │  ← 并行（可能竞争）
└─────────────────────────────────────────┘
```

**资源竞争处理**：

```cpp
// 原子操作（CAS）
bool success = sw_link_used_.compare_exchange_strong(false, true);
// 只有一个线程能成功，其他失败后等待下一cycle
```

**并行性总结**：

| 场景 | 并行性 | 原因 |
|------|--------|------|
| 不同芯粒 | ✅ 完全并行 | 无资源共享 |
| 不同节点 | ✅ 完全并行 | 不同router |
| 同节点不同方向 | ✅ 完全并行 | 不同端口 |
| 同节点同方向 | ⚠️ 顺序化 | 竞争同一端口 |

---

### 5. 16节点拓扑结构 🗺️

```
全局视图（4×4网格）:
     x=0  x=1 | x=2  x=3
y=0:  0    1  |  2    3     ← Chiplet(0,0) | Chiplet(1,0)
y=1:  4    5  |  6    7
     ─────────────────────
y=2:  8    9  | 10   11     ← Chiplet(0,1) | Chiplet(1,1)
y=3: 12   13  | 14   15
```

**坐标计算公式**：
```python
# 全局ID → 芯粒坐标 + 节点坐标
chip_x = (id % 4) // 2
chip_y = (id // 4) // 2
node_x = (id % 4) % 2
node_y = (id // 4) % 2

# 或使用id2nodeid()函数
```

---

### 6. 跳数统计机制 📊

**关键代码**：
```cpp
// src/system.cpp:160-167
if (buffer->channel_ == on_chip_channel)
    p.internal_hops_++;      // 芯粒内跳数
else if (buffer->channel_ == off_chip_serial_channel)
    p.serial_hops_++;        // 芯粒间跳数
```

**判断依据**：
- ❌ 不是直接比较节点位置
- ✅ 检查目标buffer的Channel类型
- ✅ 每次switch_allocated_时统计一次

**示例**：节点0 → 节点2
```
跳1: 0→1 (芯粒0→0) → internal_hops = 1
跳2: 1→2 (芯粒0→1) → serial_hops = 1
```

---

## 详细主题文档

### 📄 1. 数据包流水线详解

**文件**：[packet_pipeline_explained.md](./packet_pipeline_explained.md)

**包含内容**：
- ✅ Size到Flit的转换机制
- ✅ Packet结构与Flit追踪
- ✅ 所有流水线阶段的详细实现
- ✅ 完整传输示例（节点0→5，15 cycles）
- ✅ 芯粒内vs芯粒间传输差异

**适合阅读**：
- 想了解数据包如何在网络中传输
- 想知道IQ、RC、VA、SA、LT各阶段的功能
- 想理解Flit级别的传输过程

**关键章节**：
- [Size转Flit公式](./packet_pipeline_explained.md#1-size--flit数量转换)
- [流水线阶段映射](./packet_pipeline_explained.md#3-noc流水线阶段建模)
- [完整传输示例](./packet_pipeline_explained.md#5-完整传输示例)

---

### 📄 2. 流水线Cycle消耗分析

**文件**：[pipeline_cycles_analysis.md](./pipeline_cycles_analysis.md)

**包含内容**：
- ✅ 每个阶段需要多少个cycle
- ✅ 哪些阶段与数据量有关
- ✅ LT阶段的详细计算公式
- ✅ 多跳传输的完整时间线

**适合阅读**：
- 想知道每个阶段消耗多少cycle
- 想理解为什么LT是唯一与数据量相关的阶段
- 想预测任务的总延迟

**关键章节**：
- [阶段与Cycle消耗总表](./pipeline_cycles_analysis.md#1-流水线阶段与cycle消耗分析)
- [LT阶段详细分析](./pipeline_cycles_analysis.md#36-lt-link-traversal---链路传输-)
- [一跳完整计算](./pipeline_cycles_analysis.md#4-一跳的完整cycle计算)

---

### 📄 3. 并行模拟机制详解

**文件**：[parallel_simulation_explained.md](./parallel_simulation_explained.md)

**包含内容**：
- ✅ 所有子任务如何并行更新
- ✅ 原子操作与CAS机制
- ✅ 资源竞争的详细处理
- ✅ 3个子任务的完整时间线

**适合阅读**：
- 想了解多个任务如何同时模拟
- 想知道资源竞争如何自动处理
- 想理解为什么同节点任务会互相影响

**关键章节**：
- [主模拟循环](./parallel_simulation_explained.md#1-主模拟循环)
- [CAS机制详解](./parallel_simulation_explained.md#33-资源分配compare-and-swap)
- [场景分析](./parallel_simulation_explained.md#4-场景分析)

---

### 📄 4. 坐标系统详解

**文件**：[coordinate_explanation.md](./coordinate_explanation.md)

**包含内容**：
- ✅ 芯粒坐标自动计算公式
- ✅ 节点坐标自动计算公式
- ✅ 16节点完整映射表
- ✅ 全局ID与所有坐标层次的对应关系

**适合阅读**：
- 想了解节点ID如何映射到物理位置
- 想知道芯粒坐标如何存储
- 想查询特定节点的坐标

**关键章节**：
- [坐标计算公式](./coordinate_explanation.md#1-芯粒坐标初始化)
- [16节点映射表](./coordinate_explanation.md#4-16节点完整映射表)
- [可视化布局](./coordinate_explanation.md#可视化布局)

---

### 📄 5. 跳数分析详解

**文件**：[hop_analysis.md](./hop_analysis.md)

**包含内容**：
- ✅ 4个任务的逐跳分析
- ✅ 每一跳的详细路径和跳数计算
- ✅ 实际模拟器输出验证
- ✅ 平均跳数的计算方法

**适合阅读**：
- 想验证特定任务的跳数
- 想了解跳数如何手工计算
- 想理解internal_hops和serial_hops的含义

**关键章节**：
- [任务2详细分析](./hop_analysis.md#任务2-节点0--节点2-)
- [跳数汇总表](./hop_analysis.md#各任务跳数汇总)
- [关键结论](./hop_analysis.md#关键结论)

---

### 📄 6. 依赖模式实现指南

**文件**：[ENABLE_DEPENDENCIES_GUIDE.md](./ENABLE_DEPENDENCIES_GUIDE.md)

**包含内容**：
- ✅ 6处必需代码修改（前后对比）
- ✅ 编译和测试步骤
- ✅ 依赖关系文件格式示例
- ✅ 风险分析和故障排除

**适合阅读**：
- 想启用netrace依赖功能
- 想让任务按依赖关系顺序执行
- 想修改源代码并重新编译

**关键章节**：
- [必需的6处修改](./ENABLE_DEPENDENCIES_GUIDE.md#必需的6处修改)
- [实现步骤](./ENABLE_DEPENDENCIES_GUIDE.md#实现步骤)
- [测试方法](./ENABLE_DEPENDENCIES_GUIDE.md#测试与验证)

---

## 完整工作流程

### 从YAML到延迟统计的完整流程

```
┌─────────────────────────────────────────────────────────────┐
│ 1. 用户创建YAML任务描述                                       │
│    tasks:                                                    │
│      - {src: 0, dst: 5, cycle: 0, size: 64}                 │
└─────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────┐
│ 2. task_to_netrace.py 转换                                   │
│    - 读取YAML                                                │
│    - 计算flit数: 64/16 = 4                                   │
│    - 生成netrace文件                                         │
└─────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────┐
│ 3. ChipletNetworkSim.exe 模拟                                │
│    - 读取netrace文件                                         │
│    - 创建Packet对象 (length=4)                               │
│    - 每个cycle:                                              │
│      ├─ 注入新数据包 (cycle匹配时)                           │
│      ├─ 并行更新所有数据包                                   │
│      │   ├─ RC: 路由计算                                     │
│      │   ├─ VA: VC分配                                       │
│      │   ├─ SA: 交换分配                                     │
│      │   └─ LT: 链路传输                                     │
│      └─ 统计跳数和延迟                                       │
└─────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────┐
│ 4. 输出结果                                                  │
│    - CSV: 注入率、延迟、接收率                               │
│    - 控制台: 平均延迟、内部跳数、串行跳数                     │
│    - 日志: 详细调试信息                                      │
└─────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────┐
│ 5. result_extractor.py 提取（可选）                         │
│    - 解析CSV和日志                                           │
│    - 格式化输出                                              │
│    - 生成报告                                                │
└─────────────────────────────────────────────────────────────┘
```

---

### 单个数据包的生命周期

```
┌──────────────────────────────────────────────────────────────┐
│ Cycle 0: 创建和注入                                           │
│   - TM->genMes() 在cycle=0时创建Packet                       │
│   - Packet(src=0, dst=5, length=4)                          │
│   - 所有4个flit初始在节点0                                    │
└──────────────────────────────────────────────────────────────┘
                            ↓
┌──────────────────────────────────────────────────────────────┐
│ Cycle 0: 源节点处理                                           │
│   - process_timer_-- (1→0)                                   │
└──────────────────────────────────────────────────────────────┘
                            ↓
┌──────────────────────────────────────────────────────────────┐
│ Cycle 1-3: 第一跳流水线（节点0→1）                            │
│   - Cycle 1: RC - 计算路由方向 (East)                        │
│   - Cycle 2: VA - 分配VC (East.VC0)                          │
│   - Cycle 3: SA - 分配交换机 (成功)                          │
│              → switch_allocated_ = true                      │
│              → link_timer_ = 1                               │
│              → internal_hops_++ (=1)                         │
└──────────────────────────────────────────────────────────────┘
                            ↓
┌──────────────────────────────────────────────────────────────┐
│ Cycle 4-7: 第一跳链路传输                                     │
│   - Cycle 4: 头flit飞行 (link_timer_=1)                      │
│              头flit到达节点1                                  │
│   - Cycle 5: flit[1]传输 (width=1)                           │
│   - Cycle 6: flit[2]传输                                     │
│   - Cycle 7: flit[3]传输，释放节点0资源                       │
└──────────────────────────────────────────────────────────────┘
                            ↓
┌──────────────────────────────────────────────────────────────┐
│ Cycle 8-14: 第二跳（节点1→5）                                 │
│   - Cycle 8-10: RC + VA + SA                                │
│   - Cycle 11-14: LT传输                                      │
│              → internal_hops_++ (=2)                         │
└──────────────────────────────────────────────────────────────┘
                            ↓
┌──────────────────────────────────────────────────────────────┐
│ Cycle 15: 到达目的地                                          │
│   - 尾flit到达节点5                                           │
│   - finished_ = true                                         │
│   - 统计:                                                    │
│     * trans_timer_ = 15                                     │
│     * internal_hops_ = 2                                    │
│     * serial_hops_ = 0                                      │
└──────────────────────────────────────────────────────────────┘
```

---

## 常见问题FAQ

### Q1: Size=64字节会分成多少个flit？

**答**：`⌈64/16⌉ = 4个flit`

- 硬编码总线宽度：16字节/flit
- 代码位置：`src/traffic_manager.cpp:365`

---

### Q2: 一个任务会变成多个Packet吗？

**答**：❌ 不会。**1个任务 = 1个Packet = 多个Flit**

- Packet是应用层概念，对应一个完整任务
- Flit是网络层概念，是传输的最小单位

---

### Q3: 为什么只有LT阶段与数据量有关？

**答**：因为RC、VA、SA只看Packet头部信息，不管有多少flit。

**公式**：
```
LT_cycles = channel.latency + ⌈packet_length / channel.width⌉ - 1
            ─────────────────   ──────────────────────────────────
            头flit飞行时间        尾部flit串行化时间（与数据量相关）
```

---

### Q4: 一跳需要多少个cycle？

**答**：理想情况（无争用，ThreeStage，4 flit）：

- **芯粒内**：RC(1) + VA(1) + SA(1) + LT(4) = **7 cycles**
- **串行D2D**：RC(1) + VA(1) + SA(1) + LT(5) = **8 cycles**

**注意**：
- IQ等待时间取决于拥塞（0-N cycles）
- VA/SA失败会重试（+N cycles）

---

### Q5: 多个任务如何并行？

**答**：所有任务在同一cycle中并行更新，通过原子操作处理资源竞争。

```cpp
// 每个cycle
for (auto packet : all_packets) {
  system->update(packet);  // 所有任务并行调用
}
```

**竞争处理**：
```cpp
// CAS保证只有一个成功
if (sw_link_used_.compare_exchange_strong(false, true)) {
  return true;  // 成功
}
return false;   // 失败，下一cycle重试
```

---

### Q6: 跳数是如何统计的？

**答**：基于目标buffer的Channel类型，不是节点位置。

```cpp
if (buffer->channel_ == on_chip_channel)
    internal_hops_++;
else if (buffer->channel_ == off_chip_serial_channel)
    serial_hops_++;
```

**示例**：节点0→2
- 跳1: 0→1 (芯粒0→0) → internal_hops = 1
- 跳2: 1→2 (芯粒0→1) → serial_hops = 1

---

### Q7: 如何减少任务延迟？

**答**：几个方向：

1. **减少跳数**：更短的路由路径
2. **增加链路宽度**：减少flit串行化时间
3. **减少数据量**：更少的flit
4. **增加VC数量**：减少缓冲争用
5. **使用OneStage**：RC/VA/SA并行（3 cycles → 1 cycle）

---

### Q8: 如何验证我的理解？

**答**：运行测试场景并对比结果。

**测试1**：单个任务（节点0→5）
```yaml
tasks:
  - {src: 0, dst: 5, cycle: 0, size: 64}
```
**期望延迟**：≈15 cycles

**测试2**：2个独立任务
```yaml
tasks:
  - {src: 0, dst: 5, cycle: 0, size: 64}  # 芯粒1
  - {src: 8, dst: 13, cycle: 0, size: 64} # 芯粒2
```
**期望**：平均延迟 ≈ 单任务延迟（证明并行）

**测试3**：2个竞争任务
```yaml
tasks:
  - {src: 0, dst: 1, cycle: 0, size: 64}
  - {src: 0, dst: 2, cycle: 0, size: 64}
```
**期望**：平均延迟 > 单任务延迟（证明竞争）

---

### Q9: 如何修改flit大小？

**答**：修改 `src/traffic_manager.cpp:365`

```cpp
// 当前：16字节/flit
int packet_length = ceil((double)nt_get_packet_size(trace_packet) / 16);

// 改为32字节/flit
int packet_length = ceil((double)nt_get_packet_size(trace_packet) / 32);
```

**影响**：
- flit数减少 → LT时间减少
- 但每个flit更大，可能需要调整buffer_size

---

### Q10: Cycle字段必须连续吗？

**答**：❌ 不必须。可以有间隔。

```yaml
tasks:
  - {src: 0, dst: 5, cycle: 0, size: 64}
  - {src: 0, dst: 2, cycle: 10, size: 128}  # 中间空9个cycle
  - {src: 0, dst: 8, cycle: 100, size: 128} # 可以跳很大
```

**作用**：
- Cycle只是注入时间戳
- 模拟器在cycle=N时检查是否有任务要注入

---

## 关键代码索引

### 核心文件概览

| 文件 | 功能 | 关键内容 |
|------|------|---------|
| `src/main.cpp` | 主模拟循环 | 并行更新、线程管理 |
| `src/system.cpp` | 流水线逻辑 | RC、VA、SA、LT实现 |
| `src/packet.h/cpp` | 数据包定义 | Flit追踪、延迟统计 |
| `src/buffer.h/cpp` | 缓冲管理 | 原子操作、资源分配 |
| `src/traffic_manager.cpp` | 流量生成 | Netrace注入、size转flit |
| `src/topologies/multiple_chip_mesh.cpp` | 拓扑和路由 | XY路由、坐标映射 |
| `src/config.h` | 配置定义 | Channel参数 |

---

### 关键代码位置

#### 1. Size → Flit转换
**位置**：`src/traffic_manager.cpp:365`
```cpp
int packet_length = ceil((double)nt_get_packet_size(trace_packet) / 16);
Packet* packet = new Packet(network->id2nodeid(src),
                            network->id2nodeid(dest),
                            packet_length);
```

---

#### 2. Packet初始化
**位置**：`src/packet.cpp:3-26`
```cpp
Packet::Packet(NodeID source, NodeID destination, int length) {
  length_ = length;
  flit_trace_.reserve(length_);
  for (int i = 0; i < length_; i++) {
    flit_trace_.push_back(VCInfo(nullptr, 0, source_));
  }
  internal_hops_ = 0;
  serial_hops_ = 0;
}
```

---

#### 3. 流水线执行
**位置**：`src/system.cpp:63-70`
```cpp
void System::Threestage(Packet& p) {
  if (p.candidate_channels_.empty())         // RC
    routing(p);
  else if (p.next_vc_.buffer == nullptr)     // VA
    vc_allocate(p);
  else if (!p.switch_allocated_)             // SA
    switch_allocate(p);
}
```

---

#### 4. 路由计算（XY）
**位置**：`src/topologies/multiple_chip_mesh.cpp:104-131`
```cpp
void MultiChipMesh::routing_algorithm(Packet& p) const {
  // 计算全局坐标
  int cur_x = chip_x * k_node_ + node_x;
  int dst_x = dst_chip_x * k_node_ + dst_node_x;

  // XY路由：先X后Y
  if (cur_x < dst_x) {
    p.candidate_channels_.push_back(east_port);
  } else if (cur_x > dst_x) {
    p.candidate_channels_.push_back(west_port);
  } else if (cur_y < dst_y) {
    p.candidate_channels_.push_back(north_port);
  } else if (cur_y > dst_y) {
    p.candidate_channels_.push_back(south_port);
  }
}
```

---

#### 5. VC分配
**位置**：`src/system.cpp:78-99`
```cpp
void System::vc_allocate(Packet& p) const {
  for (auto& vc : p.candidate_channels_) {
    if (vc.buffer->is_empty(vc.vcb)) {
      if (vc.buffer->allocate_buffer(vc.vcb, p.length_)) {
        p.next_vc_ = vc;
        return;
      }
    }
  }
}
```

---

#### 6. 交换分配
**位置**：`src/system.cpp:101-115`
```cpp
void System::switch_allocate(Packet& p) {
  if (current_vc.buffer->allocate_sw_link()) {
    if (p.next_vc_.buffer->allocate_in_link(p)) {
      p.switch_allocated_ = true;
    }
  }
}
```

---

#### 7. 链路传输
**位置**：`src/system.cpp:153-209`
```cpp
if (p.switch_allocated_) {
  p.link_timer_ = p.next_vc_.buffer->channel_.latency;

  // 跳数统计
  if (buffer->channel_ == on_chip_channel)
    p.internal_hops_++;
  else if (buffer->channel_ == off_chip_serial_channel)
    p.serial_hops_++;

  // Flit移动
  int k = channel.width;  // 每周期传输k个flit
  for (int i = 0; i < k && i < p.length_; i++) {
    p.flit_trace_[i] = next_vc;
  }
}
```

---

#### 8. 资源分配（CAS）
**位置**：`src/buffer.cpp:94-100`
```cpp
bool Buffer::allocate_sw_link() {
  bool link_used_state = sw_link_used_.load();
  if (sw_link_used_.compare_exchange_strong(link_used_state, true)) {
    return true;  // 只有一个线程成功
  }
  return false;
}
```

---

#### 9. 并行更新
**位置**：`src/main.cpp:27-43`
```cpp
static void update_packets(std::vector<Packet*>& packets, System* system) {
  uint64_t i = pkt_i.load();
  while (i < packets.size()) {
    if (pkt_i.compare_exchange_strong(i, i + issue_width)) {
      for (uint64_t j = i; j < max_i; j++) {
        system->update(*packets[j]);  // 更新每个数据包
      }
    }
  }
}
```

---

#### 10. 坐标计算
**位置**：`src/topologies/multiple_chip_mesh.cpp:12-17`
```cpp
// 芯粒坐标
get_chip(chip_id)->chip_coordinate_[0] = chip_id % k_chip_;
get_chip(chip_id)->chip_coordinate_[1] = chip_id / k_chip_;

// 节点坐标 (src/topologies/chip_mesh.cpp:22-26)
x_ = id.node_id % k_node_;
y_ = id.node_id / k_node_;
```

---

## 配置参数参考

### 16节点配置文件示例

**文件**：`input/multiple_chip_mesh_2x2.ini`

```ini
[Network]
topology = MultiChipMesh
routing_algorithm = XY          # XY路由算法
k_node = 2                      # 每芯粒2×2节点
k_chip = 2                      # 2×2芯粒阵列
buffer_size = 20                # 每VC 20个flit空间
vc_number = 2                   # 2个虚拟通道
d2d_IF = off_chip_serial        # 串行D2D接口

[Workload]
traffic = netrace               # 使用netrace文件

[Simulation]
injection_increment = 0.05      # 注入率增量（非netrace模式）

[Files]
netrace_file = ./input/netrace/16node_scenario.tra.bz2
output_file = ./output/output_16node.csv
log_file = ./output/log_16node.txt
```

---

### 关键参数说明

| 参数 | 取值 | 影响 | 推荐值 |
|------|------|------|--------|
| **k_node** | 2 | 每芯粒节点数 = k_node² | 2 (4节点/芯粒) |
| **k_chip** | 2 | 芯粒数 = k_chip² | 2 (4芯粒) |
| **buffer_size** | 20 | VC缓冲容量（flit） | 20-40 |
| **vc_number** | 2 | 虚拟通道数 | 2-4 |
| **d2d_IF** | off_chip_serial | D2D接口类型 | off_chip_serial |
| **router_stages** | ThreeStage | 流水线深度 | ThreeStage（真实） |

---

### Channel参数（硬编码）

**位置**：`src/config.h:34-36`

```cpp
const Channel on_chip_channel(1, 1);            // 芯粒内
const Channel off_chip_serial_channel(2, 4);    // 串行D2D
const Channel off_chip_parallel_channel(1, 2);  // 并行D2D
```

**参数含义**：
- **width**: 每周期传输的flit数
- **latency**: 头flit的飞行时间（cycles）

---

## 快速命令参考

### 编译
```bash
make clean
make
```

### 运行单个配置
```bash
./ChipletNetworkSim.exe input/multiple_chip_mesh_2x2.ini
```

### 生成Netrace
```bash
python tools/task_to_netrace.py examples/simple_16node.yaml -o input/netrace/test.tra.bz2
```

### 提取结果
```bash
python tools/result_extractor.py --csv output/output_16node.csv --format text
```

### 一键运行（Windows）
```bash
run_simulation.bat examples/simple_16node.yaml
```

---

## 学习路径建议

### 🎯 初学者路径

1. **先理解数据传输**
   - 阅读：[数据包流水线详解](./packet_pipeline_explained.md)
   - 重点：Size→Flit转换、流水线阶段

2. **再理解并行机制**
   - 阅读：[并行模拟详解](./parallel_simulation_explained.md)
   - 重点：多任务如何并行、资源竞争

3. **然后理解性能**
   - 阅读：[Cycle消耗分析](./pipeline_cycles_analysis.md)
   - 重点：哪些影响延迟、如何优化

4. **最后查阅参考**
   - 坐标系统、跳数分析作为查询手册

---

### 🚀 进阶用户路径

1. **优化性能**
   - 调整VC数量、buffer大小
   - 实验不同router_stages配置
   - 分析瓶颈（IQ等待 vs LT传输）

2. **启用高级功能**
   - 阅读：[依赖模式指南](./ENABLE_DEPENDENCIES_GUIDE.md)
   - 修改源代码，支持任务依赖

3. **扩展拓扑**
   - 修改k_node、k_chip参数
   - 适配更大规模网络

---

### 🔬 研究者路径

1. **深入源码**
   - 对照本指南的代码位置
   - 理解XY路由、VC分配、SA仲裁细节

2. **实现新算法**
   - 基于现有框架实现自适应路由
   - 添加新的拥塞控制机制

3. **扩展功能**
   - 支持更多拓扑（Dragonfly等）
   - 实现功耗模型

---

## 术语表

| 术语 | 英文 | 含义 |
|------|------|------|
| **Packet** | Packet | 数据包，对应一个完整任务 |
| **Flit** | Flow Control Unit | 流量控制单元，网络传输最小单位 |
| **VC** | Virtual Channel | 虚拟通道，共享物理链路 |
| **IQ** | Input Queue | 输入队列，缓冲等待阶段 |
| **RC** | Route Computation | 路由计算 |
| **VA** | VC Allocation | 虚拟通道分配 |
| **SA** | Switch Allocation | 交换分配 |
| **ST** | Switch Traversal | 交换穿越 |
| **LT** | Link Traversal | 链路穿越 |
| **CAS** | Compare-And-Swap | 比较并交换，原子操作 |
| **D2D** | Die-to-Die | 芯粒间互连 |
| **NoC** | Network-on-Chip | 片上网络 |

---

## 版本历史

### v1.0 (2024)
- ✅ 完整的16节点场景支持
- ✅ YAML工作流
- ✅ 并行模拟机制
- ✅ 完整文档体系

---

## 相关资源

### 📚 外部文档

- **原始论文**：（如有请添加引用）
- **GitHub仓库**：（如有请添加链接）

### 🛠️ 工具文件

- `tools/task_to_netrace.py` - YAML转Netrace
- `tools/create_netrace.py` - 底层Netrace生成
- `tools/result_extractor.py` - 结果提取（待实现）

### 📝 配置文件

- `input/multiple_chip_mesh_2x2.ini` - 16节点配置
- `examples/simple_16node.yaml` - 简单示例
- `examples/sequential_16node.yaml` - 顺序注入示例

---

## 贡献指南

如果您发现文档有误或需要补充：

1. 检查相关主题文档
2. 确认代码位置是否正确
3. 提出问题或建议
4. 我们会及时更新文档

---

## 致谢

感谢您使用本指南！希望这些文档能帮助您：

- ✅ 理解芯粒网络模拟器的工作原理
- ✅ 快速定位问题和答案
- ✅ 高效使用工具链
- ✅ 进行深入研究和开发

**祝您使用愉快！** 🎉

---

**最后更新**: 2024
**维护者**: Claude Code Assistant
**反馈**: 如有问题请查阅各主题详细文档
