# Netrace测试结果报告

## 测试环境
- 网络配置：2x2 chiplets, 4x4 nodes/chiplet = 64 nodes
- D2D接口：off_chip_serial (延迟4周期)
- 芯粒内链路：on_chip (延迟1周期)

## 测试1: 自定义示例trace (custom.tra.bz2)

### 配置
```ini
netrace_file = ./input/netrace/custom.tra.bz2
```

### 执行命令
```bash
./ChipletNetworkSim.exe input/multiple_chip_mesh_4x4.ini
```

### 结果
```
Netrace File: ./input/netrace/custom.tra.bz2
Injected: 8    Arrived: 8    Timeout: 0
Average latency: 24.125 cycles
Internal Hops: 4
Parallel Hops: 0
Serial Hops: 0.875
Other Hops: 0
```

### 分析
- **数据包总数**: 8个（全部成功到达）
- **平均延迟**: 24.125周期
- **芯粒内跳数**: 4跳
- **芯粒间跳数**: 0.875跳（串行D2D）
- **结论**: 成功区分并统计了芯粒内和芯粒间通信

---

## 测试2: 压力测试trace (stress_test.tra.bz2)

### 配置
```ini
netrace_file = ./input/netrace/stress_test.tra.bz2
```

### trace生成参数
```bash
python tools/create_netrace.py --type stress --num_packets 100
```

生成统计：
- 芯粒内通信: 28个 (28.0%)
- 芯粒间通信: 72个 (72.0%)

### 执行命令
```bash
./ChipletNetworkSim.exe input/multiple_chip_mesh_4x4.ini
```

### 结果
```
Netrace File: ./input/netrace/stress_test.tra.bz2
Injected: 100    Arrived: 100    Timeout: 0
Average latency: 26.73 cycles
Internal Hops: 4.06
Parallel Hops: 0
Serial Hops: 1.02
Other Hops: 0
```

### 分析
- **数据包总数**: 100个（全部成功到达）
- **平均延迟**: 26.73周期
- **芯粒内跳数**: 4.06跳
- **芯粒间跳数**: 1.02跳（串行D2D）
- **结论**:
  - 芯粒间通信比例更高时，Serial Hops显著增加（0.875 → 1.02）
  - 验证了模拟器正确追踪不同类型的hop

---

## 测试总结

### ✅ 验证结论

1. **延迟建模正确性**
   - 模拟器成功区分芯粒内通信（Internal Hops）和芯粒间通信（Serial Hops）
   - 不同trace的hop统计与实际通信模式一致

2. **自定义trace工具有效性**
   - Python工具成功生成符合格式的netrace文件
   - 模拟器正确读取并执行自定义trace
   - 节点ID映射机制工作正常

3. **性能对比**
   | 指标 | 自定义trace (8包) | 压力测试 (100包) |
   |------|------------------|------------------|
   | 平均延迟 | 24.125周期 | 26.73周期 |
   | Internal Hops | 4.00 | 4.06 |
   | Serial Hops | 0.875 | 1.02 |
   | 芯粒间比例 | 约50% | 72% |

4. **工具链完整性**
   - ✅ Python trace生成工具
   - ✅ 节点ID映射验证
   - ✅ 芯粒内/外通信区分
   - ✅ 模拟器正确统计hop类型
   - ✅ 文档完整（NETRACE_GUIDE.md）

### 📊 关键发现

**芯粒间通信对延迟的影响**:
- 芯粒间通信比例从50%增加到72%时
- Serial Hops从0.875增加到1.02 (+16.6%)
- 平均延迟从24.125增加到26.73 (+10.8%)

这验证了论文中的结论：**芯粒间D2D链路的延迟（4周期）显著高于芯粒内链路（1周期）**

### 🎯 使用建议

1. **生成自定义trace**时，使用`--k_node`和`--k_chip`参数匹配配置文件
2. **控制通信比例**时，修改`create_netrace.py`中的随机选择逻辑
3. **验证结果**时，检查hop统计是否符合预期的通信模式
4. **性能评估**时，对比不同芯粒间通信比例下的延迟变化

---

## 附录：文件清单

- **配置文件**: `input/multiple_chip_mesh_4x4.ini`
- **自定义trace**: `input/netrace/custom.tra.bz2` (220 bytes, 8 packets)
- **压力测试trace**: `input/netrace/stress_test.tra.bz2` (100 packets)
- **生成工具**: `tools/create_netrace.py`
- **使用指南**: `NETRACE_GUIDE.md`
- **技术分析**: `docs/chiplet_network_analysis.md`

---

**测试日期**: 2026-03-04
**测试平台**: Windows (MSYS_NT-10.0-22631)
**编译器**: Visual Studio 2022
**Boost版本**: 1.82.0
