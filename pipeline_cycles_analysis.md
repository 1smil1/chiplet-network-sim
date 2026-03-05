# NoC流水线阶段Cycle消耗详解

## 核心问题

1. **哪些阶段跟数据量有关？哪些跟跳数有关？**
2. **每个阶段需要多少个cycle？**
3. **一个跳数 = 一次完整的流水线流程吗？**

---

## 1. 流水线阶段与Cycle消耗分析

### 完整表格

| 阶段 | 全称 | 与数据量关系 | 与跳数关系 | Cycle消耗 | 说明 |
|------|------|------------|-----------|----------|------|
| **IQ** | Input Queue | ❌ 无关 | ✅ 每跳一次 | **0-N cycle**（等待） | 队列等待时间，取决于缓冲争用 |
| **RC** | Route Computation | ❌ 无关 | ✅ 每跳一次 | **1 cycle**（ThreeStage模式） | 只计算一次路由方向 |
| **VA** | VC Allocation | ❌ 无关 | ✅ 每跳一次 | **1 cycle**（ThreeStage模式） | 只分配一次虚拟通道 |
| **SA** | Switch Allocation | ❌ 无关 | ✅ 每跳一次 | **1 cycle**（ThreeStage模式） | 只分配一次交换机端口 |
| **ST** | Switch Traversal | ❌ 无关 | ✅ 每跳一次 | **包含在链路延迟中** | 头flit穿越交换机 |
| **LT** | Link Traversal | ✅ **有关** | ✅ 每跳一次 | **latency + ⌈length/width⌉ - 1** | 头flit飞行时间 + 尾部flit串行化时间 |

### 关键发现

#### ✅ 只有LT阶段与数据量（flit数量）有关！

**公式**：
```
LT阶段总cycle数 = channel.latency + ceil(packet_length / channel.width) - 1
```

**示例**（4个flit，芯粒内链路）：
```cpp
// on_chip_channel: width=1, latency=1
LT_cycles = 1 + ceil(4/1) - 1 = 1 + 4 - 1 = 4 cycles

// 详细过程：
Cycle 1: 头flit飞行（latency=1）
Cycle 2: 头flit到达，第2个flit开始传输（width=1限制）
Cycle 3: 第2个flit到达，第3个flit开始传输
Cycle 4: 第3个flit到达，第4个flit开始传输
Cycle 5: 第4个flit（尾flit）到达
```

---

## 2. 一个跳数 = 一次完整流水线

**您的理解100%正确！**

```
跳数1（Router 0 → Router 1）:
  IQ(等待) → RC(1 cycle) → VA(1 cycle) → SA(1 cycle) → LT(latency + flit串行化)
  ↓
跳数2（Router 1 → Router 2）:
  IQ(等待) → RC(1 cycle) → VA(1 cycle) → SA(1 cycle) → LT(latency + flit串行化)
  ↓
跳数3（Router 2 → 目的地）:
  ...
```

**每一跳都是独立的流水线流程！**

---

## 3. 详细Cycle消耗分析

### 3.1 IQ（Input Queue）- 等待阶段

**代码位置**: `src/system.cpp:144`

```cpp
if (!p.switch_allocated_) p.wait_timer_++;
```

**Cycle消耗**：
- **0 cycle**（理想情况，无缓冲争用）
- **N cycles**（最坏情况，N取决于其他数据包占用）

**与什么有关**：
- ❌ **与数据量无关**：无论数据包多大，等待时间相同
- ✅ **与跳数有关**：每一跳都可能遇到队列等待
- ✅ **与网络负载有关**：负载越高，等待越久

**示例**：
```
场景1：空闲网络
  - 数据包到达buffer，队列为空
  - wait_timer_ = 0
  - 立即进入RC阶段

场景2：拥塞网络
  - 数据包到达buffer，前面有3个数据包排队
  - 每个数据包需要3 cycles完成RC+VA+SA
  - wait_timer_ = 9 cycles
```

### 3.2 RC（Route Computation）- 路由计算

**代码位置**: `src/system.cpp:72-76`

```cpp
void System::routing(Packet& p) const {
  routing_algorithm(p);  // 计算候选端口
}
```

**Cycle消耗**：
- **1 cycle**（ThreeStage模式）
- **0 cycle**（OneStage模式，与VA/SA并行）

**与什么有关**：
- ❌ **与数据量无关**：只看头flit的目的地址
- ✅ **与跳数有关**：每一跳重新计算
- ✅ **与router_stages配置有关**：
  - ThreeStage: RC独占1 cycle
  - TwoStage: RC独占1 cycle
  - OneStage: RC与VA/SA并行，0额外cycle

**XY路由计算示例**：
```cpp
// src/topologies/multiple_chip_mesh.cpp:104-131
// 只需要简单的坐标比较，1 cycle完成

if (cur_x < dst_x) {
  candidate_channels_ = [East];
} else if (cur_x > dst_x) {
  candidate_channels_ = [West];
} else if (cur_y < dst_y) {
  candidate_channels_ = [North];
} else {
  candidate_channels_ = [South];
}
```

**计算量分析**：
- 读取当前坐标：(cur_x, cur_y)
- 读取目标坐标：(dst_x, dst_y)
- 比较和选择端口
- **总计**：几个算术比较操作，1 cycle足够

### 3.3 VA（VC Allocation）- 虚拟通道分配

**代码位置**: `src/system.cpp:78-99`

```cpp
void System::vc_allocate(Packet& p) const {
  for (auto& vc : p.candidate_channels_) {
    if (vc.buffer->is_empty(vc.vcb)) {
      if (vc.buffer->allocate_buffer(vc.vcb, p.length_)) {
        p.next_vc_ = vc;
        return;  // 分配成功
      }
    }
  }
}
```

**Cycle消耗**：
- **1 cycle**（ThreeStage模式）
- **0 cycle**（OneStage模式，与RC/SA并行）

**与什么有关**：
- ❌ **与数据量无关**：只检查buffer是否有足够空间
- ✅ **与跳数有关**：每一跳需要分配新的VC
- ⚠️ **可能失败**：如果所有VC都满，分配失败，需要下一个cycle重试

**分配逻辑**：
```cpp
// 检查buffer空间：allocate_buffer(vcb, p.length_)
// 例如：buffer_size=20, packet_length=4
if (buffer空闲空间 >= 4) {
  分配成功;  // 1 cycle
} else {
  分配失败;  // 继续等待，下一cycle重试
}
```

**多次尝试场景**：
```
Cycle N: VA尝试，VC0满，VC1满 → 失败
Cycle N+1: VA重试，VC0满，VC1有空间 → 成功
```

### 3.4 SA（Switch Allocation）- 交换分配

**代码位置**: `src/system.cpp:101-115`

```cpp
void System::switch_allocate(Packet& p) {
  if (current_vc.buffer->allocate_sw_link()) {      // 1. 交换机输出端口
    if (p.next_vc_.buffer->allocate_in_link(p)) {  // 2. 下一跳输入链路
      p.switch_allocated_ = true;
    }
  }
}
```

**Cycle消耗**：
- **1 cycle**（ThreeStage模式）
- **0 cycle**（OneStage模式，与RC/VA并行）

**与什么有关**：
- ❌ **与数据量无关**：只分配交换机端口和链路
- ✅ **与跳数有关**：每一跳需要重新分配
- ⚠️ **可能失败**：如果端口被其他数据包占用，失败

**两级仲裁**：
```
阶段1: 分配当前交换机的输出端口
  - 多个VC竞争同一个输出端口
  - 使用Round-Robin仲裁

阶段2: 分配下一跳的输入链路
  - 检查下一个buffer的输入链路是否空闲

两个都成功 → switch_allocated_ = true （1 cycle完成）
```

### 3.5 ST（Switch Traversal）- 交换穿越

**说明**：ST阶段在本模拟器中**隐含在link_timer_中**，没有单独建模。

**Cycle消耗**：
- **0 cycle**（包含在链路延迟中）

**理由**：
```cpp
// src/system.cpp:156
p.link_timer_ = p.next_vc_.buffer->channel_.latency;
```

链路延迟（latency）已经包含了：
- 交换穿越时间（ST）
- 链路飞行时间（LT）

### 3.6 LT（Link Traversal）- 链路传输 ⭐️

**这是唯一与数据量有关的阶段！**

**代码位置**: `src/system.cpp:153-209`

```cpp
if (p.switch_allocated_) {
  // 设置链路延迟
  p.link_timer_ = p.next_vc_.buffer->channel_.latency;

  // 头flit移动
  p.flit_trace_[0] = temp1;
}

// 链路延迟倒计时
if (p.link_timer_ > 0) {
  p.link_timer_--;
}

// 尾部flit根据链路宽度逐步移动
int k = temp1.buffer->channel_.width;  // 每周期传输k个flit
// 每cycle传输width个flit
```

**Cycle消耗公式**：

```
LT总cycle数 = channel.latency + ceil(packet_length / channel.width) - 1
```

**分解说明**：
- **channel.latency**：头flit的飞行时间（物理延迟）
- **ceil(packet_length / channel.width) - 1**：尾部flit的串行化时间

**示例1：芯粒内链路**
```cpp
// on_chip_channel: width=1, latency=1
// packet_length = 4 flit

LT_cycles = 1 + ceil(4/1) - 1 = 1 + 4 - 1 = 4 cycles

详细：
Cycle 1: 头flit飞行（link_timer_=1）
Cycle 2: 头flit到达，flit[1]开始传输（width=1）
Cycle 3: flit[1]到达，flit[2]开始传输
Cycle 4: flit[2]到达，flit[3]开始传输
→ 总共4 cycles
```

**示例2：串行D2D链路**
```cpp
// off_chip_serial_channel: width=2, latency=4
// packet_length = 4 flit

LT_cycles = 4 + ceil(4/2) - 1 = 4 + 2 - 1 = 5 cycles

详细：
Cycle 1-4: 头flit飞行（link_timer_=4）
Cycle 5: 头flit到达，flit[1]和flit[2]同时开始传输（width=2）
Cycle 6: flit[1,2]到达，flit[3]开始传输
→ 总共5 cycles
```

**示例3：大数据包（8 flit，芯粒内）**
```cpp
// on_chip_channel: width=1, latency=1
// packet_length = 8 flit

LT_cycles = 1 + ceil(8/1) - 1 = 1 + 8 - 1 = 8 cycles

→ 数据包越大，LT阶段越长！
```

---

## 4. 一跳的完整Cycle计算

### 公式总结

```
单跳总cycle数 = IQ等待 + RC + VA + SA + LT

具体：
- IQ等待：0-N cycles（取决于缓冲争用）
- RC：1 cycle（ThreeStage）
- VA：1 cycle（ThreeStage）
- SA：1 cycle（ThreeStage）
- LT：latency + ceil(length/width) - 1 cycles
```

### 理想情况（无争用，ThreeStage模式）

**芯粒内链路，4个flit**：
```
IQ: 0 cycle
RC: 1 cycle
VA: 1 cycle
SA: 1 cycle
LT: 1 + 4 - 1 = 4 cycles
────────────────
总计：7 cycles/跳
```

**串行D2D链路，4个flit**：
```
IQ: 0 cycle
RC: 1 cycle
VA: 1 cycle
SA: 1 cycle
LT: 4 + 2 - 1 = 5 cycles
────────────────
总计：8 cycles/跳
```

### 拥塞情况（有争用）

**场景**：前面有2个数据包排队
```
IQ: 14 cycles（2个数据包 × 7 cycles/跳）
RC: 1 cycle
VA: 1 cycle
SA: 1 cycle
LT: 4 cycles
────────────────
总计：21 cycles/跳
```

---

## 5. 多跳传输示例

### 任务1：节点0 → 节点5（芯粒内，2跳）

**路径**：节点0 → 节点1 → 节点5
**Packet**：4个flit
**链路**：on_chip_channel (width=1, latency=1)

**第一跳（节点0→1）**：
```
Cycle 0: 源节点处理（process_timer_=1）
Cycle 1: RC计算 → East
Cycle 2: VA分配 → VC0
Cycle 3: SA分配 → 成功，link_timer_=1
Cycle 4: LT开始，头flit飞行
Cycle 5: 头flit到达，flit[1]开始传输
Cycle 6: flit[1]到达，flit[2]开始传输
Cycle 7: flit[2]到达，flit[3]开始传输
────────────────
第一跳完成：8 cycles（包含源节点处理）
```

**第二跳（节点1→5）**：
```
Cycle 8: RC计算 → South
Cycle 9: VA分配 → VC0
Cycle 10: SA分配 → 成功，link_timer_=1
Cycle 11: LT开始，头flit飞行
Cycle 12: 头flit到达，flit[1]开始传输
Cycle 13: flit[1]到达，flit[2]开始传输
Cycle 14: flit[2]到达，flit[3]开始传输
────────────────
第二跳完成：7 cycles
```

**总延迟**：
```
总延迟 = 源处理(1) + 第一跳(7) + 第二跳(7) = 15 cycles
```

### 任务2：节点0 → 节点2（跨芯粒，2跳）

**路径**：节点0 → 节点1（芯粒内）→ 节点2（跨芯粒）
**Packet**：4个flit

**第一跳（节点0→1，芯粒内）**：
```
Cycle 0: 源处理
Cycle 1-3: RC + VA + SA = 3 cycles
Cycle 4-7: LT = 4 cycles
────────────────
第一跳：8 cycles
```

**第二跳（节点1→2，跨芯粒）**：
```
Cycle 8-10: RC + VA + SA = 3 cycles
Cycle 11-15: LT = 5 cycles
  - latency=4: 头flit飞行4 cycles
  - 尾部串行化：ceil(4/2) - 1 = 1 cycle
────────────────
第二跳：8 cycles
```

**总延迟**：
```
总延迟 = 1 + 8 + 8 = 17 cycles
```

---

## 6. 关键代码位置

### 等待计数器
**位置**: `src/system.cpp:144`
```cpp
if (!p.switch_allocated_) p.wait_timer_++;
```

### 流水线模式选择
**位置**: `src/system.cpp:135-143`
```cpp
if (router_stages_ == "OneStage") {
  onestage(p);  // RC/VA/SA并行
} else if (router_stages_ == "TwoStage") {
  twostage(p);  // RC串行，VA/SA并行
} else if (router_stages_ == "ThreeStage") {
  Threestage(p);  // RC/VA/SA完全串行
}
```

### 链路延迟设置
**位置**: `src/system.cpp:156`
```cpp
p.link_timer_ = p.next_vc_.buffer->channel_.latency;
```

### Flit串行化传输
**位置**: `src/system.cpp:177-200`
```cpp
int k = temp1.buffer->channel_.width;  // 链路宽度
// 每周期传输k个flit
while (i < p.length_) {
  if (j < k) {
    p.flit_trace_[i] = temp1;  // flit移动
    j++;
  }
  i++;
}
```

---

## 7. 总结表格

### 与数据量和跳数的关系

| 阶段 | 与数据量（flit数） | 与跳数 | Cycle消耗 |
|------|------------------|--------|----------|
| **IQ** | ❌ 无关 | ✅ 每跳 | 0-N（争用） |
| **RC** | ❌ 无关 | ✅ 每跳 | 1 |
| **VA** | ❌ 无关 | ✅ 每跳 | 1 |
| **SA** | ❌ 无关 | ✅ 每跳 | 1 |
| **LT** | ✅ **线性相关** | ✅ 每跳 | latency + ⌈length/width⌉ - 1 |

### Cycle消耗对比（4个flit）

| 场景 | IQ | RC | VA | SA | LT | 总计 |
|------|----|----|----|----|-------|------|
| 芯粒内，无争用 | 0 | 1 | 1 | 1 | 4 | **7** |
| 芯粒内，有争用 | 10 | 1 | 1 | 1 | 4 | **17** |
| 串行D2D，无争用 | 0 | 1 | 1 | 1 | 5 | **8** |
| 并行D2D，无争用 | 0 | 1 | 1 | 1 | 3 | **6** |

### 数据量影响对比（芯粒内链路）

| Flit数 | IQ+RC+VA+SA | LT | 单跳总计 |
|--------|-------------|-----|---------|
| 1 | 3 | 1 | **4** |
| 4 | 3 | 4 | **7** |
| 8 | 3 | 8 | **11** |
| 16 | 3 | 16 | **19** |

**结论**：数据量翻倍，LT阶段cycle翻倍，总延迟接近翻倍！

---

## 8. 关键结论

### ✅ 您的理解完全正确

1. **一个跳数 = 一次完整的流水线流程**
   - 每一跳都要经历：IQ → RC → VA → SA → LT
   - 每一跳都是在不同的router上执行

2. **只有LT阶段与数据量有关**
   - RC/VA/SA只看packet头部信息
   - LT需要传输所有flit

3. **每个阶段的cycle消耗**
   - RC/VA/SA：各1 cycle（ThreeStage模式）
   - LT：latency + 尾部flit串行化时间
   - IQ：取决于网络拥塞

4. **链路宽度影响带宽**
   - width=1：每周期1个flit（慢）
   - width=2：每周期2个flit（快）

5. **链路延迟影响首包延迟**
   - on_chip: latency=1（近）
   - off_chip_serial: latency=4（远）

### 性能优化方向

1. **减少跳数**：更直接的路由路径
2. **增加链路宽度**：减少flit串行化时间
3. **减少拥塞**：降低IQ等待时间
4. **使用OneStage**：RC/VA/SA并行，减少3 cycles到1 cycle
