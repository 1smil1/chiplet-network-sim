# Chiplet网络模拟器 - Netrace文件完全指南

## 📚 目录

1. [延迟建模确认](#1-延迟建模确认)
2. [Netrace文件格式](#2-netrace文件格式)
3. [节点ID映射机制](#3-节点id映射机制)
4. [工具使用](#4-工具使用)
5. [完整示例](#5-完整示例)

---

## 1. 延迟建模确认

### ✅ 核心结论

**该模拟器完全支持并区分芯粒内和芯粒间通信延迟！**

### 延迟配置（src/config.h）

```cpp
// 芯粒内：1周期延迟
const Channel on_chip_channel(1, 1);

// 芯粒间（并行D2D）：2周期延迟
const Channel off_chip_parallel_channel(1, 2);

// 芯粒间（串行D2D）：4周期延迟
const Channel off_chip_serial_channel(2, 4);
```

### 实际运行验证

```bash
$ ./ChipletNetworkSim.exe input/multiple_chip_mesh_4x4.ini

# 输出结果：
Internal Hops: 4.50877   # 芯粒内跳数
Serial Hops: 1.01754     # 芯粒间跳数（使用串行D2D）
```

---

## 2. Netrace文件格式

### 文件结构

```
[Header - 固定大小]
  ├─ Magic Number: 0x484A5455
  ├─ Version: 1.0
  ├─ Benchmark Name: 最多30字符
  ├─ Num Nodes: 节点数（如64）
  ├─ Num Cycles: 仿真周期数
  ├─ Num Packets: 数据包总数
  └─ Regions Info: 区域信息

[Packets - 变长]
  ├─ Packet 1 (21 bytes固定 + 依赖)
  │   ├─ Cycle: 8 bytes
  │   ├─ ID: 4 bytes
  │   ├─ Addr: 4 bytes
  │   ├─ Type: 1 byte
  │   ├─ SRC: 1 byte  ← 源节点ID
  │   ├─ DST: 1 byte  ← 目标节点ID
  │   ├─ Node Types: 1 byte
  │   ├─ Num Deps: 1 byte
  │   └─ Dependencies: 4*num_deps bytes
  ├─ Packet 2
  └─ ...
```

### 关键字段说明

| 字段 | 类型 | 说明 |
|------|------|------|
| cycle | uint64 | 数据包注入周期 |
| id | uint32 | 唯一标识符 |
| **src** | uint8 | **源节点ID（0-255）** |
| **dst** | uint8 | **目标节点ID（0-255）** |
| type | uint8 | ReadReq/WriteReq等 |

---

## 3. 节点ID映射机制

### 配置示例：2x2芯粒，每芯粒4x4节点

```ini
[Network]
k_node = 4  # 每个芯粒4x4节点
k_chip = 2  # 2x2芯粒阵列
```

### ID分布图

```
总节点数：64

┌─────────────────┬─────────────────┐
│  Chiplet (0,0)  │  Chiplet (1,0)  │
│   IDs: 0-15     │   IDs: 16-31    │
│                 │                 │
│  0  1  2  3     │  16 17 18 19    │
│  4  5  6  7     │  20 21 22 23    │
│  8  9 10 11     │  24 25 26 27    │
│ 12 13 14 15     │  28 29 30 31    │
├─────────────────┼─────────────────┤
│  Chiplet (0,1)  │  Chiplet (1,1)  │
│   IDs: 32-47    │   IDs: 48-63    │
│                 │                 │
│ 32 33 34 35     │  48 49 50 51    │
│ 36 37 38 39     │  52 53 54 55    │
│ 40 41 42 43     │  56 57 58 59    │
│ 44 45 46 47     │  60 61 62 63    │
└─────────────────┴─────────────────┘
```

### 映射公式（C++代码）

```cpp
// src/topologies/multiple_chip_mesh.h
inline NodeID id2nodeid(int id) const {
    int K = k_node * k_chip;  // = 8
    int x = id % K;
    int y = id / K;

    int chip_x = x / k_node;
    int chip_y = y / k_node;
    int node_x = x % k_node;
    int node_y = y % k_node;

    int chip_id = chip_x + chip_y * k_chip;
    int node_id = node_x + node_y * k_node;

    return NodeID(node_id, chip_id);
}
```

### 具体例子

```
ID=0:  Chiplet(0,0) Node(0,0) ← 芯粒内
ID=5:  Chiplet(0,0) Node(1,1) ← 芯粒内
ID=20: Chiplet(1,0) Node(0,2) ← 芯粒间（跨chiplet）

src=0, dst=5   → 芯粒内通信（都在Chiplet 0,0）
src=0, dst=20  → 芯粒间通信（0,0 → 1,0）
```

---

## 4. 工具使用

### 安装

无需安装，直接使用Python 3：

```bash
cd tools
python create_netrace.py --help
```

### 生成示例trace

```bash
# 基本示例（8个数据包，包含芯粒内和芯粒间通信）
python create_netrace.py --type example

# 输出：
# Creating network topology: 2x2 chiplets, 4x4 nodes/chiplet
# Total nodes: 64
#
# Example 1: Intra-chiplet communication
# Node ID 0: Chiplet(0,0) Node(0,0)
# Node ID 3: Chiplet(0,0) Node(3,0)
#   → Type: Intra-chiplet
#
# Example 2: Inter-chiplet communication
# Node ID 0: Chiplet(0,0) Node(0,0)
# Node ID 20: Chiplet(1,0) Node(0,2)
#   → Type: Inter-chiplet
#
# [OK] Trace file created!
#   Location: ../input/netrace/custom.tra.bz2
#   Size: 220 bytes
```

### 生成压力测试trace

```bash
# 1000个随机数据包
python create_netrace.py --type stress --num_packets 1000

# 输出统计：
# Creating stress test trace: 1000 packets
#   Intra-chiplet: 250 (25.0%)
#   Inter-chiplet: 750 (75.0%)
# [OK] Stress test trace created
```

### 自定义配置

```bash
# 更大规模：4x4芯粒，每个8x8节点 = 1024节点
python create_netrace.py --k_node 8 --k_chip 4 --output my_trace.tra.bz2
```

---

## 5. 完整示例

### 步骤1：生成trace文件

```bash
cd tools
python create_netrace.py --type example --k_node 4 --k_chip 2
```

### 步骤2：修改配置文件

编辑 `input/multiple_chip_mesh_4x4.ini`：

```ini
[Network]
topology = MultiChipMesh
routing_algorithm = XY
k_node = 4                    # 必须匹配trace
k_chip = 2                    # 必须匹配trace
d2d_IF = off_chip_serial      # 芯粒间接口类型

[Workload]
traffic = netrace             # 使用trace模式

[Files]
netrace_file = ./input/netrace/custom.tra.bz2  # trace文件路径
output_file = ./output/output.csv
log_file = ./output/log.txt
```

### 步骤3：运行模拟

```bash
./ChipletNetworkSim.exe input/multiple_chip_mesh_4x4.ini
```

### 步骤4：查看结果

```bash
cat output/output.csv
```

输出示例：
```csv
injection_rate,latency,receiving_rate
0.000400935,31.2222,0.000400935
```

控制台输出：
```
Internal Hops: 4.51    # 芯粒内平均跳数
Parallel Hops: 0.00    # 并行D2D跳数
Serial Hops: 1.02      # 串行D2D跳数（有跨芯粒通信）
```

---

## 6. 编程接口示例

### Python自定义trace生成

```python
from create_netrace import (
    ChipletTopology,
    NetracePacket,
    NetraceHeader,
    PacketType
)
import bz2

# 1. 创建拓扑
topo = ChipletTopology(k_node=4, k_chip=2)

# 2. 创建数据包列表
packets = []

# 芯粒内通信：Chiplet(0,0)内部
packets.append(NetracePacket(
    cycle=0,
    packet_id=0,
    src=0,   # Chiplet(0,0) Node(0,0)
    dst=5,   # Chiplet(0,0) Node(1,1)
    pkt_type=PacketType.ReadReq,
    addr=0x1000
))

# 芯粒间通信：Chiplet(0,0) → Chiplet(1,0)
packets.append(NetracePacket(
    cycle=10,
    packet_id=1,
    src=0,    # Chiplet(0,0) Node(0,0)
    dst=20,   # Chiplet(1,0) Node(0,2)
    pkt_type=PacketType.WriteReq,
    addr=0x2000
))

# 验证通信类型
print(f"Is intra-chiplet? {topo.is_intra_chiplet(0, 5)}")    # True
print(f"Is intra-chiplet? {topo.is_intra_chiplet(0, 20)}")   # False

# 3. 创建header
header = NetraceHeader(
    benchmark_name="my_custom_trace",
    num_nodes=64,
    num_cycles=1000,
    num_packets=len(packets),
    notes="Custom trace with mixed traffic"
)

# 4. 写入文件
temp_file = "my_trace.tra"
with open(temp_file, 'wb') as f:
    f.write(header.to_bytes())
    for p in packets:
        f.write(p.to_bytes())

# 5. 压缩
with open(temp_file, 'rb') as f_in:
    with bz2.open('my_trace.tra.bz2', 'wb') as f_out:
        f_out.write(f_in.read())

print("Custom trace created successfully!")
```

---

## 7. 常见问题与解答

### Q1: 如何确定数据包是芯粒内还是芯粒间通信？

**A:** 使用辅助函数检查：

```python
topo = ChipletTopology(k_node=4, k_chip=2)

# 方法1：使用is_intra_chiplet
if topo.is_intra_chiplet(src=0, dst=5):
    print("芯粒内通信")
else:
    print("芯粒间通信")

# 方法2：比较芯粒ID
chip_src = topo.get_chiplet_id(0)
chip_dst = topo.get_chiplet_id(5)
if chip_src == chip_dst:
    print("芯粒内通信")
```

### Q2: 如何生成特定比例的芯粒间通信？

**A:** 控制源和目标节点的选择：

```python
import random

def generate_mixed_traffic(num_packets, inter_ratio=0.3):
    """
    生成混合流量
    inter_ratio: 芯粒间通信比例（0.0-1.0）
    """
    topo = ChipletTopology(k_node=4, k_chip=2)
    packets = []

    for i in range(num_packets):
        if random.random() < inter_ratio:
            # 芯粒间：从不同chiplet选择
            src = random.choice(range(0, 16))      # Chiplet 0
            dst = random.choice(range(16, 32))     # Chiplet 1
        else:
            # 芯粒内：从同一chiplet选择
            chiplet_base = random.choice([0, 16, 32, 48])
            src = chiplet_base + random.randint(0, 15)
            dst = chiplet_base + random.randint(0, 15)
            while src == dst:
                dst = chiplet_base + random.randint(0, 15)

        packets.append(NetracePacket(
            cycle=i*5,
            packet_id=i,
            src=src,
            dst=dst,
            pkt_type=PacketType.ReadReq
        ))

    return packets
```

### Q3: 节点数和配置不匹配会怎样？

**A:** 模拟器会出错或产生错误结果。**必须确保**：

```
trace中的num_nodes = k_node² × k_chip²

例如：
k_node=4, k_chip=2 → num_nodes必须是64
k_node=8, k_chip=2 → num_nodes必须是256
```

### Q4: 如何调试trace文件？

**A:** 使用解压工具查看：

```bash
# 解压查看
bzip2 -dk input/netrace/custom.tra.bz2

# 查看十六进制
hexdump -C input/netrace/custom.tra | head -50

# 验证magic number
hexdump -C input/netrace/custom.tra | head -1
# 应该看到: 55 54 4a 48 (0x484A5455的小端序)
```

### Q5: 如何增加D2D链路的延迟？

**A:** 修改源代码中的Channel定义：

```cpp
// src/config.h
const Channel off_chip_serial_channel(2, 10);  // 带宽2, 延迟改为10周期
```

或在配置文件中选择不同的D2D接口：

```ini
d2d_IF = off_chip_serial    # 4周期
# vs
d2d_IF = off_chip_parallel  # 2周期
```

---

## 8. 参考资料

### 文档
- 详细分析：`docs/chiplet_network_analysis.md`
- 工具说明：`tools/README.md`

### 代码位置
- Netrace读取：`src/netrace/netrace.c`
- ID映射：`src/topologies/multiple_chip_mesh.h:12-20`
- 延迟定义：`src/config.h:22-37`
- Hop统计：`src/system.cpp:156-167`

### 论文
Feng et al., "Evaluating Chiplet-based Large-Scale Interconnection Networks
via Cycle-Accurate Packet-Parallel Simulation", USENIX ATC 2024

### GitHub
https://github.com/Yinxiao-Feng/chiplet-network-sim

---

## 9. 快速参考

### 命令速查

```bash
# 生成示例trace
python tools/create_netrace.py --type example

# 生成1000包压力测试
python tools/create_netrace.py --type stress --num_packets 1000

# 运行模拟
./ChipletNetworkSim.exe input/multiple_chip_mesh_4x4.ini

# 查看结果
cat output/output.csv
```

### ID速查表（k_node=4, k_chip=2）

| Chiplet | ID范围 | 位置 |
|---------|--------|------|
| (0,0) | 0-15 | 左上 |
| (1,0) | 16-31 | 右上 |
| (0,1) | 32-47 | 左下 |
| (1,1) | 48-63 | 右下 |

### 通信类型示例

```python
# 芯粒内
src=0, dst=5     # Chiplet(0,0)内部
src=16, dst=23   # Chiplet(1,0)内部

# 芯粒间（相邻）
src=0, dst=16    # Chiplet(0,0) → (1,0)
src=0, dst=32    # Chiplet(0,0) → (0,1)

# 芯粒间（对角）
src=0, dst=63    # Chiplet(0,0) → (1,1)
```

---

**祝您使用愉快！如有问题，请参考详细文档或查看源代码注释。**
