# 启用Netrace依赖关系模式实施指南

## 概述

本指南提供了启用netrace依赖关系（dependencies）功能的完整步骤。当前模拟器默认禁用了依赖关系，本修改将使其支持基于数据包依赖的顺序控制。

---

## 快速总结

**修改范围**:
- 3个源文件
- 6个代码位置
- 约30行代码改动

**预期影响**:
- ✅ 支持包级别的依赖关系
- ✅ 允许包B等待包A完成后再注入
- ⚠️ 5-15%的性能开销
- ⚠️ 需要注意死锁风险

---

## 修改清单

### 必需修改（6个）

| # | 文件 | 位置 | 操作 | 难度 |
|---|------|------|------|------|
| 1 | `src/packet.h` | 类定义中 | 添加成员变量 | 简单 |
| 2 | `src/traffic_manager.cpp` | 第19行 | 替换函数调用 | 简单 |
| 3 | `src/traffic_manager.cpp` | 第369行 | 保存指针 | 简单 |
| 4 | `src/traffic_manager.cpp` | 第374行 | 删除free调用 | 简单 |
| 5 | `src/traffic_manager.cpp` | 第358行 | 修改异常处理 | 简单 |
| 6 | `src/system.cpp` | 第211-222行 | 添加依赖清除 | 中等 |

### 推荐修改（2个，提高稳定性）

| # | 文件 | 位置 | 操作 | 目的 |
|---|------|------|------|------|
| 7 | `src/system.cpp` | 第122-123行后 | 超时时清除依赖 | 防止死锁 |
| 8 | `src/traffic_manager.h` | 类定义中 | 添加互斥锁 | 线程安全 |

---

## 详细修改步骤

### 步骤1: 修改 `src/packet.h`

**位置**: 在文件开头添加netrace包含，在Packet类中添加成员

**当前代码**（第1-10行）:
```cpp
#pragma once
#include "buffer.h"

class Packet {
 public:
  Packet(NodeID src, NodeID dst, int length);
  // ...
```

**修改后**:
```cpp
#pragma once
#include "buffer.h"

// 添加netrace头文件引用
extern "C" {
#include "netrace.h"
}

class Packet {
 public:
  Packet(NodeID src, NodeID dst, int length);
  // ...

  // 在finished_成员之后添加（约第33行）
  bool finished_;      // check message whether arrived
  bool releaselink_;   // if the tail of a message shifts
  VCInfo leaving_vc_;

  // 新增：依赖关系支持
  nt_packet_t* netrace_packet_;  // 指向原始netrace包，用于依赖清除
```

---

### 步骤2: 修改 `src/traffic_manager.cpp` - 启用依赖

**位置**: 第16-21行（构造函数中）

**当前代码**:
```cpp
} else if (traffic_ == "netrace") {
    CTX = new nt_context_t();
    nt_open_trfile(CTX, param->netrace_file.c_str());
    nt_disable_dependencies(CTX);  // ← 这行需要修改
    nt_print_trheader(CTX);
}
```

**修改后**:
```cpp
} else if (traffic_ == "netrace") {
    CTX = new nt_context_t();
    nt_open_trfile(CTX, param->netrace_file.c_str());
    nt_init_cleared_packets_list(CTX);  // ← 替换为此行
    nt_print_trheader(CTX);
}
```

**说明**: 从禁用依赖改为初始化依赖跟踪列表。

---

### 步骤3: 修改 `src/traffic_manager.cpp` - 保存netrace包指针

**位置**: 第369行（netrace()函数中）

**当前代码**:
```cpp
if (src != dest) {
    int packet_length = ceil((double)nt_get_packet_size(trace_packet) / 16);
    Packet* packet = new Packet(network->id2nodeid(src),
                                network->id2nodeid(dest),
                                packet_length);
    vecmess.push_back(packet);
    all_message_num_++;
}
```

**修改后**:
```cpp
if (src != dest) {
    int packet_length = ceil((double)nt_get_packet_size(trace_packet) / 16);
    Packet* packet = new Packet(network->id2nodeid(src),
                                network->id2nodeid(dest),
                                packet_length);
    packet->netrace_packet_ = trace_packet;  // ← 新增：保存指针
    vecmess.push_back(packet);
    all_message_num_++;
}
```

---

### 步骤4: 修改 `src/traffic_manager.cpp` - 删除过早的free

**位置**: 第374行（netrace()函数末尾）

**当前代码**:
```cpp
    vecmess.push_back(packet);
    all_message_num_++;
}
nt_packet_free(trace_packet);  // ← 删除这行
```

**修改后**:
```cpp
    vecmess.push_back(packet);
    all_message_num_++;
}
// trace_packet现在由Packet拥有，稍后在完成时清除
```

**说明**: 不再立即释放netrace包，而是在数据包完成时释放。

---

### 步骤5: 修改 `src/traffic_manager.cpp` - 修改无效包处理

**位置**: 第358行（netrace()函数中）

**当前代码**:
```cpp
else if (nt_get_packet_size(trace_packet) == -1) {
    nt_packet_free(trace_packet);
    continue;
}
```

**修改后**:
```cpp
else if (nt_get_packet_size(trace_packet) == -1) {
    nt_clear_dependencies_free_packet(CTX, trace_packet);  // ← 修改
    continue;
}
```

**说明**: 即使包无效被跳过，也需要清除其依赖关系。

---

### 步骤6: 修改 `src/system.cpp` - 在完成时清除依赖（关键！）

**位置**: 第211-222行（update()函数中）

**当前代码**:
```cpp
// 如果最后一个flit到达目的地，删除消息
if (p.link_timer_ == 0 && p.tail_trace().id == p.destination_) {
    VCInfo dest_vc = p.tail_trace();
    dest_vc.buffer->release_buffer(dest_vc.vcb, p.length_);
    p.finished_ = true;
    TM->message_arrived_++;
    TM->total_cycles_ += p.trans_timer_;
    TM->total_parallel_hops_ += p.parallel_hops_;
    TM->total_serial_hops_ += p.serial_hops_;
    TM->total_internal_hops_ += p.internal_hops_;
    TM->total_other_hops_ += p.other_hops_;
    return;
}
```

**修改后**:
```cpp
// 如果最后一个flit到达目的地，删除消息
if (p.link_timer_ == 0 && p.tail_trace().id == p.destination_) {
    VCInfo dest_vc = p.tail_trace();
    dest_vc.buffer->release_buffer(dest_vc.vcb, p.length_);

    // 新增：清除netrace中的依赖关系
    if (TM->traffic_ == "netrace" && p.netrace_packet_ != nullptr) {
        nt_packet_t* pkt_to_free = p.netrace_packet_;
        p.netrace_packet_ = nullptr;  // 防止重复释放
        nt_clear_dependencies_free_packet(TM->CTX, pkt_to_free);
    }

    p.finished_ = true;
    TM->message_arrived_++;
    TM->total_cycles_ += p.trans_timer_;
    TM->total_parallel_hops_ += p.parallel_hops_;
    TM->total_serial_hops_ += p.serial_hops_;
    TM->total_internal_hops_ += p.internal_hops_;
    TM->total_other_hops_ += p.other_hops_;
    return;
}
```

**说明**: 这是最关键的修改，在数据包完成时清除其依赖，使等待它的包可以被注入。

---

### 步骤7（推荐）: 修改 `src/system.cpp` - 超时时清除依赖

**位置**: 第122-123行之后

**添加代码**:
```cpp
if (p.wait_timer_ == timeout_time_) {  // timeout
    TM->message_timeout_++;

    // 新增：清除超时包的依赖，防止死锁
    if (TM->traffic_ == "netrace" && p.netrace_packet_ != nullptr) {
        nt_packet_t* pkt_to_free = p.netrace_packet_;
        p.netrace_packet_ = nullptr;
        nt_clear_dependencies_free_packet(TM->CTX, pkt_to_free);
    }
}
```

**说明**: 防止超时的包阻塞其他包，避免死锁。

---

### 步骤8（可选）: 添加线程安全保护

如果您的配置使用多线程（threads_number > 1），建议添加互斥锁。

**文件**: `src/traffic_manager.h`

**在类定义中添加**:
```cpp
#include <mutex>

class TrafficManager {
  // ...
 private:
  std::mutex netrace_lock_;  // 保护CTX的并发访问
};
```

**在 `src/traffic_manager.cpp` 的 `netrace()` 函数开头添加**:
```cpp
void TrafficManager::netrace(std::vector<Packet*>& vecmess, uint64_t cyc) {
    std::lock_guard<std::mutex> lock(netrace_lock_);  // 加锁
    // ... 其余代码 ...
}
```

---

## 编译和测试

### 编译步骤

```bash
cd D:\python_prj\chiplet-network-sim

# 清理旧构建
rm -rf build

# 创建新构建
mkdir build
cd build

# 配置CMake（Windows + Visual Studio 2022）
cmake .. -G "Visual Studio 17 2022" -A x64

# 编译
cmake --build . --config Release

# 复制可执行文件到项目根目录
cp Release/ChipletNetworkSim.exe ../
```

### 验证编译

检查编译输出，应该**没有**以下错误：
- ❌ `undefined reference to nt_init_cleared_packets_list`
- ❌ `undefined reference to nt_clear_dependencies_free_packet`
- ❌ `'netrace_packet_' was not declared in this scope`

如果编译成功，应该看到：
```
[100%] Built target ChipletNetworkSim
```

---

## 创建测试Trace

### 简单依赖测试

创建 `examples/dependency_test.yaml`:
```yaml
topology:
  k_node: 2
  k_chip: 2

tasks:
  # 任务1：独立任务，立即可注入
  - src: 0
    dst: 5
    cycle: 0
    size: 64
    id: 1
    dependencies: []

  # 任务2：依赖任务1
  - src: 0
    dst: 2
    cycle: 0     # 同样cycle，但因依赖而延迟
    size: 128
    id: 2
    dependencies: [1]  # 等待包1完成

  # 任务3：依赖任务2
  - src: 0
    dst: 8
    cycle: 0
    size: 128
    id: 3
    dependencies: [2]  # 等待包2完成

simulation:
  benchmark_name: "dependency_test"
```

**注意**: 需要修改 `tools/task_to_netrace.py` 以支持dependencies字段！

### 修改task_to_netrace.py以支持dependencies

在 `generate_from_tasks()` 函数中（约第217行）:

```python
# 获取依赖关系
deps = []
if 'dependencies' in task:
    deps = task['dependencies']

# 创建packet
packet = NetracePacket(
    cycle=cycle,
    packet_id=i,
    src=src,
    dst=dst,
    pkt_type=pkt_type,
    addr=addr,
    dependencies=deps  # 添加依赖
)
```

---

## 运行测试

### 测试1：无依赖trace（回归测试）

```bash
# 使用现有的sequential_test
./ChipletNetworkSim.exe input/multiple_chip_mesh_2x2.ini
```

**预期结果**:
- Injected: 4
- Arrived: 4
- 没有崩溃

### 测试2：有依赖trace

```bash
# 先生成带依赖的trace
cd tools
python task_to_netrace.py ../examples/dependency_test.yaml \
    -o ../input/netrace/dependency_test.tra.bz2

# 修改配置
# 编辑 input/multiple_chip_mesh_2x2.ini
# netrace_file =./input/netrace/dependency_test.tra.bz2

# 运行
cd ..
./ChipletNetworkSim.exe input/multiple_chip_mesh_2x2.ini
```

**预期结果**:
- Injected: 3
- Arrived: 3
- 平均延迟应该增加（因为包等待依赖）

### 验证依赖是否生效

添加调试输出验证（在 `traffic_manager.cpp` 的 `netrace()` 开头）:

```cpp
static bool first_call = true;
if (first_call) {
    std::cout << "=== Dependency Tracking Status ===" << std::endl;
    std::cout << "Dependencies disabled: "
              << (CTX->dependencies_off ? "YES (BAD)" : "NO (GOOD)") << std::endl;
    std::cout << "Cleared packets list: "
              << (CTX->track_cleared_packets_list ? "YES (GOOD)" : "NO (BAD)") << std::endl;
    first_call = false;
}
```

**应该看到**:
```
=== Dependency Tracking Status ===
Dependencies disabled: NO (GOOD)
Cleared packets list: YES (GOOD)
```

---

## 故障排除

### 问题1: 编译错误 - undefined reference

**错误信息**:
```
undefined reference to `nt_init_cleared_packets_list'
```

**解决方案**:
检查是否正确链接了netrace库。查看 `CMakeLists.txt` 确保：
```cmake
target_link_libraries(ChipletNetworkSim PRIVATE netrace)
```

### 问题2: 运行时崩溃 - Segmentation Fault

**可能原因**:
- netrace_packet_未初始化为nullptr
- 重复释放netrace包

**解决方案**:
在 `Packet` 构造函数中初始化：
```cpp
Packet::Packet(NodeID src, NodeID dst, int length)
  : source_(src), destination_(dst), length_(length) {
  // ...
  netrace_packet_ = nullptr;  // 添加初始化
}
```

### 问题3: 模拟挂起 - 死锁

**症状**: 模拟器运行一段时间后停止，CPU占用0%

**可能原因**: 依赖循环或孤立包

**排查**:
```bash
# 添加超时限制
# 编辑配置文件，减小simulation_time
# 添加调试输出显示等待的包
```

**解决**: 使用步骤7的超时清除机制

### 问题4: 依赖未生效 - 所有包立即注入

**症状**: 有依赖的包没有等待

**排查**:
1. 检查调试输出，确认dependencies_off=0
2. 检查trace文件是否正确包含依赖信息
3. 验证 `nt_clear_dependencies_free_packet` 是否被调用

---

## 性能对比

### 预期性能影响

| 指标 | 禁用依赖 | 启用依赖 | 变化 |
|------|----------|----------|------|
| 运行时间 | 1.0x | 1.05-1.15x | +5-15% |
| 内存使用 | 基准 | 基准 + 几MB | 轻微增加 |
| 平均延迟 | L | L + delta | 取决于依赖 |
| 包到达率 | ~100% | ~100% | 相同 |

### 测量方法

```bash
# 禁用依赖
time ./ChipletNetworkSim.exe config.ini

# 启用依赖（修改后）
time ./ChipletNetworkSim.exe config.ini

# 对比运行时间
```

---

## 回滚方案

如果修改后出现问题，快速回滚：

### 方法1: Git回滚（如果使用Git）

```bash
git checkout src/packet.h
git checkout src/traffic_manager.cpp
git checkout src/system.cpp
```

### 方法2: 手动回滚

只需要恢复步骤2的修改：

```cpp
// src/traffic_manager.cpp 第19行
- nt_init_cleared_packets_list(CTX);
+ nt_disable_dependencies(CTX);
```

然后重新编译即可恢复原始行为。

---

## 总结

### 修改复杂度: ⭐⭐⭐ (中等)

- ✅ 代码量小（约30行）
- ✅ 改动位置明确
- ⚠️ 需要理解依赖生命周期
- ⚠️ 需要仔细测试

### 推荐实施路径

1. **第一阶段**（30分钟）: 完成步骤1-6的必需修改，编译测试
2. **第二阶段**（20分钟）: 添加步骤7的超时保护
3. **第三阶段**（30分钟）: 修改工具支持dependencies，创建测试trace
4. **第四阶段**（30分钟）: 运行测试，验证功能，性能对比

**总计**: 约2小时

### 关键收益

- ✅ 支持复杂的数据包依赖关系
- ✅ 更真实的应用场景模拟
- ✅ 符合论文中描述的完整功能
- ✅ 为未来扩展打好基础

---

## 参考资料

- Netrace格式文档: `NETRACE_GUIDE.md`
- 源代码: `src/netrace/netrace.c`
- 论文: Feng et al., USENIX ATC 2024
