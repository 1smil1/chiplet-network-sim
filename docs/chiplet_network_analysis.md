# Chiplet网络模拟器完整分析

## 目录
- [1. 延迟建模分析](#1-延迟建模分析)
- [2. Netrace文件格式详解](#2-netrace文件格式详解)
- [3. 节点ID到芯粒映射机制](#3-节点id到芯粒映射机制)
- [4. 如何创建自定义Netrace文件](#4-如何创建自定义netrace文件)
- [5. 完整示例](#5-完整示例)

---

## 1. 延迟建模分析

### 1.1 核心问题
该模拟器是否区分并建模了：
- ✅ **芯粒内部通信延迟**（intra-chiplet latency）
- ✅ **芯粒间通信延迟**（inter-chiplet latency）

### 1.2 答案：是的！

### 1.3 通过Channel抽象区分通信类型

**位置**: `src/config.h` (lines 22-37)

```cpp
const Channel on_chip_channel(1, 1);                    // 芯粒内: 带宽=1, 延迟=1周期
const Channel off_chip_parallel_channel(1, 2);          // 芯粒间并行: 带宽=1, 延迟=2周期
const Channel off_chip_serial_channel(2, 4);            // 芯粒间串行: 带宽=2, 延迟=4周期
const Channel long_distance_channel(1, 10);             // 长距离: 带宽=1, 延迟=10周期
```

**延迟差异**:
| 通信类型 | 延迟 | 带宽 | 使用场景 |
|---------|------|------|---------|
| **芯粒内通信** | 1周期 | 1 flit/cycle | on-chip mesh内的XY路由 |
| **芯粒间并行D2D** | 2周期 | 1 flit/cycle | 更快的D2D接口 |
| **芯粒间串行D2D** | 4周期 | 2 flits/cycle | 更高带宽，延迟较大 |
| **长距离** | 10周期 | 1 flit/cycle | 特殊长距离链路 |

### 1.4 Hop类型追踪机制

**位置**: `src/packet.h` (lines 29-32)

每个数据包追踪不同类型的跳数：

```cpp
class Packet {
    int internal_hops_;      // 芯粒内跳数（使用on_chip_channel）
    int parallel_hops_;      // 并行D2D跳数（使用off_chip_parallel_channel）
    int serial_hops_;        // 串行D2D跳数（使用off_chip_serial_channel）
    int other_hops_;         // 其他类型（扩展性）
};
```

### 1.5 跳数统计逻辑

**位置**: `src/system.cpp` (lines 156-167)

```cpp
if (temp1.buffer->channel_ == on_chip_channel)
    p.internal_hops_++;
else if (temp1.buffer->channel_ == off_chip_parallel_channel)
    p.parallel_hops_++;
else if (temp1.buffer->channel_ == off_chip_serial_channel)
    p.serial_hops_++;
else
    p.other_hops_++;
```

### 1.6 D2D接口配置

**位置**: `src/topologies/multiple_chip_mesh.cpp` (lines 43-46)

```cpp
if (d2d_IF_ == "off_chip_parallel")
    node->xneg_in_buffer_->channel_ = off_chip_parallel_channel;
else if (d2d_IF_ == "off_chip_serial")
    node->xneg_in_buffer_->channel_ = off_chip_serial_channel;
```

通过配置文件中的 `d2d_IF` 参数选择：
- `d2d_IF = off_chip_parallel` → 2周期延迟
- `d2d_IF = off_chip_serial` → 4周期延迟

### 1.7 实验验证

运行输出示例：
```
Internal Hops: 4.50877   ← 芯粒内跳数
Parallel Hops: 0         ← 并行D2D跳数
Serial Hops: 1.01754     ← 串行D2D跳数
Other Hops: 0
```

---

## 2. Netrace文件格式详解

### 2.1 Netrace简介

Netrace是由德州大学奥斯汀分校开发的网络trace格式，用于存储周期精确的网络流量记录。

### 2.2 文件结构

Netrace文件采用**二进制格式**，由以下部分组成：

```
┌─────────────────────┐
│   Header            │  ← 文件头信息
├─────────────────────┤
│   Notes (可选)      │  ← 文本注释
├─────────────────────┤
│   Regions (可选)    │  ← 区域信息
├─────────────────────┤
│   Packet 1          │  ← 数据包记录
├─────────────────────┤
│   Dependencies 1    │  ← 依赖关系
├─────────────────────┤
│   Packet 2          │
├─────────────────────┤
│   Dependencies 2    │
├─────────────────────┤
│   ...               │
└─────────────────────┘
```

### 2.3 Header结构

**C结构定义** (`src/netrace/netrace.h`):

```c
struct nt_header {
    unsigned int nt_magic;                    // 魔数: 0x484A5455
    float version;                            // 版本号: 1.0
    char benchmark_name[30];                  // benchmark名称
    unsigned char num_nodes;                  // 节点数量
    unsigned long long int num_cycles;        // 仿真周期数
    unsigned long long int num_packets;       // 数据包总数
    unsigned int notes_length;                // 注释长度
    unsigned int num_regions;                 // 区域数量
    char* notes;                              // 注释内容
    nt_regionhead_t* regions;                 // 区域头信息
};
```

### 2.4 Packet结构

```c
struct nt_packet {
    unsigned long long int cycle;    // 注入周期
    unsigned int id;                 // 数据包ID
    unsigned int addr;               // 地址
    unsigned char type;              // 类型（ReadReq, WriteReq等）
    unsigned char src;               // 源节点ID
    unsigned char dst;               // 目标节点ID
    unsigned char node_types;        // 节点类型（L1/L2/MC）
    unsigned char num_deps;          // 依赖数量
    nt_dependency_t* deps;           // 依赖列表
};
```

### 2.5 支持的数据包类型

```c
const char* nt_packet_types[] = {
    "InvalidCmd", "ReadReq", "ReadResp",
    "ReadRespWithInvalidate", "WriteReq", "WriteResp",
    "Writeback", "UpgradeReq", "UpgradeResp",
    "ReadExReq", "ReadExResp", "InvalidateReq",
    "InvalidateResp", "DowngradeReq", "DowngradeResp",
    // ... 共31种类型
};
```

---

## 3. 节点ID到芯粒映射机制

### 3.1 MultiChipMesh拓扑的ID映射

**关键代码**: `src/topologies/multiple_chip_mesh.h` (lines 12-20)

```cpp
inline NodeID id2nodeid(int id) const override {
    int K = k_node_ * k_chip_;        // 总行/列数
    int x = id % K;                    // 全局x坐标
    int y = id / K;                    // 全局y坐标
    int node_id = x % k_node_ + (y % k_node_) * k_node_;  // 芯粒内节点ID
    int chip_id = x / k_node_ + (y / k_node_) * k_chip_;  // 芯粒ID
    return NodeID(node_id, chip_id);
}
```

### 3.2 映射公式

假设配置为：
- `k_node = 4`（每个芯粒4x4节点）
- `k_chip = 2`（2x2芯粒阵列）
- 总节点数 = 64（4×4×2×2）

**ID → 坐标映射**：

```
全局ID → (全局x, 全局y) → (芯粒x, 芯粒y, 节点x, 节点y)

例子1: ID=0
  K = 4*2 = 8
  全局x = 0 % 8 = 0
  全局y = 0 / 8 = 0
  芯粒x = 0 / 4 = 0
  芯粒y = 0 / 4 = 0
  节点x = 0 % 4 = 0
  节点y = 0 % 4 = 0
  → Chiplet(0,0) 的 Node(0,0)

例子2: ID=34
  K = 8
  全局x = 34 % 8 = 2
  全局y = 34 / 8 = 4
  芯粒x = 2 / 4 = 0
  芯粒y = 4 / 4 = 1
  节点x = 2 % 4 = 2
  节点y = 4 % 4 = 0
  → Chiplet(0,1) 的 Node(2,0)

例子3: ID=6
  K = 8
  全局x = 6 % 8 = 6
  全局y = 6 / 8 = 0
  芯粒x = 6 / 4 = 1
  芯粒y = 0 / 4 = 0
  节点x = 6 % 4 = 2
  节点y = 0 % 4 = 0
  → Chiplet(1,0) 的 Node(2,0)
```

### 3.3 判断通信类型

基于源节点和目标节点的芯粒ID：

```python
def is_intra_chiplet(src_id, dst_id, k_node, k_chip):
    """判断是否为芯粒内通信"""
    K = k_node * k_chip
    src_x, src_y = src_id % K, src_id // K
    dst_x, dst_y = dst_id % K, dst_id // K

    src_chip_x = src_x // k_node
    src_chip_y = src_y // k_node
    dst_chip_x = dst_x // k_node
    dst_chip_y = dst_y // k_node

    # 同一个芯粒
    return (src_chip_x == dst_chip_x and src_chip_y == dst_chip_y)

# 示例
# k_node=4, k_chip=2的情况下
# ID 0-15: Chiplet(0,0)
# ID 16-31: Chiplet(1,0)
# ID 32-47: Chiplet(0,1)
# ID 48-63: Chiplet(1,1)

# 芯粒内通信: src=0, dst=5 (都在Chiplet 0,0)
# 芯粒间通信: src=0, dst=20 (Chiplet 0,0 → Chiplet 1,0)
```

### 3.4 可视化示例（2x2芯粒，每个2x2节点）

```
全局视图（16个节点）:
  0   1 | 2   3
  4   5 | 6   7
  ------+------
  8   9 | 10  11
  12  13| 14  15

芯粒分布:
  Chiplet(0,0) | Chiplet(1,0)
  0  1  2  3   | 16 17 18 19
  4  5  6  7   | 20 21 22 23
  8  9  10 11  | 24 25 26 27
  12 13 14 15  | 28 29 30 31
  -------------+-------------
  Chiplet(0,1) | Chiplet(1,1)
  32 33 34 35  | 48 49 50 51
  36 37 38 39  | 52 53 54 55
  40 41 42 43  | 56 57 58 59
  44 45 46 47  | 60 61 62 63
```

---

## 4. 如何创建自定义Netrace文件

### 4.1 需要的工具

创建Python脚本 `create_netrace.py` 来生成二进制netrace文件。

### 4.2 关键步骤

1. **定义Header**
2. **创建Packet列表**
3. **写入二进制文件**
4. **压缩为.bz2**

---

## 5. 完整示例

见配套的Python脚本 `tools/create_netrace.py`

### 5.1 芯粒内通信示例

```python
# Chiplet(0,0)内部通信: Node 0 → Node 5
# k_node=4, k_chip=2
src = 0   # Chiplet(0,0) Node(0,0)
dst = 5   # Chiplet(0,0) Node(1,1)
```

### 5.2 芯粒间通信示例

```python
# 跨芯粒通信: Chiplet(0,0) → Chiplet(1,0)
src = 0   # Chiplet(0,0) Node(0,0)
dst = 20  # Chiplet(1,0) Node(0,1)
```

### 5.3 运行自定义trace

```bash
# 1. 创建trace文件
python tools/create_netrace.py

# 2. 修改配置
# input/multiple_chip_mesh_4x4.ini:
# netrace_file = ./input/netrace/custom.tra.bz2

# 3. 运行模拟
./ChipletNetworkSim.exe input/multiple_chip_mesh_4x4.ini

# 4. 查看结果
cat output/output.csv
```

---

## 附录：关键文件位置

### 延迟建模相关
- `src/config.h:22-37` - Channel定义
- `src/packet.h:29-32` - Hop计数器
- `src/system.cpp:156-167` - 延迟应用和hop统计
- `src/buffer.h:56` - Buffer中的channel成员
- `src/topologies/multiple_chip_mesh.cpp:33-91` - D2D链路设置

### Netrace相关
- `src/netrace/netrace.h` - Netrace头文件
- `src/netrace/netrace.c` - Netrace实现
- `src/traffic_manager.cpp:353-374` - Netrace数据包生成
- `src/topologies/multiple_chip_mesh.h:12-20` - ID映射函数

### 配置文件
- `input/multiple_chip_mesh_4x4.ini` - 主配置文件
- `src/config.cpp:14-33` - 配置文件解析

---

## 参考文献

1. Feng et al., "Evaluating Chiplet-based Large-Scale Interconnection Networks via Cycle-Accurate Packet-Parallel Simulation", USENIX ATC 2024
2. Hestness et al., "Netrace: Dependency-Driven Trace-Based Network-on-Chip Simulation", 2010
3. 项目GitHub: https://github.com/Yinxiao-Feng/chiplet-network-sim
