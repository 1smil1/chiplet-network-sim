# 并行化模拟机制详解

## 用户问题

```
场景1：2个子任务同时进行
  - 子任务1：芯粒1内部（节点A→B）
  - 子任务2：芯粒2内部（节点C→D）
  → 是否可以并行处理？

场景2：3个子任务同时进行
  - 子任务1：芯粒1内部（节点A→B）
  - 子任务2：芯粒2内部（节点C→D）
  - 子任务3：芯粒1→芯粒2（节点E→F）
  → 是否都并行处理？
  → 子任务3是否会考虑子任务1和2的资源占用？
```

---

## 核心回答

### ✅ 是的！模拟器完全支持并行模拟！

**关键机制**：
1. **所有子任务（数据包）在同一个cycle中并行更新**
2. **使用原子操作（Atomic）处理资源竞争**
3. **子任务3会自动考虑子任务1和2的资源占用**
4. **通过Compare-And-Swap（CAS）确保资源分配的互斥性**

---

## 1. 主模拟循环

### 代码位置：`src/main.cpp:136-142`

```cpp
for (uint64_t i = 0; i < num_cycles; i++) {
  TM->genMes(all_packets, i);        // 1. 注入新数据包
  run_one_cycle(all_packets, network); // 2. 更新所有数据包
}
```

### 关键数据结构

```cpp
std::vector<Packet*> all_packets;  // 所有在网络中传输的数据包
```

**示例**：假设当前网络中有3个子任务
```cpp
all_packets = [
  Packet1(芯粒1: 节点0→1),   // 子任务1
  Packet2(芯粒2: 节点8→9),   // 子任务2
  Packet3(芯粒1→2: 节点0→2)  // 子任务3
];
```

---

## 2. 一个Cycle的执行流程

### 函数：`run_one_cycle()` (`src/main.cpp:60-104`)

```cpp
static void run_one_cycle(std::vector<Packet*>& vec_pkts, System* system) {
  // ========== 阶段1：清理阶段 ==========
  for (auto i = 0; i < vecsize; ++i) {
    Packet*& pkt = vec_pkts[i];

    // 释放链路资源
    if (pkt->releaselink_ == true) {
      pkt->tail_trace().buffer->release_in_link(*pkt);
      pkt->leaving_vc_.buffer->release_sw_link();
      pkt->releaselink_ = false;
    }

    // 删除已完成的数据包
    if (pkt->finished_) {
      delete pkt;
    }
  }

  // ========== 阶段2：并行更新所有数据包 ==========
  pkt_i.store(0);
  if (vec_pkts.size() < param->threads * 1 || param->threads < 2) {
    // 单线程模式
    update_packets(vec_pkts, system);
  } else {
    // 多线程模式：启动所有工作线程
    for (int i = 0; i < param->threads; ++i) {
      worker_launch[i] = true;
    }
    cv.notify_all();  // 唤醒所有线程
    // 等待所有线程完成
  }
}
```

### 关键：阶段2的并行更新

```cpp
static void update_packets(std::vector<Packet*>& packets, System* system) {
  uint64_t i = pkt_i.load();  // 原子加载下一个数据包索引
  while (i < vec_size) {
    // 使用CAS原子获取一批数据包
    if (pkt_i.compare_exchange_strong(i, i + issue_width)) {
      // 更新这批数据包
      do {
        system->update(*packets[i]);  // 🔥 核心：更新每个数据包
      } while (++i < max_i);
    }
  }
}
```

---

## 3. 并行化机制详解

### 3.1 Cycle级并行

```
Cycle N:
  ┌────────────────────────────────────────────────┐
  │  所有数据包同时尝试流水线操作                    │
  ├────────────────────────────────────────────────┤
  │  Packet1: IQ → RC → VA → SA → LT              │ ← 同时执行
  │  Packet2: IQ → RC → VA → SA → LT              │ ← 同时执行
  │  Packet3: IQ → RC → VA → SA → LT              │ ← 同时执行
  └────────────────────────────────────────────────┘
```

**每个数据包调用一次`system->update()`**：
```cpp
// src/system.cpp:117-223
void System::update(Packet& p) {
  p.trans_timer_++;  // 延迟+1

  // 处理源节点的processing时间
  if (p.process_timer_ > 0) {
    p.process_timer_--;
    return;
  }

  // 链路传输倒计时
  if (p.link_timer_ > 0) {
    p.link_timer_--;
    return;
  }

  // 如果到达buffer，执行流水线
  if (p.link_timer_ == 0) {
    if (router_stages_ == "ThreeStage") {
      Threestage(p);  // RC → VA → SA
    }
  }

  // 如果switch_allocated_，开始链路传输
  if (p.switch_allocated_) {
    p.link_timer_ = channel.latency;
    // ... flit移动逻辑
  }
}
```

### 3.2 资源竞争与原子操作

**关键数据结构** (`src/buffer.h:58-68`)：

```cpp
class Buffer {
 private:
  // 使用原子变量保证线程安全
  std::atomic_bool in_link_used_;      // 输入链路状态
  std::atomic_bool sw_link_used_;      // 交换机链路状态
  std::atomic_int* vc_buffer_;         // 每个VC的空闲buffer空间
  std::atomic<Packet*>* vc_head_packet; // 每个VC的队头数据包
};
```

### 3.3 资源分配：Compare-And-Swap（CAS）

#### 场景：多个数据包竞争同一个交换机端口

**代码** (`src/buffer.cpp:94-100`)：

```cpp
bool Buffer::allocate_sw_link() {
  bool link_used_state = sw_link_used_.load();  // 读取当前状态

  if (link_used_state)
    return false;  // 已被占用，分配失败
  else if (sw_link_used_.compare_exchange_strong(link_used_state, true)) {
    // CAS成功：原子地将false变为true
    return true;   // 分配成功
  }
  return false;    // CAS失败（其他线程抢先），分配失败
}
```

**工作原理**：

```
初始状态：sw_link_used_ = false

同一时刻，3个数据包同时尝试分配：
┌─────────────────────────────────────────────────────────┐
│ Thread 1 (Packet1): compare_exchange(false, true)      │
│ Thread 2 (Packet2): compare_exchange(false, true)      │
│ Thread 3 (Packet3): compare_exchange(false, true)      │
└─────────────────────────────────────────────────────────┘
              ↓
只有一个线程的CAS会成功！
              ↓
┌─────────────────────────────────────────────────────────┐
│ Thread 1: CAS成功 ✅ → 分配成功，sw_link_used_ = true   │
│ Thread 2: CAS失败 ❌ → 分配失败                         │
│ Thread 3: CAS失败 ❌ → 分配失败                         │
└─────────────────────────────────────────────────────────┘
```

#### VC Buffer空间分配

**代码** (`src/buffer.cpp:50-56`)：

```cpp
bool Buffer::allocate_buffer(int vcb, int n) {
  int buffer = vc_buffer_[vcb].load();  // 读取当前空闲空间
  while (true) {
    if (buffer < n)
      return false;  // 空间不足
    else if (vc_buffer_[vcb].compare_exchange_weak(buffer, buffer - n))
      return true;   // CAS成功，空间减少n个flit
  }
}
```

**工作原理**：

```
初始状态：vc_buffer_[0] = 20（buffer_size=20）

2个数据包同时请求分配：
- Packet1: 需要4个flit空间
- Packet2: 需要8个flit空间

┌─────────────────────────────────────────────────────────┐
│ Thread 1: CAS(20 → 16) ✅  成功                         │
│ Thread 2: CAS(20 → 12) ❌  失败（20已变成16）           │
│           → 重新读取buffer=16                            │
│           → CAS(16 → 8) ✅  成功                         │
└─────────────────────────────────────────────────────────┘

最终状态：vc_buffer_[0] = 8（剩余8个flit空间）
```

---

## 4. 场景分析

### 场景1：2个独立芯粒内的子任务

```
子任务1：芯粒1内部（节点0→1）
子任务2：芯粒2内部（节点8→9）
```

#### Cycle N的执行过程

```
┌──────────────────────────────────────────────────────────┐
│ Packet1（节点0→1）                                        │
├──────────────────────────────────────────────────────────┤
│ - RC: 计算路由 → East                                     │
│ - VA: 尝试分配芯粒1的East端口VC0                          │
│   → allocate_buffer(VC0, 4) → CAS成功 ✅                  │
│ - SA: 尝试分配芯粒1的East交换机端口                       │
│   → allocate_sw_link() → CAS成功 ✅                       │
│ - 结果：switch_allocated_ = true                         │
└──────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────┐
│ Packet2（节点8→9）                                        │
├──────────────────────────────────────────────────────────┤
│ - RC: 计算路由 → East                                     │
│ - VA: 尝试分配芯粒2的East端口VC0                          │
│   → allocate_buffer(VC0, 4) → CAS成功 ✅                  │
│ - SA: 尝试分配芯粒2的East交换机端口                       │
│   → allocate_sw_link() → CAS成功 ✅                       │
│ - 结果：switch_allocated_ = true                         │
└──────────────────────────────────────────────────────────┘
```

**结论**：
- ✅ **两个子任务完全并行执行**
- ✅ **无资源冲突**（在不同芯粒上）
- ✅ **都成功完成RC+VA+SA，进入LT阶段**

### 场景2：3个子任务，包含跨芯粒

```
子任务1：芯粒1内部（节点0→1）
子任务2：芯粒2内部（节点8→9）
子任务3：芯粒1→2（节点0→2）
```

#### Cycle N的执行过程

```
┌──────────────────────────────────────────────────────────┐
│ Packet1（节点0→1）- 在节点0                               │
├──────────────────────────────────────────────────────────┤
│ - RC: 路由 → East（节点0→1）                              │
│ - VA: 分配节点0的East端口VC0                              │
│   → allocate_buffer(East.VC0, 4) → CAS成功 ✅            │
│ - SA: 分配节点0的East交换机端口                           │
│   → allocate_sw_link() → CAS成功 ✅                       │
│ - 结果：成功 ✅                                           │
└──────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────┐
│ Packet2（节点8→9）- 在节点8                               │
├──────────────────────────────────────────────────────────┤
│ - RC: 路由 → East（节点8→9）                              │
│ - VA: 分配节点8的East端口VC0                              │
│   → allocate_buffer(East.VC0, 4) → CAS成功 ✅            │
│ - SA: 分配节点8的East交换机端口                           │
│   → allocate_sw_link() → CAS成功 ✅                       │
│ - 结果：成功 ✅                                           │
└──────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────┐
│ Packet3（节点0→2）- 在节点0                               │
├──────────────────────────────────────────────────────────┤
│ - RC: 路由 → East（节点0→1，先到达节点1再跨芯粒）        │
│ - VA: 尝试分配节点0的East端口VC0                          │
│   → allocate_buffer(East.VC0, 4)                         │
│   → ❌ 失败！因为Packet1已经占用了East.VC0               │
│   → 尝试East.VC1                                          │
│   → ✅ 成功（假设VC1空闲）                                │
│ - SA: 尝试分配节点0的East交换机端口                       │
│   → allocate_sw_link()                                   │
│   → ❌ 失败！因为Packet1已经占用了交换机端口              │
│ - 结果：失败，等待下一个cycle ⏰                          │
└──────────────────────────────────────────────────────────┘
```

**结论**：
- ✅ **Packet1和2完全并行**（不同节点，无冲突）
- ⚠️ **Packet3受Packet1影响**（同一节点，竞争East端口）
- ✅ **资源竞争自动处理**（通过CAS机制）
- ⏰ **Packet3需要等待下一个cycle重试**

---

## 5. 资源竞争的详细分析

### 5.1 可能的竞争资源

| 资源类型 | 原子变量 | 竞争场景 |
|---------|---------|---------|
| **VC Buffer空间** | `vc_buffer_[vcb]` | 多个数据包请求同一个VC |
| **输入链路** | `in_link_used_` | 多个数据包进入同一个buffer |
| **交换机端口** | `sw_link_used_` | 多个数据包离开同一个节点的同一方向 |
| **VC队列** | `vc_head_packet[vcb]` | 多个数据包排队等待同一个VC |

### 5.2 竞争场景示例

#### 场景A：2个数据包在同一节点竞争East端口

```
Cycle N: 节点0有2个数据包
- Packet1: 0→1（需要East）
- Packet3: 0→2（需要East，先到1再到2）

┌─────────────────────────────────────────────────┐
│ 节点0的East交换机端口（只有1个）                 │
├─────────────────────────────────────────────────┤
│ Packet1: allocate_sw_link() → CAS成功 ✅        │
│ Packet3: allocate_sw_link() → CAS失败 ❌        │
└─────────────────────────────────────────────────┘

结果：
- Packet1: switch_allocated_ = true，开始传输
- Packet3: switch_allocated_ = false，wait_timer_++
```

#### 场景B：2个数据包竞争同一个VC

```
Cycle N: East端口有2个VC
- VC0: 空闲，20个flit空间
- VC1: 空闲，20个flit空间

Packet1和Packet3同时请求：

┌─────────────────────────────────────────────────┐
│ VA阶段：优先尝试VC0                              │
├─────────────────────────────────────────────────┤
│ Packet1: allocate_buffer(VC0, 4) → CAS成功 ✅   │
│ Packet3: allocate_buffer(VC0, 4) → CAS失败 ❌   │
│          → 尝试VC1                               │
│          → allocate_buffer(VC1, 4) → 成功 ✅    │
└─────────────────────────────────────────────────┘

结果：
- Packet1: 使用VC0
- Packet3: 使用VC1
- 两个都可以继续SA阶段
```

#### 场景C：Buffer空间不足

```
Cycle N: VC0有8个flit空间剩余
- Packet1: 需要4个flit
- Packet3: 需要8个flit

┌─────────────────────────────────────────────────┐
│ Packet1: allocate_buffer(VC0, 4)               │
│          → buffer=8, CAS(8→4) ✅ 成功            │
│                                                  │
│ Packet3: allocate_buffer(VC0, 8)               │
│          → buffer=4 < 8 ❌ 失败（空间不足）      │
│          → 尝试VC1...                            │
└─────────────────────────────────────────────────┘
```

---

## 6. 多线程并行

### 线程级并行（额外优化）

模拟器支持多线程加速更新过程：

```cpp
// main.cpp:88-103
if (vec_pkts.size() < param->threads * 1 || param->threads < 2) {
  // 单线程：顺序更新所有数据包
  update_packets(vec_pkts, system);
} else {
  // 多线程：并行更新数据包
  for (int i = 0; i < param->threads; ++i) {
    worker_launch[i] = true;
  }
  cv.notify_all();  // 唤醒所有工作线程
}
```

**工作方式**：
```
假设有100个数据包，4个工作线程：

Thread 1: 更新 Packet[0-24]
Thread 2: 更新 Packet[25-49]
Thread 3: 更新 Packet[50-74]
Thread 4: 更新 Packet[75-99]

→ 所有线程并行执行
→ 使用原子操作避免冲突
→ 加速4倍（理想情况）
```

---

## 7. 时间线示例

### 完整场景：3个子任务的多cycle模拟

```
初始状态：
- Packet1: 节点0，目标节点1（芯粒内）
- Packet2: 节点8，目标节点9（芯粒内）
- Packet3: 节点0，目标节点2（跨芯粒）

all_packets = [Packet1, Packet2, Packet3]
```

#### Cycle 0: 源节点处理

```
Packet1: process_timer_-- (1→0)
Packet2: process_timer_-- (1→0)
Packet3: process_timer_-- (1→0)
```

#### Cycle 1: 流水线 - RC

```
Packet1: RC → East (节点0→1)
Packet2: RC → East (节点8→9)
Packet3: RC → East (节点0→1→2)
```

#### Cycle 2: 流水线 - VA

```
Packet1: VA → 节点0.East.VC0 ✅
Packet2: VA → 节点8.East.VC0 ✅
Packet3: VA → 节点0.East.VC0 ❌（被Packet1占用）
         → 尝试VC1 ✅
```

#### Cycle 3: 流水线 - SA

```
Packet1: SA → 节点0.East交换机 ✅
         → switch_allocated_ = true
         → link_timer_ = 1

Packet2: SA → 节点8.East交换机 ✅
         → switch_allocated_ = true
         → link_timer_ = 1

Packet3: SA → 节点0.East交换机 ❌（被Packet1占用）
         → wait_timer_++
```

#### Cycle 4: LT + 重试SA

```
Packet1: link_timer_-- (1→0)
         → 头flit到达节点1

Packet2: link_timer_-- (1→0)
         → 头flit到达节点9

Packet3: SA重试 → 节点0.East交换机 ✅
         → switch_allocated_ = true
         → link_timer_ = 1
```

#### Cycle 5-7: 继续传输

```
Packet1: 尾部flit传输，完成第一跳
Packet2: 尾部flit传输，完成第一跳
Packet3: link_timer_倒计时，头flit飞行
```

**关键观察**：
- ✅ Packet1和2完全独立，全程并行
- ⚠️ Packet3在Cycle 3被阻塞，Cycle 4成功
- ✅ 资源竞争自动处理，无需手动干预

---

## 8. 关键代码总结

### 主模拟循环
**位置**: `src/main.cpp:139-142`
```cpp
for (uint64_t i = 0; i < num_cycles; i++) {
  TM->genMes(all_packets, i);
  run_one_cycle(all_packets, network);
}
```

### 并行更新
**位置**: `src/main.cpp:27-43`
```cpp
static void update_packets(std::vector<Packet*>& packets, System* system) {
  for (each packet in packets) {
    system->update(*packet);
  }
}
```

### 资源分配（CAS）
**位置**: `src/buffer.cpp:94-100`
```cpp
bool Buffer::allocate_sw_link() {
  bool link_used_state = sw_link_used_.load();
  if (sw_link_used_.compare_exchange_strong(link_used_state, true)) {
    return true;  // 只有一个线程能成功
  }
  return false;
}
```

### Buffer空间分配
**位置**: `src/buffer.cpp:50-56`
```cpp
bool Buffer::allocate_buffer(int vcb, int n) {
  int buffer = vc_buffer_[vcb].load();
  if (buffer >= n &&
      vc_buffer_[vcb].compare_exchange_weak(buffer, buffer - n)) {
    return true;
  }
  return false;
}
```

---

## 9. 总结

### ✅ 您的理解完全正确

1. **子任务并行处理**：
   - 所有子任务在同一个cycle中并行更新
   - 不同芯粒的子任务完全独立，无冲突
   - 同一节点的子任务会竞争资源

2. **资源考虑机制**：
   - 子任务3自动考虑子任务1和2的资源占用
   - 通过原子操作（CAS）保证互斥性
   - 竞争失败的子任务自动等待下一个cycle

3. **并行化层次**：
   - **Cycle级并行**：所有数据包同时更新
   - **线程级并行**：多线程加速计算（可选）
   - **Router级并行**：不同router的操作完全独立

### 对比表

| 场景 | 子任务1 | 子任务2 | 子任务3 | 并行性 |
|------|---------|---------|---------|--------|
| 不同芯粒 | 芯粒1 | 芯粒2 | - | 完全并行 ✅ |
| 不同节点 | 节点0 | 节点1 | 节点2 | 完全并行 ✅ |
| 同一节点不同方向 | 节点0→East | 节点0→North | - | 完全并行 ✅ |
| 同一节点同一方向 | 节点0→East | 节点0→East | - | 竞争，顺序化 ⚠️ |

### 性能影响

**理想情况**（无竞争）：
- N个独立子任务 = 1个子任务的时间（完全并行）

**拥塞情况**（有竞争）：
- 竞争的子任务需要额外等待cycles
- 通过增加VC数量可以减少竞争

### 配置参数

**减少竞争的方法**：
```ini
[Network]
vc_number = 4        # 增加虚拟通道数量（默认2）
buffer_size = 40     # 增加buffer大小（默认20）
router_stages = OneStage  # 减少流水线深度（RC/VA/SA并行）
```

---

## 10. 验证方法

### 如何验证并行性？

**实验**：创建3个独立子任务，对比延迟

```yaml
# test_parallel.yaml
tasks:
  # 3个完全独立的子任务（不同芯粒）
  - {src: 0, dst: 1, cycle: 0, size: 64}   # 芯粒1内
  - {src: 8, dst: 9, cycle: 0, size: 64}   # 芯粒2内
  - {src: 4, dst: 5, cycle: 0, size: 64}   # 芯粒3内
```

**期望结果**：
- 3个子任务的平均延迟 ≈ 单个子任务的延迟
- 证明它们是并行处理的

**对比实验**：3个竞争子任务

```yaml
# test_contention.yaml
tasks:
  # 3个竞争同一资源的子任务（同一节点）
  - {src: 0, dst: 1, cycle: 0, size: 64}   # 节点0→East
  - {src: 0, dst: 2, cycle: 0, size: 64}   # 节点0→East（竞争）
  - {src: 0, dst: 5, cycle: 0, size: 64}   # 节点0→East+South（竞争）
```

**期望结果**：
- 平均延迟 > 单个子任务延迟
- 因为存在资源竞争和等待
