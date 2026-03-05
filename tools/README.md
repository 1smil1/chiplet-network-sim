# Netrace文件生成工具使用说明

## 快速开始

### 1. 生成示例trace文件

```bash
cd tools
python create_netrace.py --type example --k_node 4 --k_chip 2
```

这会生成一个包含芯粒内和芯粒间通信示例的trace文件到 `input/netrace/custom.tra.bz2`

### 2. 生成压力测试trace文件

```bash
python create_netrace.py --type stress --k_node 4 --k_chip 2 --num_packets 5000
```

生成包含5000个随机数据包的压力测试文件。

### 3. 自定义输出路径

```bash
python create_netrace.py --type example --output ../input/netrace/my_trace.tra.bz2
```

## 参数说明

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `--k_node` | 每个芯粒的节点行/列数 | 4 |
| `--k_chip` | 芯粒阵列的行/列数 | 2 |
| `--output` | 输出文件路径 | `../input/netrace/custom.tra.bz2` |
| `--type` | trace类型：example或stress | example |
| `--num_packets` | 压力测试的数据包数量 | 1000 |

## 网络拓扑配置

### 示例：2x2芯粒，每个4x4节点

- 总节点数：64（4×4×2×2）
- 芯粒数：4

#### 节点ID分布

```
Chiplet (0,0): IDs 0-15
Chiplet (1,0): IDs 16-31
Chiplet (0,1): IDs 32-47
Chiplet (1,1): IDs 48-63
```

#### 通信类型示例

**芯粒内通信：**
- Node 0 → Node 5（都在Chiplet 0,0内）
- 延迟：~1周期/hop

**芯粒间通信：**
- Node 0 → Node 20（Chiplet 0,0 → Chiplet 1,0）
- 延迟：~2-4周期/hop（取决于d2d_IF配置）

## 使用生成的trace文件

### 1. 修改配置文件

编辑 `input/multiple_chip_mesh_4x4.ini`：

```ini
[Workload]
traffic = netrace

[Files]
netrace_file = ./input/netrace/custom.tra.bz2
```

### 2. 运行模拟

```bash
./ChipletNetworkSim.exe input/multiple_chip_mesh_4x4.ini
```

### 3. 查看结果

```bash
cat output/output.csv
cat output/log.txt
```

## 节点ID计算公式

对于 k_node=4, k_chip=2 的配置：

```python
# 给定节点ID，计算坐标
K = k_node * k_chip  # = 8
x = id % K
y = id // K

chip_x = x // k_node
chip_y = y // k_node
node_x = x % k_node
node_y = y % k_node

# 例子：ID=34
# x=2, y=4
# chip_x=0, chip_y=1
# node_x=2, node_y=0
# → Chiplet(0,1) Node(2,0)
```

## 自定义trace文件

### 导入库并创建拓扑

```python
from create_netrace import ChipletTopology, NetracePacket, NetraceHeader
import bz2

topo = ChipletTopology(k_node=4, k_chip=2)
```

### 创建数据包

```python
packets = []

# 芯粒内通信
packets.append(NetracePacket(
    cycle=0,
    packet_id=0,
    src=0,    # Chiplet(0,0) Node(0,0)
    dst=5,    # Chiplet(0,0) Node(1,1)
    pkt_type=PacketType.ReadReq,
    addr=0x1000
))

# 芯粒间通信
packets.append(NetracePacket(
    cycle=10,
    packet_id=1,
    src=0,    # Chiplet(0,0) Node(0,0)
    dst=20,   # Chiplet(1,0) Node(0,2)
    pkt_type=PacketType.WriteReq,
    addr=0x2000
))
```

### 写入文件

```python
header = NetraceHeader(
    benchmark_name="my_trace",
    num_nodes=64,
    num_cycles=1000,
    num_packets=len(packets)
)

with open('temp.tra', 'wb') as f:
    f.write(header.to_bytes())
    for p in packets:
        f.write(p.to_bytes())

with open('temp.tra', 'rb') as f_in:
    with bz2.open('my_trace.tra.bz2', 'wb') as f_out:
        f_out.write(f_in.read())
```

## 验证trace文件

运行模拟器后，查看输出中的hop统计：

```
Internal Hops: X.XX   ← 芯粒内跳数
Parallel Hops: X.XX   ← 并行D2D跳数
Serial Hops: X.XX     ← 串行D2D跳数
```

- 如果只有芯粒内通信，serial/parallel hops应为0
- 如果有跨芯粒通信，应该能看到serial或parallel hops > 0

## 常见问题

### Q: 如何确保节点ID映射正确？

A: 使用工具函数验证：

```python
from create_netrace import ChipletTopology

topo = ChipletTopology(k_node=4, k_chip=2)
topo.print_node_info(34)  # 输出: Chiplet(0,1) Node(2,0)
```

### Q: 如何生成更多芯粒间通信？

A: 选择来自不同chiplet的节点ID：

```python
# Chiplet 0,0: IDs 0-15
# Chiplet 1,0: IDs 16-31
# Chiplet 0,1: IDs 32-47
# Chiplet 1,1: IDs 48-63

# 跨芯粒通信
src = 0    # Chiplet 0,0
dst = 48   # Chiplet 1,1
```

### Q: 如何调整D2D接口类型？

A: 修改配置文件：

```ini
[Network]
d2d_IF = off_chip_serial    # 4周期延迟，2 flits/cycle
# 或
d2d_IF = off_chip_parallel  # 2周期延迟，1 flit/cycle
```

## 完整文档

详细的分析文档请参阅：`docs/chiplet_network_analysis.md`
