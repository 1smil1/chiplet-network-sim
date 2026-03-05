# 16节点场景跳数详细分析

## 节点映射验证（k_node=2, k_chip=2）

```
节点ID | Chiplet    | 芯粒内坐标 | 全局坐标 | 芯粒ID
-------|------------|-----------|---------|-------
0      | (0,0)      | (0,0)     | (0,0)   | 0
1      | (0,0)      | (1,0)     | (1,0)   | 0
2      | (1,0)      | (0,0)     | (2,0)   | 1  ← 不同芯粒!
5      | (0,0)      | (1,1)     | (1,1)   | 0
8      | (0,1)      | (0,0)     | (0,2)   | 2
15     | (1,1)      | (1,1)     | (3,3)   | 3
```

## 任务列表

```yaml
tasks:
  - src: 0, dst: 5,  cycle: 0   # 任务1
  - src: 0, dst: 2,  cycle: 10  # 任务2 ← 您关注的任务
  - src: 0, dst: 8,  cycle: 20  # 任务3
  - src: 0, dst: 15, cycle: 30  # 任务4
```

## 逐任务跳数分析

### 任务1: 节点0 → 节点5

**节点信息**:
- 源: 节点0, Chiplet(0,0), 全局(0,0)
- 目标: 节点5, Chiplet(0,0), 全局(1,1)
- **通信类型**: 芯粒内（同在Chiplet 0）

**XY路由路径** (先X后Y):
```
(0,0) → (1,0) → (1,1)
节点0  → 节点1  → 节点5
```

**跳数计算**:
- 跳1: 节点0→1
  - 节点0: Chiplet(0,0)
  - 节点1: Chiplet(0,0)
  - 同芯粒 ✓
  - buffer->channel_ = **on_chip_channel**
  - **internal_hops++ → 1**

- 跳2: 节点1→5
  - 节点1: Chiplet(0,0)
  - 节点5: Chiplet(0,0)
  - 同芯粒 ✓
  - buffer->channel_ = **on_chip_channel**
  - **internal_hops++ → 2**

**结果**: internal_hops=2, serial_hops=0

---

### 任务2: 节点0 → 节点2 ⭐️

**节点信息**:
- 源: 节点0, Chiplet(0,0), 全局(0,0)
- 目标: 节点2, Chiplet(1,0), 全局(2,0)
- **通信类型**: 芯粒间（Chiplet 0→1）

**XY路由路径** (先X后Y):
```
(0,0) → (1,0) → (2,0)
节点0  → 节点1  → 节点2
```

**跳数计算**:
- 跳1: 节点0→1
  - 节点0: Chiplet(0,0) ← 芯粒ID=0
  - 节点1: Chiplet(0,0) ← 芯粒ID=0
  - **同芯粒内** ✓
  - buffer->channel_ = **on_chip_channel**
  - **internal_hops++ → 1**

- 跳2: 节点1→2
  - 节点1: Chiplet(0,0) ← 芯粒ID=0
  - 节点2: Chiplet(1,0) ← 芯粒ID=1
  - **跨芯粒边界** ⚠️
  - buffer->channel_ = **off_chip_serial_channel**
  - **serial_hops++ → 1**

**结果**: internal_hops=1, serial_hops=1

**您的理解完全正确！**
- 第一跳（节点0→1）是芯粒内，internal_hops+1
- 第二跳（节点1→2）跨越芯粒边界，serial_hops+1

---

### 任务3: 节点0 → 节点8

**节点信息**:
- 源: 节点0, Chiplet(0,0), 全局(0,0)
- 目标: 节点8, Chiplet(0,1), 全局(0,2)
- **通信类型**: 芯粒间（Chiplet 0→2）

**XY路由路径** (先X后Y):
```
(0,0) → (0,1) → (0,2)
节点0  → 节点4  → 节点8
```

**节点4验证**:
```python
K = 4, x = 4%4 = 0, y = 4//4 = 1
chip_x = 0//2 = 0, chip_y = 1//2 = 0
→ 节点4: Chiplet(0,0) Node(0,1), 芯粒ID=0
```

**跳数计算**:
- 跳1: 节点0→4
  - 节点0: Chiplet(0,0)
  - 节点4: Chiplet(0,0)
  - 同芯粒 ✓
  - **internal_hops++ → 1**

- 跳2: 节点4→8
  - 节点4: Chiplet(0,0), 芯粒ID=0
  - 节点8: Chiplet(0,1), 芯粒ID=2
  - 跨芯粒 ⚠️
  - **serial_hops++ → 1**

**结果**: internal_hops=1, serial_hops=1

---

### 任务4: 节点0 → 节点15

**节点信息**:
- 源: 节点0, Chiplet(0,0), 全局(0,0)
- 目标: 节点15, Chiplet(1,1), 全局(3,3)
- **通信类型**: 芯粒间（对角，Chiplet 0→3）

**XY路由路径** (先X后Y):
```
(0,0) → (1,0) → (2,0) → (3,0) → (3,1) → (3,2) → (3,3)
节点0  → 节点1  → 节点2  → 节点3  → 节点7  → 节点11 → 节点15
```

**中间节点验证**:
```
节点3: x=3, y=0 → chip_x=1, chip_y=0 → Chiplet(1,0), 芯粒ID=1
节点7: x=3, y=1 → chip_x=1, chip_y=0 → Chiplet(1,0), 芯粒ID=1
节点11: x=3, y=2 → chip_x=1, chip_y=1 → Chiplet(1,1), 芯粒ID=3
```

**跳数计算**:
- 跳1: 节点0→1 (Chiplet 0→0) → **internal_hops+1**
- 跳2: 节点1→2 (Chiplet 0→1) → **serial_hops+1** ⚠️
- 跳3: 节点2→3 (Chiplet 1→1) → **internal_hops+1**
- 跳4: 节点3→7 (Chiplet 1→1) → **internal_hops+1**
- 跳5: 节点7→11 (Chiplet 1→3) → **serial_hops+1** ⚠️
- 跳6: 节点11→15 (Chiplet 3→3) → **internal_hops+1**

**结果**: internal_hops=4, serial_hops=2

---

## 总计和平均值

### 各任务跳数汇总

| 任务 | 路径        | Internal Hops | Serial Hops |
|------|------------|---------------|-------------|
| 1    | 0→5        | 2             | 0           |
| 2    | 0→2        | 1             | 1           |
| 3    | 0→8        | 1             | 1           |
| 4    | 0→15       | 4             | 2           |
| **总计** |        | **8**         | **4**       |

### 平均值计算

```
message_arrived = 4

average_internal_hops = 8 / 4 = 2.0
average_serial_hops = 4 / 4 = 1.0
```

### 实际模拟器输出对比

```
Injected: 4    Arrived: 4    Timeout: 0
Average latency: 17.5
Internal Hops: 2   Serial Hops: 1
```

**✅ 完全匹配！**

---

## 关键结论

### 问题1: 模拟器如何知道节点位置？

1. **两层坐标系统**:
   - 芯粒坐标: `chip_coordinate_[0,1]` 存储在ChipMesh对象中
   - 节点坐标: `x_, y_` 存储在NodeMesh对象中
   - 全局坐标 = (chip_x * k_node + x_, chip_y * k_node + y_)

2. **节点查找流程**:
   ```
   全局ID → id2nodeid() → NodeID(node_id, chip_id)
         → get_chip(chip_id) → ChipMesh对象
         → get_node(node_id) → NodeMesh对象
         → 读取 x_, y_, chip_coordinate_
   ```

3. **位置信息使用时机**:
   - XY路由算法计算时读取坐标
   - 每次路由决策都基于当前节点和目标节点的全局坐标

### 问题2: 芯粒内和芯粒间跳数如何计算？

1. **判断依据**:
   - **不是直接比较节点位置**
   - 而是检查目标buffer的`channel_`类型
   - `on_chip_channel` → internal_hops++
   - `off_chip_serial_channel` → serial_hops++

2. **Channel设置时机**:
   - 芯粒内连接: 默认使用`on_chip_channel`
   - 芯粒间连接: 在`connect_chiplets()`中设置为`off_chip_serial_channel`
   - 设置位置: 芯粒边界节点的输入buffer

3. **任务2的具体跳数**（您的例子）:
   ```
   节点0 → 节点2 (总跳数=2)

   跳1: 0→1 (芯粒内)
     - 节点0和1都在Chiplet(0,0)
     - buffer->channel_ = on_chip_channel
     - internal_hops = 1

   跳2: 1→2 (芯粒间) ⭐️
     - 节点1在Chiplet(0,0), 节点2在Chiplet(1,0)
     - buffer->channel_ = off_chip_serial_channel
     - serial_hops = 1

   最终: internal_hops=1, serial_hops=1 ✓
   ```

4. **输出含义**:
   - 显示的是**平均值**，不是总和
   - Internal Hops: 2 = (2+1+1+4) / 4
   - Serial Hops: 1 = (0+1+1+2) / 4

### 您的理解100%正确！

您说的"先到节点1，芯粒内跳数+1，然后从节点1到节点2，芯粒间跳数+1"完全准确！
