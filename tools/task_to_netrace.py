#!/usr/bin/env python3
"""
高层次任务描述转Netrace工具
将用户友好的YAML任务描述转换为netrace格式

支持16节点场景（k_node=2, k_chip=2）

作者: Claude Code
日期: 2024
"""

import argparse
import yaml
import os
import sys
from typing import List, Dict, Tuple

# 导入现有的netrace类
from create_netrace import (
    ChipletTopology, NetraceHeader, NetracePacket, PacketType
)
import bz2


class NodeIDMapper(ChipletTopology):
    """
    节点ID映射器
    扩展ChipletTopology类，添加16节点专用功能
    """
    def __init__(self, k_node: int = 2, k_chip: int = 2):
        super().__init__(k_node, k_chip)

        # 验证16节点配置
        if self.num_nodes != 16:
            print(f"警告: 当前配置生成{self.num_nodes}个节点，不是16节点")

    def validate_node_id(self, node_id: int) -> bool:
        """验证节点ID是否在有效范围内"""
        return 0 <= node_id < self.num_nodes

    def print_layout(self):
        """打印16节点布局"""
        print("\n16节点布局 (k_node=2, k_chip=2):")
        print("="*60)
        print("全局视图:")
        print("     x=0  x=1 | x=2  x=3")
        for y in range(4):
            row_str = f"y={y}:"
            for x in range(4):
                node_id = y * 4 + x
                row_str += f" {node_id:3d} "
                if x == 1:
                    row_str += "|"
            print(row_str)
            if y == 1:
                print("     " + "─"*54)

        print("\n芯粒视图:")
        print("┌─────────────┬─────────────┐")
        print("│ Chiplet(0,0)│ Chiplet(1,0)│")
        print("│   0    1    │   4    5    │")
        print("│   2    3    │   6    7    │")
        print("├─────────────┼─────────────┤")
        print("│ Chiplet(0,1)│ Chiplet(1,1)│")
        print("│   8    9    │  12   13    │")
        print("│  10   11    │  14   15    │")
        print("└─────────────┴─────────────┘")
        print("="*60 + "\n")


class TaskDescriptionLoader:
    """加载和解析YAML任务描述"""

    @staticmethod
    def load_yaml(file_path: str) -> dict:
        """加载YAML文件"""
        try:
            with open(file_path, 'r', encoding='utf-8') as f:
                data = yaml.safe_load(f)
            return data
        except FileNotFoundError:
            print(f"错误: 找不到文件 {file_path}")
            sys.exit(1)
        except yaml.YAMLError as e:
            print(f"错误: YAML解析失败: {e}")
            sys.exit(1)

    @staticmethod
    def validate_format(data: dict) -> bool:
        """验证YAML格式"""
        if 'topology' not in data:
            print("错误: 缺少 'topology' 字段")
            return False

        if 'tasks' not in data:
            print("错误: 缺少 'tasks' 字段")
            return False

        topo = data['topology']
        if 'k_node' not in topo or 'k_chip' not in topo:
            print("错误: topology必须包含 k_node 和 k_chip")
            return False

        return True


class CommunicationAnalyzer:
    """通信模式分析"""

    def __init__(self, mapper: NodeIDMapper):
        self.mapper = mapper

    def analyze_task(self, src: int, dst: int) -> dict:
        """分析单个任务的通信特征"""
        is_intra = self.mapper.is_intra_chiplet(src, dst)
        src_coords = self.mapper.id_to_coords(src)
        dst_coords = self.mapper.id_to_coords(dst)

        # 计算路径信息
        path_info = self.calculate_path(src_coords, dst_coords)

        return {
            'type': 'intra_chiplet' if is_intra else 'inter_chiplet',
            'src_chiplet': self.mapper.get_chiplet_id(src),
            'dst_chiplet': self.mapper.get_chiplet_id(dst),
            'src_coords': src_coords,
            'dst_coords': dst_coords,
            'path': path_info,
            'estimated_hops': path_info['total_hops'],
            'inter_chip_hops': path_info['inter_chip_hops']
        }

    def calculate_path(self, src_coords: Tuple, dst_coords: Tuple) -> dict:
        """
        计算路径信息（基于XY路由）
        返回预估的跳数和路径类型
        """
        src_cx, src_cy, src_nx, src_ny = src_coords
        dst_cx, dst_cy, dst_nx, dst_ny = dst_coords

        # 计算全局坐标
        src_x = src_cx * self.mapper.k_node + src_nx
        src_y = src_cy * self.mapper.k_node + src_ny
        dst_x = dst_cx * self.mapper.k_node + dst_nx
        dst_y = dst_cy * self.mapper.k_node + dst_ny

        # 计算距离
        hops_x = abs(dst_x - src_x)
        hops_y = abs(dst_y - src_y)
        total_hops = hops_x + hops_y

        # 计算跨芯粒边界次数
        inter_chip_hops = 0
        if src_cx != dst_cx:
            inter_chip_hops += 1  # X方向跨芯粒
        if src_cy != dst_cy:
            inter_chip_hops += 1  # Y方向跨芯粒

        return {
            'total_hops': total_hops,
            'hops_x': hops_x,
            'hops_y': hops_y,
            'inter_chip_hops': inter_chip_hops,
            'intra_chip_hops': total_hops - inter_chip_hops
        }


class NetraceGenerator:
    """生成netrace文件"""

    def __init__(self, mapper: NodeIDMapper):
        self.mapper = mapper

    def parse_node_spec(self, spec) -> int:
        """
        解析节点规范，支持多种格式：
        - 直接ID: 5
        - 字典: {node_id: 5}
        - 字典: {chiplet: [0, 0], node: [1, 1]}
        """
        if isinstance(spec, int):
            return spec

        if isinstance(spec, dict):
            if 'node_id' in spec:
                return spec['node_id']

            if 'chiplet' in spec and 'node' in spec:
                chip_x, chip_y = spec['chiplet']
                node_x, node_y = spec['node']
                return self.mapper.coords_to_id(chip_x, chip_y, node_x, node_y)

        raise ValueError(f"无效的节点规范: {spec}")

    def generate_from_tasks(self, tasks: List[dict], output_file: str,
                           benchmark_name: str = "user_scenario"):
        """从任务列表生成netrace"""
        packets = []

        for i, task in enumerate(tasks):
            # 解析节点ID（支持简化格式）
            if 'src' in task:
                src = task['src']
            elif 'source' in task:
                src = self.parse_node_spec(task['source'])
            else:
                raise ValueError(f"任务{i}: 缺少source或src字段")

            if 'dst' in task:
                dst = task['dst']
            elif 'destination' in task:
                dst = self.parse_node_spec(task['destination'])
            else:
                raise ValueError(f"任务{i}: 缺少destination或dst字段")

            # 验证节点ID
            if not self.mapper.validate_node_id(src):
                raise ValueError(f"任务{i}: 源节点ID {src} 超出范围 (0-{self.mapper.num_nodes-1})")

            if not self.mapper.validate_node_id(dst):
                raise ValueError(f"任务{i}: 目标节点ID {dst} 超出范围 (0-{self.mapper.num_nodes-1})")

            # 获取其他参数
            cycle = task.get('cycle', task.get('start_cycle', i * 10))
            size = task.get('size', 64)
            pkt_type_str = task.get('packet_type', 'ReadReq')
            addr = task.get('address', task.get('addr', 0x1000 + i * 64))

            # 转换packet type
            pkt_type = getattr(PacketType, pkt_type_str, PacketType.ReadReq)

            # 创建packet
            packet = NetracePacket(
                cycle=cycle,
                packet_id=i,
                src=src,
                dst=dst,
                pkt_type=pkt_type,
                addr=addr
            )
            packets.append(packet)

        # 计算总周期数
        max_cycle = max(p.cycle for p in packets) if packets else 1000
        num_cycles = max_cycle + 1000

        # 创建header
        header = NetraceHeader(
            benchmark_name=benchmark_name[:30],
            num_nodes=self.mapper.num_nodes,
            num_cycles=num_cycles,
            num_packets=len(packets),
            notes=f"User-defined 16-node scenario with {len(packets)} tasks"
        )

        # 写入文件
        self.write_netrace(header, packets, output_file)

    def write_netrace(self, header: NetraceHeader, packets: List[NetracePacket],
                     output_file: str):
        """写入netrace文件（带压缩）"""
        # 写入临时未压缩文件
        temp_file = output_file.replace('.bz2', '') + '.tmp'

        with open(temp_file, 'wb') as f:
            f.write(header.to_bytes())
            for packet in packets:
                f.write(packet.to_bytes())

        # 压缩为bz2
        with open(temp_file, 'rb') as f_in:
            with bz2.open(output_file, 'wb') as f_out:
                f_out.write(f_in.read())

        # 删除临时文件
        os.remove(temp_file)


def main():
    parser = argparse.ArgumentParser(
        description='将高层次YAML任务描述转换为netrace文件'
    )
    parser.add_argument('input', help='输入YAML文件')
    parser.add_argument('-o', '--output', default='../input/netrace/16node_scenario.tra.bz2',
                       help='输出netrace文件路径')
    parser.add_argument('--analyze', action='store_true',
                       help='显示通信分析')
    parser.add_argument('--validate-only', action='store_true',
                       help='仅验证YAML格式，不生成文件')
    parser.add_argument('--show-layout', action='store_true',
                       help='显示16节点布局')

    args = parser.parse_args()

    # 1. 加载YAML
    print(f"[1/5] 加载任务描述: {args.input}")
    loader = TaskDescriptionLoader()
    data = loader.load_yaml(args.input)

    # 2. 验证格式
    print(f"[2/5] 验证格式")
    if not loader.validate_format(data):
        sys.exit(1)

    # 3. 创建mapper
    topo = data['topology']
    k_node = topo['k_node']
    k_chip = topo['k_chip']

    mapper = NodeIDMapper(k_node, k_chip)

    if args.show_layout:
        mapper.print_layout()

    print(f"  拓扑配置: {k_chip}x{k_chip} chiplets, {k_node}x{k_node} nodes/chiplet")
    print(f"  总节点数: {mapper.num_nodes}")

    if mapper.num_nodes != 16:
        print(f"\n警告: 此工具专为16节点设计 (k_node=2, k_chip=2)")
        print(f"       当前配置生成 {mapper.num_nodes} 节点")

    # 4. 解析和分析任务
    print(f"\n[3/5] 解析任务列表: {len(data['tasks'])} 个任务")

    analyzer = CommunicationAnalyzer(mapper)
    tasks = data['tasks']

    intra_count = 0
    inter_count = 0

    for i, task in enumerate(tasks):
        # 获取src和dst
        if 'src' in task:
            src = task['src']
        elif 'source' in task:
            src = NetraceGenerator(mapper).parse_node_spec(task['source'])
        else:
            print(f"错误: 任务{i} 缺少source或src字段")
            sys.exit(1)

        if 'dst' in task:
            dst = task['dst']
        elif 'destination' in task:
            dst = NetraceGenerator(mapper).parse_node_spec(task['destination'])
        else:
            print(f"错误: 任务{i} 缺少destination或dst字段")
            sys.exit(1)

        # 验证
        if not mapper.validate_node_id(src):
            print(f"错误: 任务{i} 源节点ID {src} 超出范围 (0-{mapper.num_nodes-1})")
            sys.exit(1)

        if not mapper.validate_node_id(dst):
            print(f"错误: 任务{i} 目标节点ID {dst} 超出范围 (0-{mapper.num_nodes-1})")
            sys.exit(1)

        # 分析
        analysis = analyzer.analyze_task(src, dst)

        if analysis['type'] == 'intra_chiplet':
            intra_count += 1
        else:
            inter_count += 1

        if args.analyze:
            task_name = task.get('name', task.get('id', f'task_{i}'))
            print(f"\n任务 {i}: {task_name}")
            print(f"  源: 节点{src} -> Chiplet({analysis['src_coords'][0]},{analysis['src_coords'][1]}) "
                  f"Node({analysis['src_coords'][2]},{analysis['src_coords'][3]})")
            print(f"  目标: 节点{dst} -> Chiplet({analysis['dst_coords'][0]},{analysis['dst_coords'][1]}) "
                  f"Node({analysis['dst_coords'][2]},{analysis['dst_coords'][3]})")
            print(f"  类型: {analysis['type']}")
            print(f"  预估跳数: {analysis['estimated_hops']}")
            print(f"    - 芯粒内: {analysis['path']['intra_chip_hops']}")
            print(f"    - 芯粒间: {analysis['path']['inter_chip_hops']}")

    print(f"\n通信统计:")
    print(f"  芯粒内通信: {intra_count} ({100*intra_count/len(tasks):.1f}%)")
    print(f"  芯粒间通信: {inter_count} ({100*inter_count/len(tasks):.1f}%)")

    if args.validate_only:
        print("\n[OK] 验证通过!")
        return

    # 5. 生成netrace
    print(f"\n[4/5] 生成netrace文件: {args.output}")

    # 确保输出目录存在
    os.makedirs(os.path.dirname(args.output), exist_ok=True)

    generator = NetraceGenerator(mapper)
    benchmark_name = data.get('simulation', {}).get('benchmark_name', '16node_scenario')
    generator.generate_from_tasks(tasks, args.output, benchmark_name)

    file_size = os.path.getsize(args.output)

    print(f"\n{'='*60}")
    print(f"生成完成!")
    print(f"  输出文件: {args.output}")
    print(f"  文件大小: {file_size} bytes")
    print(f"  任务数: {len(tasks)}")
    print(f"  芯粒内通信: {intra_count} ({100*intra_count/len(tasks):.1f}%)")
    print(f"  芯粒间通信: {inter_count} ({100*inter_count/len(tasks):.1f}%)")
    print(f"{'='*60}\n")

    # 6. 提示下一步
    print("[5/5] 下一步操作:")
    print(f"1. 确保配置文件使用16节点配置 (k_node=2, k_chip=2)")
    print(f"2. 在配置文件中设置: netrace_file = {args.output}")
    print(f"3. 运行模拟器: ./ChipletNetworkSim.exe input/multiple_chip_mesh_2x2.ini")
    print(f"4. 查看结果: 检查 output/ 目录中的CSV和日志文件")


if __name__ == '__main__':
    main()
