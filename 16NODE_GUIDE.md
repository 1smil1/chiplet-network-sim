# 16节点芯粒网络模拟器使用指南

## 概述

本指南介绍如何使用新的工具链为16节点场景（2x2芯粒，每个芯粒2x2节点）创建和运行自定义通信任务。

## 关键发现：模拟器功能

### 1. 库可以做什么？

该芯粒网络模拟器可以：

- **模拟多芯粒网络架构**：支持可配置的芯粒和节点数量
- **精确的延迟建模**：区分芯粒内通信（1周期）和芯粒间通信（2-4周期）
- **XY路由算法**：动态路由，每跳计算一次，先X后Y
- **跳数统计**：分别追踪芯粒内跳数（Internal Hops）和芯粒间跳数（Serial/Parallel Hops）
- **自定义trace**：通过netrace文件指定任意通信模式
- **性能分析**：输出平均延迟、注入率、接收率等指标

### 2. 路由和延迟机制

#### XY路由工作原理

**位置**: `src/topologies/multiple_chip_mesh.cpp:104-131`

```cpp
// 计算全局坐标（融合芯粒坐标和节点坐标）
int cur_x = current_chip->chip_coordinate_[0] * k_node_ + current_node->x_;
int cur_y = current_chip->chip_coordinate_[1] * k_node_ + current_node->y_;
int dest_x = destination_chip->chip_coordinate_[0] * k_node_ + destination_node->x_;
int dest_y = destination_chip->chip_coordinate_[1] * k_node_ + destination_node->y_;

// 先X后Y路由
if (dis_x != 0)     // 优先X方向
    // 选择X方向端口
else if (dis_y != 0) // 然后Y方向
    // 选择Y方向端口
```

**特点**:
- 动态路由：每个节点根据当前位置重新计算下一跳
- 确定性路径：同一源和目标总是走相同路径
- 无死锁：XY路由算法保证无死锁

#### 跳类型判断

**位置**: `src/system.cpp:153-167`

每一跳根据目标buffer的channel类型判断：

```cpp
if (temp1.buffer->channel_ == on_chip_channel)
    p.internal_hops_++;      // 芯粒内跳数
else if (temp1.buffer->channel_ == off_chip_serial_channel)
    p.serial_hops_++;        // 芯粒间跳数（串行D2D）
```

**延迟设置**:
- 芯粒内链路：1周期（`on_chip_channel`）
- 芯粒间串行：4周期（`off_chip_serial_channel`）
- 芯粒间并行：2周期（`off_chip_parallel_channel`）

#### 路径计算示例

**场景**: 节点0 → 节点15 （对角跨芯粒）

```
节点0:  Chiplet(0,0) Node(0,0)  全局坐标: (0,0)
节点15: Chiplet(1,1) Node(1,1)  全局坐标: (3,3)

XY路由路径（先X后Y）:
(0,0) → (1,0) → (2,0) → (3,0) → (3,1) → (3,2) → (3,3)

跳数分析:
- 总跳数: 6
- X方向跳数: 3
- Y方向跳数: 3
- 跨芯粒边界: 2次（X方向1次，Y方向1次）
- Internal Hops: 4（芯粒内跳）
- Serial Hops: 2（芯粒间跳）
```

### 3. 节点ID映射（16节点场景）

**配置**: k_node=2, k_chip=2

#### 节点布局

```
全局坐标:
     x=0  x=1  x=2  x=3
y=0:  0    1    2    3     Chiplet(0,0) | Chiplet(1,0)
y=1:  4    5    6    7
     ──────────────────
y=2:  8    9   10   11     Chiplet(0,1) | Chiplet(1,1)
y=3: 12   13   14   15

芯粒分布:
  Chiplet(0,0): 0, 1, 4, 5
  Chiplet(1,0): 2, 3, 6, 7
  Chiplet(0,1): 8, 9, 12, 13
  Chiplet(1,1): 10, 11, 14, 15
```

#### 映射公式

**位置**: `src/topologies/multiple_chip_mesh.h:12-19`

```cpp
// ID → 坐标
K = k_node * k_chip = 4
x = id % K
y = id // K
chip_x = x // k_node
chip_y = y // k_node
node_x = x % k_node
node_y = y % k_node

// 坐标 → ID
x = chip_x * k_node + node_x
y = chip_y * k_node + node_y
id = y * K + x
```

#### 通信类型示例

```
芯粒内通信:
  0 → 5:   Chiplet(0,0)内部, 跳数=2
  2 → 3:   Chiplet(1,0)内部, 跳数=1

芯粒间通信（水平）:
  0 → 2:   Chiplet(0,0) → (1,0), 跳数=2, 跨1个边界

芯粒间通信（垂直）:
  0 → 8:   Chiplet(0,0) → (0,1), 跳数=2, 跨1个边界

芯粒间通信（对角）:
  0 → 15:  Chiplet(0,0) → (1,1), 跳数=6, 跨2个边界
```

## 快速开始

### 步骤1: 创建YAML任务描述文件

创建 `my_task.yaml`:

```yaml
topology:
  k_node: 2
  k_chip: 2

tasks:
  # 芯粒内通信
  - src: 0
    dst: 5
    cycle: 0
    size: 64

  # 芯粒间通信（水平）
  - src: 0
    dst: 2
    cycle: 10
    size: 128

  # 芯粒间通信（对角）
  - src: 0
    dst: 15
    cycle: 20
    size: 256
```

### 步骤2: 生成netrace文件

```bash
cd tools
python task_to_netrace.py ../my_task.yaml -o ../input/netrace/my_task.tra.bz2 --analyze
```

**输出**:
```
[1/5] 加载任务描述
[2/5] 验证格式
[3/5] 解析任务列表: 3 个任务

任务 0: task_0
  源: 节点0 -> Chiplet(0,0) Node(0,0)
  目标: 节点5 -> Chiplet(0,0) Node(1,1)
  类型: intra_chiplet
  预估跳数: 2

通信统计:
  芯粒内通信: 1 (33.3%)
  芯粒间通信: 2 (66.7%)

[4/5] 生成netrace文件
生成完成!
  文件大小: 168 bytes
```

### 步骤3: 修改配置文件

编辑 `input/multiple_chip_mesh_2x2.ini`，设置：

```ini
netrace_file =./input/netrace/my_task.tra.bz2
```

或使用已有的配置文件。

### 步骤4: 运行模拟器

```bash
./ChipletNetworkSim.exe input/multiple_chip_mesh_2x2.ini
```

**输出示例**:
```
Multi Chip 2D-mesh, 2x2 chiplets, each chiplet 2x2 nodes
Netrace File: ./input/netrace/my_task.tra.bz2
Benchmark: my_task
Simulated Packets: 3

Time elapsed: 0.0025s
Injected: 3    Arrived: 3    Timeout: 0
Average latency: 20.3 cycles
Internal Hops: 1.67   Serial Hops: 0.67
```

### 步骤5: 查看结果

```bash
cat output/output_16node.csv
```

**格式**: `injection_rate, latency, receiving_rate`

```csv
0.000186,20.3,0.000186
```

## 完整YAML格式

### 简化格式（推荐）

```yaml
topology:
  k_node: 2
  k_chip: 2

tasks:
  - {src: 0, dst: 5, cycle: 0, size: 64}
  - {src: 0, dst: 2, cycle: 10, size: 128}
```

### 完整格式（更多控制）

```yaml
topology:
  k_node: 2
  k_chip: 2
  total_nodes: 16

tasks:
  - id: task_1
    name: "芯粒内数据传输"
    source:
      chiplet: [0, 0]
      node: [0, 0]
      node_id: 0
    destination:
      chiplet: [0, 0]
      node: [1, 1]
      node_id: 5
    size: 64
    packet_type: "ReadReq"
    start_cycle: 0
    address: 0x1000

simulation:
  benchmark_name: "my_scenario"
  notes: "自定义测试场景"
```

## 工具说明

### task_to_netrace.py

**功能**: 将YAML任务描述转换为netrace格式

**使用**:

```bash
python task_to_netrace.py <input.yaml> [选项]

选项:
  -o, --output <file>    输出netrace文件路径
  --analyze              显示详细的通信分析
  --validate-only        仅验证YAML，不生成文件
  --show-layout          显示16节点布局图
```

**示例**:

```bash
# 生成并分析
python task_to_netrace.py ../examples/simple_16node.yaml \
    -o ../input/netrace/test.tra.bz2 \
    --analyze

# 仅验证格式
python task_to_netrace.py ../my_task.yaml --validate-only

# 显示节点布局
python task_to_netrace.py ../my_task.yaml --show-layout
```

## 文件组织

```
chiplet-network-sim/
├── tools/
│   ├── create_netrace.py          # 现有工具（底层库）
│   └── task_to_netrace.py         # 新工具（YAML转netrace）
├── input/
│   ├── multiple_chip_mesh_2x2.ini # 16节点配置
│   └── netrace/
│       └── 16node_scenario.tra.bz2
├── examples/
│   └── simple_16node.yaml         # 简单示例
├── output/
│   ├── output_16node.csv          # 模拟结果
│   └── log_16node.txt             # 日志
└── 16NODE_GUIDE.md                # 本指南
```

## 测试结果

### 简单16节点场景测试

**配置**:
- 4个任务
- 1个芯粒内通信 (25%)
- 3个芯粒间通信 (75%)

**结果**:
```
Injected: 4
Arrived: 4
Timeout: 0
Average latency: 17.5 cycles
Internal Hops: 2
Serial Hops: 1
```

**分析**:
- 所有数据包成功到达 ✓
- 平均延迟17.5周期
- 正确区分芯粒内和芯粒间跳数 ✓

## 常见问题

### Q1: 如何确定两个节点是芯粒内还是芯粒间通信？

**A**: 使用节点布局表或工具的分析功能：

```bash
python task_to_netrace.py my_task.yaml --analyze
```

工具会显示每个任务的通信类型。

### Q2: 如何计算预期的跳数？

**A**: 使用Manhattan距离（L1距离）：

```python
# 获取全局坐标
src_x = src_chip_x * 2 + src_node_x
src_y = src_chip_y * 2 + src_node_y
dst_x = dst_chip_x * 2 + dst_node_x
dst_y = dst_chip_y * 2 + dst_node_y

# 计算跳数
total_hops = abs(dst_x - src_x) + abs(dst_y - src_y)
```

### Q3: 为什么我的节点ID映射与预期不同？

**A**: 确保理解映射公式。节点ID是按照全局坐标连续分配的，而不是按芯粒块。使用`--show-layout`查看实际布局。

### Q4: 如何获得总延迟而不只是平均延迟？

**A**: 总延迟 = 平均延迟 × 数据包数量

从输出中：
- `Average latency: 17.5 cycles`
- `Injected: 4`
- 总延迟 = 17.5 × 4 = 70 cycles

### Q5: 如何增加芯粒间D2D链路的延迟？

**A**: 修改配置文件中的`d2d_IF`参数：

```ini
d2d_IF = off_chip_serial    # 4周期延迟
# 或
d2d_IF = off_chip_parallel  # 2周期延迟
```

或修改源代码 `src/config.h`。

## 下一步

### 建议的扩展（P1和P2优先级）

1. **结果提取工具** (`result_extractor.py`)：自动解析CSV和日志
2. **通信分析模块**：更详细的路径和延迟分析
3. **一键式脚本**：自动化完整工作流程
4. **复杂示例**：包含依赖关系的多阶段通信

### 进一步阅读

- 详细分析：`docs/chiplet_network_analysis.md`
- Netrace格式：`NETRACE_GUIDE.md`
- 论文：Feng et al., "Evaluating Chiplet-based Large-Scale Interconnection Networks", USENIX ATC 2024

## 参考

### 关键代码位置

- XY路由实现：`src/topologies/multiple_chip_mesh.cpp:104-131`
- 跳类型判断：`src/system.cpp:153-167`
- 节点ID映射：`src/topologies/multiple_chip_mesh.h:12-19`
- 延迟定义：`src/config.h:22-37`
- Hop统计：`src/system.cpp:156-167`

### 配置文件

- 16节点：`input/multiple_chip_mesh_2x2.ini`
- 64节点：`input/multiple_chip_mesh_4x4.ini`

---

**祝使用愉快！如有问题，请参考本指南或查看源代码注释。**
