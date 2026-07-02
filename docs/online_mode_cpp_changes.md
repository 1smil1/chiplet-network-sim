# 在线模式 C++ 改造说明

## 1. 文档目的

本文说明为了支持在线模式，当前 `chiplet-network-sim` 在 C++ 侧具体做了哪些改动、没有改哪些内容、内部逻辑是否被重构、外部接口怎么变了。

这不是设计文档，而是实现说明文档。

适合用于：

- 给后来维护 C++ 的人快速建立上下文
- 解释“为了做在线模式，我们实际改到了哪里”
- 回答“有没有动 NoC 内核”

---

## 2. 总体原则

本次在线模式改造遵循两个原则。

### 2.1 不重写 NoC 内核

没有去改：

- router 基本行为
- buffer 行为
- flit/packet 在网络中的推进逻辑
- 拥塞、仲裁、链路竞争的核心机制

也就是说，网络内部的时序竞争仍然由原有模拟器负责。

### 2.2 只在“输入侧、调度侧、结果输出侧”加一层在线能力

主要改动集中在：

- 如何加载在线 workload
- 如何在运行时决定何时注入一个包群模板
- 如何把在线结果结构化输出

因此，本次改造的本质不是“改网络”，而是“改 workload 驱动层”。

---

## 3. 新增/修改的核心文件

### 3.1 新增 `src/online_workload.h`

作用：

- 定义在线 workload 调度器的数据结构和接口

当前主要结构：

- `OnlinePacketTemplate`
- `OnlineGroupTemplate`
- `OnlinePhase`
- `OnlinePhaseState`
- `OnlineGroupRunSummary`
- `OnlineInputSummary`
- `OnlineWorkloadScheduler`

这里承接了在线模式全部运行态。

### 3.2 新增 `src/online_workload.cpp`

作用：

- 实现在线 workload 的加载、调度、动态注入、结果输出

核心能力包括：

- 读取 `manifest.json`
- 读取包群模板 `.bz2`
- phase 级依赖与资源调度
- 在线动态注入
- 空闲时跳时钟
- 在线结果 JSON 输出

### 3.3 修改 `src/main.cpp`

作用：

- 把在线模式接入主程序入口

新增逻辑：

- 当 `traffic == "online_workload"` 时，不再走离线 `TrafficManager::netrace(...)`
- 改为创建 `OnlineWorkloadScheduler`
- 在主循环中调用：
  - `ProcessFinishedPackets`
  - `ReclaimCompletedPackets`
  - `StartNewComputePhases`
  - `PromoteFinishedComputePhases`
  - `InjectReadyPackets`
  - `FastForwardIfIdle`
- 在线结束后写 JSON 结果

同时也给离线模式补了 JSON 输出。

### 3.4 之前已改的 `src/config.h / src/config.cpp`

之前为了接在线模式已经加了这些配置：

- `traffic = online_workload`
- `Files.workload_file`
- `Simulation.pause_on_first_injection`
- `Simulation.pause_on_input_done`
- `Simulation.interactive_pause`

这次没有推翻，而是继续沿用。

### 3.5 之前已改的 `src/packet.h / src/packet.cpp`

之前为了让在线模式能在 packet 完成后回溯归属，已经给 `Packet` 增加了元信息字段：

- `input_id_`
- `phase_id_`
- `task_id_`
- `template_packet_id_`
- `completion_recorded_`

这次继续沿用。

注意：

- 这里的 `task_id_` 在当前在线实现中已经不再表示静态 `group_id`
- 而是运行时的 `run_id`

这是因为同一个 `.bz2` 模板可能被多个 phase 重复使用，必须给每次运行一个唯一实例 ID。

当前 shared-prefix 模式又增加了：

- `shared_follower_count_`

该字段只在在线模式使用。若一个 packet 是某个 `shared_key` 的 leader，它记录同一 group run 内被它代表的 follower 数量；packet 到达时，调度器按 `1 + shared_follower_count_` 扣减逻辑完成计数。

### 3.6 新增 `tools/test_online_dynamic_injection.py`

作用：

- 生成最小在线样例
- 直接写 `.bz2` 包群模板
- 写 `manifest.json`
- 写 `.ini`
- 调用 `ChipletNetworkSim.exe`
- 打印 stdout 和结果 JSON

这个脚本是当前在线模式的最小回归用例。

---

## 4. 在线模式之前是什么样

在这次改造前，在线模式最初是一个过渡版本：

- Python 直接提供 packet 级 JSON
- JSON 里有：
  - `phases`
  - `comm_tasks`
  - `packet_templates`
- C++ 直接从 JSON 读 packet 模板

这个版本能跑，但和离线 `.bz2` 体系割裂较大。

你提出的新目标是：

- 在线模式尽量复用原来离线模式的 `.bz2`
- 让 Python 侧输入尽量简单

所以这次改造把在线主方案从：

- `packet JSON`

切换成了：

- `.bz2 包群模板 + manifest 调度表`

同时保留了旧 JSON loader 的兼容入口，主要是为了调试和过渡，不作为主方案推荐。

---

## 5. 在线模式现在怎么工作

### 5.1 输入层

在线模式现在的主输入是两层。

#### 第一层：`manifest.json`

描述：

- 有哪些包群模板
- 有哪些 phase
- phase 的 compute latency
- phase 的依赖
- phase 占哪些资源
- phase 引用哪个包群模板

#### 第二层：若干个 `.bz2`

每个 `.bz2` 是一个包群模板。

它只负责回答：

- 这批同时注入的 packet 有哪些
- 各自的 `src/dst/size` 是什么

如果旁边存在同名 `.json` sidecar，C++ 会读取 `metadata.shared_prefix_policy` 和每个 packet 的 `shared_key`。`shared_key` 是 group-template 局部编号，`-1` 表示不共享。非负 key 只在同一个 group run 内比较，不跨不同 `.bz2`、不同 `group_id` 比较。

`shared_prefix_policy` 的含义：

- `0`: 不共享，所有逻辑 packet 都正常注入
- `1`: leader-collapse，只注入同 key 的第一个 packet；leader 到达后 followers 一并逻辑完成
- `2`: boundary fanout，只共享源端到目标芯粒边界的 prefix；prefix 到达后，从边界注入 fanout packet 到每个真实 dst，使目标芯粒内部竞争仍被建模

结果 JSON 中每个 group run 会输出：

- `packet_count`: 原始逻辑 packet 数
- `physical_packet_count`: 实际注入 NoC 的 packet 数
- `shared_follower_count`: 被 leader 代表、没有实际注入的 follower 数
- `boundary_fanout_packet_count`: policy 2 下从目标芯粒边界展开注入的 fanout packet 数

它不再负责回答：

- 什么时候注入

真正的注入时刻由 C++ 在线调度决定。

---

### 5.2 加载层

当前在线调度器 `LoadFromFile(...)` 的逻辑是：

1. 读取 `workload_file`
2. 若发现有 `template_groups`
   - 走新的 `manifest + .bz2` 路径
3. 否则
   - 走旧的 `legacy packet JSON` 兼容路径

新的主路径中：

- `LoadFromManifest(...)` 负责读 phase 和 group 关系
- `LoadGroupTemplate(...)` 负责用现有 netrace 解析器读取每个 `.bz2`

这里明确复用了原有 netrace 解析逻辑，没有自己重新发明 `.bz2` 解析器。

---

### 5.3 调度层

在线调度器维护 phase 状态机。

当前 phase 状态有：

- `waiting`
- `computing`
- `ready_to_inject`
- `communicating`
- `done`

主循环顺序是：

1. 处理本拍已完成 packet
2. 回收已完成 packet 的内存
3. 启动满足依赖和资源条件的 compute phase
4. 把 compute 已完成的 phase 提升到 `ready_to_inject`
5. 为 ready phase 注入对应的包群模板
6. 若当前无包、也无新注入，则尝试跳到最近未来事件
7. 否则跑一拍网络

这个循环完全在 C++ 内部完成。

---

## 6. `.bz2` 模板在 C++ 里是怎么用的

### 6.1 不是直接把 `.bz2` 当完整 trace 跑

在线模式不会像离线模式那样：

- 直接把 `.bz2` 按绝对 `cycle` 喂给 `TrafficManager::netrace(...)`

因为在线模式需要动态决定注入时刻。

### 6.2 当前做法

当前做法是：

1. 启动时读取 `.bz2`
2. 只把其中 packet 内容缓存成只读模板
3. 当某个 phase 真正 ready 时：
   - clone 出一批新的运行态 packet
   - 给它们写入：
     - `input_id_`
     - `phase_id_`
     - `task_id_ = run_id`
     - `template_packet_id_`
   - 然后推入原模拟器的 `all_packets`

关键点：

- 不会修改磁盘上的 `.bz2`
- 不会把模板 packet 本体重复复用进运行态
- 每次注入都新建运行态 packet

这就是为什么同一个 `.bz2` 模板可以被多个 phase/input 安全复用。

---

## 7. 是否修改了模拟器内部逻辑

### 7.1 没有改 NoC 内部推进逻辑

没有改这些核心逻辑：

- `System::update(...)`
- buffer 占用 / 释放
- switch allocation
- link traversal
- packet 在网络中的竞争和拥塞行为

也就是说：

- 在线模式的真实拥塞
- 新包与旧包在网络中竞争

仍然是靠原有模拟器自然形成，不是手工建模的。

### 7.2 改了 workload 驱动主循环

为了支持在线调度，`main.cpp` 里新增了一条在线模式专用主循环。

这意味着变的是：

- 什么时候生成 packet
- 什么时候把 packet 放进网络
- 什么时候认为某个 phase 完成

不是变：

- packet 一旦进网后如何走

### 7.3 增加了完成事件的归因逻辑

在线模式为了知道：

- 哪个包群完成了
- 哪个 phase 完成了
- 哪个 input 全部通信完成了

需要在 packet 到达后做归因。

所以这次在在线调度层新增了：

- 运行态 `run_id`
- `run_id -> group run summary`
- packet 完成后按 `task_id_` 回查所属运行实例

这属于“外层调度归因逻辑”，不是网络内核逻辑。

---

## 8. 是否修改了离线模式

### 8.1 离线 netrace 主行为未改

离线模式仍然是：

- `traffic = netrace`
- `TrafficManager` 打开一个完整 `.bz2`
- 按文件里已有绝对 `cycle` 注入
- 跑完整个 trace

所以离线原有行为没有被在线模式替换。

### 8.2 给离线模式补了 JSON 输出

当前新增的是：

- 离线结束后自动写一个结构化 JSON

包含：

- `mode = offline`
- `task_completion_cycle`
- `average_latency`
- `message_arrived`
- `slowest_packet`
- `hop_summary`

这不会改变离线仿真结果，只是增加结果导出形式。

---

## 9. 在线模式的结果输出改了什么

### 9.1 之前

之前主要只有：

- stdout 打印
- 传统 csv

### 9.2 现在

在线模式新增 JSON 输出，当前包含：

- `mode`
- `final_cycle`
- `inputs`
  - `input_id`
  - `first_injection_cycle`
  - `all_comm_done_cycle`
- `groups`
  - `run_id`
  - `group_id`
  - `phase_id`
  - `input_id`
  - `template_bz2`
  - `inject_cycle`
  - `finish_cycle`
  - `packet_count`
- `message_arrived`
- `average_latency`

其中最重要的是：

- `groups[].finish_cycle`

这是“一个包群最后一个包完成的真实 cycle”。

---

## 10. 当前实现中几个重要的语义选择

### 10.1 一个 `.bz2` 只表示一个包群模板

这是当前最简单、最稳定的约定。

没有在一个 `.bz2` 内再拆多个 group。

### 10.2 一个 phase 当前只绑定一个 `group_id`

当前 phase 定义是：

- 一段 compute
- 后接一组通信模板注入

若将来一个 phase 要分裂成多批通信，需要扩展接口。

### 10.3 资源在 phase 通信完成前不释放

当前采用保守语义：

- phase 开始 compute 时占用资源
- 直到 phase 的 group 完成才释放

这样实现简单，但语义偏严格。

### 10.4 在线完成时刻按真实 packet 到达算

在线 group 的 `finish_cycle` 不是：

- 注入时刻
- 第一包到达时刻

而是：

- 该 group 最后一个 packet 的到达 cycle

这和你要求的包群口径一致。

---

## 11. 当前没有做的事

这次在线模式没有做下面这些功能。

### 11.1 没有做 save/load

没有引入网络状态 checkpoint。

### 11.2 没有做运行时 append 新任务

当前是：

- 一次加载完整 workload
- C++ 自己跑完

不是：

- 中途暂停再由 Python 增量塞新 group

### 11.3 没有做真正交互式 pause/resume

当前 pause 是：

- 到某些事件点打印信息

不是：

- 卡住等待用户命令再继续

### 11.4 没有改 packet 二进制格式

`.bz2` 仍然是原 netrace 格式。

没有发明新的二进制模板格式。

这点非常重要，因为它保证了在线模式尽量复用了离线生态。

---

## 12. 当前实现的实际效果

用最小样例验证过：

- 两个 input
- 两个 `.bz2` 模板
- 两个模板被两个 input 复用
- 共享同一组 resource

结果表明：

- `input0` 会先注入
- `input1` 因资源冲突延后
- 同一模板可重复使用
- 每个包群都有独立的 `inject_cycle` 和 `finish_cycle`
- 离线路径仍然可跑

这说明当前改造已经满足了最初目标：

- 在线模式尽量复用离线 `.bz2`
- 差异主要收敛在一个小 `manifest`
- 不必重写 NoC 内核

---

## 13. 一句话总结

这次在线模式改造，本质上只做了三件事：

1. 把在线输入从“packet JSON”收敛成“`.bz2` 包群模板 + manifest”
2. 在 C++ 外层新增了一个在线 phase 调度器
3. 增加了在线/离线统一的 JSON 结果输出

没有重构 NoC 内核，没有修改 `.bz2` 二进制格式，没有破坏离线 `netrace` 主路径。
