#!/usr/bin/env python3
"""
Netrace文件生成工具
用于创建自定义的chiplet网络trace文件

作者: 基于chiplet-network-sim项目分析
日期: 2024
"""

import struct
import bz2
import os
from typing import List, Tuple

# Netrace格式常量
NT_MAGIC = 0x484A5455
NT_VERSION = 1.0
NT_BMARK_NAME_LENGTH = 30

# 数据包类型
class PacketType:
    InvalidCmd = 0
    ReadReq = 1
    ReadResp = 2
    ReadRespWithInvalidate = 3
    WriteReq = 4
    WriteResp = 5
    Writeback = 6
    UpgradeReq = 13
    UpgradeResp = 14
    ReadExReq = 15
    ReadExResp = 16

# 节点类型
class NodeType:
    L1D = 0  # L1 Data Cache
    L1I = 1  # L1 Instruction Cache
    L2 = 2   # L2 Cache
    MC = 3   # Memory Controller


class NetraceHeader:
    """Netrace文件头"""
    def __init__(self, benchmark_name: str, num_nodes: int, num_cycles: int,
                 num_packets: int, notes: str = "", num_regions: int = 1):
        self.nt_magic = NT_MAGIC
        self.version = NT_VERSION
        self.benchmark_name = benchmark_name[:NT_BMARK_NAME_LENGTH-1].ljust(NT_BMARK_NAME_LENGTH, '\x00')
        self.num_nodes = num_nodes
        self.num_cycles = num_cycles
        self.num_packets = num_packets
        self.notes = notes
        self.notes_length = len(notes) + 1 if notes else 0
        self.num_regions = num_regions

    def to_bytes(self) -> bytes:
        """转换为二进制格式"""
        # 使用pragma pack(1)对齐，匹配C结构体：magic, version, benchmark_name, num_nodes, pad, ...
        # IMPORTANT: Keep same order as C struct - benchmark_name BEFORE num_nodes
        header_pack = struct.pack(
            '<If30sHBQQII8s',  # < = little endian, H=unsigned short (0-65535)
            self.nt_magic,           # unsigned int (4 bytes)
            self.version,            # float (4 bytes)
            self.benchmark_name.encode('ascii'),  # char[30] (30 bytes)
            self.num_nodes,          # unsigned short (2 bytes, changed from char to support >255 nodes)
            0,                       # unsigned char pad (1 byte)
            self.num_cycles,         # unsigned long long (8 bytes)
            self.num_packets,        # unsigned long long (8 bytes)
            self.notes_length,       # unsigned int (4 bytes)
            self.num_regions,        # unsigned int (4 bytes)
            b'\x00' * 8              # padding (8 bytes)
        )

        result = header_pack

        # 添加notes
        if self.notes:
            result += self.notes.encode('ascii') + b'\x00'

        # 添加region headers（简化版本：单个region）
        if self.num_regions > 0:
            for i in range(self.num_regions):
                region = struct.pack('<QQQ',
                    0,  # seek_offset
                    self.num_cycles,  # num_cycles
                    self.num_packets  # num_packets
                )
                result += region

        return result


class NetracePacket:
    """Netrace数据包"""
    def __init__(self, cycle: int, packet_id: int, src: int, dst: int,
                 pkt_type: int = PacketType.ReadReq, addr: int = 0,
                 src_type: int = NodeType.L1D, dst_type: int = NodeType.L2,
                 dependencies: List[int] = None,
                 custom_size: int = 0):
        self.cycle = cycle
        self.id = packet_id
        self.addr = addr
        self.type = pkt_type
        self.src = src
        self.dst = dst
        self.node_types = (src_type << 4) | dst_type
        self.dependencies = dependencies or []
        self.num_deps = len(self.dependencies)
        self.custom_size = custom_size  # Custom packet size in bytes (0-65535)

    def to_bytes(self) -> bytes:
        """转换为二进制格式"""
        packet_pack = struct.pack(
            '<QIIBxHHBBH',    # Added H for custom_size (unsigned short, 2 bytes)
            self.cycle,       # unsigned long long (8 bytes)
            self.id,          # unsigned int (4 bytes)
            self.addr,        # unsigned int (4 bytes)
            self.type,        # unsigned char (1 byte)
                             # [1 byte padding added by x]
            self.src,         # unsigned short (2 bytes) - CHANGED to support >255 nodes
            self.dst,         # unsigned short (2 bytes) - CHANGED to support >255 nodes
            self.node_types,  # unsigned char (1 byte)
            self.num_deps,    # unsigned char (1 byte)
            self.custom_size  # unsigned short (2 bytes) - Custom packet size
        )

        # 添加依赖关系
        deps_pack = b''
        for dep in self.dependencies:
            deps_pack += struct.pack('<I', dep)

        return packet_pack + deps_pack


class ChipletTopology:
    """芯粒拓扑辅助类"""
    def __init__(self, k_node: int, k_chip: int):
        self.k_node = k_node
        self.k_chip = k_chip
        self.K = k_node * k_chip
        self.num_nodes = k_node * k_node * k_chip * k_chip

    def id_to_coords(self, node_id: int) -> Tuple[int, int, int, int]:
        """
        将节点ID转换为坐标
        返回: (chip_x, chip_y, node_x, node_y)
        """
        x = node_id % self.K
        y = node_id // self.K

        chip_x = x // self.k_node
        chip_y = y // self.k_node
        node_x = x % self.k_node
        node_y = y % self.k_node

        return chip_x, chip_y, node_x, node_y

    def coords_to_id(self, chip_x: int, chip_y: int, node_x: int, node_y: int) -> int:
        """
        将坐标转换为节点ID
        """
        x = chip_x * self.k_node + node_x
        y = chip_y * self.k_node + node_y
        return y * self.K + x

    def is_intra_chiplet(self, src_id: int, dst_id: int) -> bool:
        """判断是否为芯粒内通信"""
        src_chip_x, src_chip_y, _, _ = self.id_to_coords(src_id)
        dst_chip_x, dst_chip_y, _, _ = self.id_to_coords(dst_id)
        return src_chip_x == dst_chip_x and src_chip_y == dst_chip_y

    def get_chiplet_id(self, node_id: int) -> int:
        """获取芯粒ID"""
        chip_x, chip_y, _, _ = self.id_to_coords(node_id)
        return chip_y * self.k_chip + chip_x

    def print_node_info(self, node_id: int):
        """打印节点信息"""
        chip_x, chip_y, node_x, node_y = self.id_to_coords(node_id)
        chiplet_id = self.get_chiplet_id(node_id)
        print(f"Node ID {node_id}: Chiplet({chip_x},{chip_y}) [ID={chiplet_id}], "
              f"Node({node_x},{node_y})")


def create_example_trace(output_file: str, k_node: int = 4, k_chip: int = 2):
    """
    创建示例trace文件
    包含芯粒内和芯粒间通信的混合示例
    """
    topo = ChipletTopology(k_node, k_chip)

    print(f"创建网络拓扑: {k_chip}x{k_chip} chiplets, {k_node}x{k_node} nodes/chiplet")
    print(f"总节点数: {topo.num_nodes}")
    print()

    # 创建数据包列表
    packets = []
    packet_id = 0
    current_cycle = 0

    # 1. 芯粒内通信示例（Chiplet 0,0内部）
    print("=" * 50)
    print("示例1: 芯粒内通信")
    print("=" * 50)
    for i in range(5):
        src = i % 16  # Chiplet(0,0)的前16个节点
        dst = (i + 3) % 16

        topo.print_node_info(src)
        topo.print_node_info(dst)
        print(f"  → 通信类型: {'芯粒内' if topo.is_intra_chiplet(src, dst) else '芯粒间'}")
        print()

        packets.append(NetracePacket(
            cycle=current_cycle + i * 10,
            packet_id=packet_id,
            src=src,
            dst=dst,
            pkt_type=PacketType.ReadReq,
            addr=0x1000 + i * 64
        ))
        packet_id += 1

    # 2. 芯粒间通信示例（跨芯粒）
    print("=" * 50)
    print("示例2: 芯粒间通信")
    print("=" * 50)

    # Chiplet(0,0) -> Chiplet(1,0)
    src = 0   # Chiplet(0,0) Node(0,0)
    dst = 20  # Chiplet(1,0) Node(0,1)
    topo.print_node_info(src)
    topo.print_node_info(dst)
    print(f"  → 通信类型: {'芯粒内' if topo.is_intra_chiplet(src, dst) else '芯粒间'}")
    print()

    packets.append(NetracePacket(
        cycle=current_cycle + 50,
        packet_id=packet_id,
        src=src,
        dst=dst,
        pkt_type=PacketType.ReadReq,
        addr=0x2000
    ))
    packet_id += 1

    # Chiplet(0,0) -> Chiplet(0,1)
    src = 5   # Chiplet(0,0) Node(1,1)
    dst = 37  # Chiplet(0,1) Node(1,0)
    topo.print_node_info(src)
    topo.print_node_info(dst)
    print(f"  → 通信类型: {'芯粒内' if topo.is_intra_chiplet(src, dst) else '芯粒间'}")
    print()

    packets.append(NetracePacket(
        cycle=current_cycle + 60,
        packet_id=packet_id,
        src=src,
        dst=dst,
        pkt_type=PacketType.WriteReq,
        addr=0x3000
    ))
    packet_id += 1

    # 3. 对角线跨芯粒通信
    print("=" * 50)
    print("示例3: 对角线跨芯粒通信")
    print("=" * 50)
    src = 0   # Chiplet(0,0) Node(0,0)
    dst = 63  # Chiplet(1,1) Node(3,3)
    topo.print_node_info(src)
    topo.print_node_info(dst)
    print(f"  → 通信类型: {'芯粒内' if topo.is_intra_chiplet(src, dst) else '芯粒间'}")
    print(f"  → 跨越2个芯粒边界")
    print()

    packets.append(NetracePacket(
        cycle=current_cycle + 70,
        packet_id=packet_id,
        src=src,
        dst=dst,
        pkt_type=PacketType.ReadReq,
        addr=0x4000
    ))
    packet_id += 1

    # 计算统计信息
    num_packets = len(packets)
    num_cycles = max(p.cycle for p in packets) + 100

    # 创建header
    header = NetraceHeader(
        benchmark_name="chiplet_example",
        num_nodes=topo.num_nodes,
        num_cycles=num_cycles,
        num_packets=num_packets,
        notes="Custom chiplet trace: intra and inter-chiplet communication"
    )

    # 写入文件
    print("=" * 50)
    print(f"写入trace文件: {output_file}")
    print(f"  数据包数量: {num_packets}")
    print(f"  仿真周期: {num_cycles}")
    print("=" * 50)

    # 写入未压缩文件
    temp_file = output_file.replace('.bz2', '')
    with open(temp_file, 'wb') as f:
        # 写入header
        f.write(header.to_bytes())

        # 写入所有packets
        for packet in packets:
            f.write(packet.to_bytes())

    # 压缩为bz2
    with open(temp_file, 'rb') as f_in:
        with bz2.open(output_file, 'wb') as f_out:
            f_out.write(f_in.read())

    # 删除临时文件
    os.remove(temp_file)

    print(f"\n[OK] Trace文件创建成功！")
    print(f"  文件位置: {output_file}")
    print(f"  文件大小: {os.path.getsize(output_file)} bytes")


def create_stress_test_trace(output_file: str, k_node: int = 4, k_chip: int = 2,
                             num_packets: int = 1000):
    """
    创建压力测试trace
    生成大量芯粒间通信
    """
    topo = ChipletTopology(k_node, k_chip)

    print(f"创建压力测试trace: {num_packets}个数据包")

    packets = []
    import random
    random.seed(42)

    for i in range(num_packets):
        # 随机选择源和目标
        src = random.randint(0, topo.num_nodes - 1)
        dst = random.randint(0, topo.num_nodes - 1)

        while src == dst:  # 确保不是自己发给自己
            dst = random.randint(0, topo.num_nodes - 1)

        cycle = i * 5  # 每5个周期注入一个包

        packets.append(NetracePacket(
            cycle=cycle,
            packet_id=i,
            src=src,
            dst=dst,
            pkt_type=PacketType.ReadReq if i % 2 == 0 else PacketType.WriteReq,
            addr=0x10000 + i * 64
        ))

    # 统计芯粒内外通信比例
    intra_count = sum(1 for p in packets if topo.is_intra_chiplet(p.src, p.dst))
    inter_count = num_packets - intra_count

    print(f"  芯粒内通信: {intra_count} ({100*intra_count/num_packets:.1f}%)")
    print(f"  芯粒间通信: {inter_count} ({100*inter_count/num_packets:.1f}%)")

    # 创建并写入文件
    num_cycles = max(p.cycle for p in packets) + 1000

    header = NetraceHeader(
        benchmark_name="stress_test",
        num_nodes=topo.num_nodes,
        num_cycles=num_cycles,
        num_packets=num_packets,
        notes="Stress test with random traffic"
    )

    # 写入
    temp_file = output_file.replace('.bz2', '')
    with open(temp_file, 'wb') as f:
        f.write(header.to_bytes())
        for packet in packets:
            f.write(packet.to_bytes())

    with open(temp_file, 'rb') as f_in:
        with bz2.open(output_file, 'wb') as f_out:
            f_out.write(f_in.read())

    os.remove(temp_file)

    print(f"[OK] 压力测试trace创建成功: {output_file}")


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description='Netrace文件生成工具')
    parser.add_argument('--k_node', type=int, default=4, help='每个芯粒的节点数 (默认: 4)')
    parser.add_argument('--k_chip', type=int, default=2, help='芯粒数 (默认: 2)')
    parser.add_argument('--output', type=str, default='../input/netrace/custom.tra.bz2',
                       help='输出文件路径')
    parser.add_argument('--type', choices=['example', 'stress'], default='example',
                       help='trace类型: example(示例) 或 stress(压力测试)')
    parser.add_argument('--num_packets', type=int, default=1000,
                       help='压力测试的数据包数量')

    args = parser.parse_args()

    # 确保输出目录存在
    os.makedirs(os.path.dirname(args.output), exist_ok=True)

    if args.type == 'example':
        create_example_trace(args.output, args.k_node, args.k_chip)
    else:
        create_stress_test_trace(args.output, args.k_node, args.k_chip, args.num_packets)

    print("\n使用方法:")
    print(f"1. 修改配置文件 input/multiple_chip_mesh_4x4.ini")
    print(f"   netrace_file = {args.output}")
    print(f"2. 运行模拟器:")
    print(f"   ./ChipletNetworkSim.exe input/multiple_chip_mesh_4x4.ini")
