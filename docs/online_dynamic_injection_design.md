# 在线动态注入版 `chiplet-network-sim` 设计

## 1. 目标与边界

本文定义一个最小可实现方案，用于支持：

- 多 `input` 在线交叠执行
- compute 和 communication 联合调度
- 共享 tile 资源约束
- 共享网络中的真实拥塞与竞争
- 调试用暂停、打印和状态观察

本方案的核心约束如下：

- Python 负责重预处理，C++ 负责轻运行时调度
- 所有静态输入一次性交给 C++
- C++ 内部在线决定 phase 的启动、通信注入和完成时刻
- `pause` 仅用于调试打印和状态观察，不承担运行时追加任务
- 无 packet 时不空转长时间 cycle
- 已完成 packet / task / phase / input 的运行时内存应尽快释放

本文**不**讨论以下内容：

- Python 如何从 NeuroSim / `co_final` / CSV / 设计文件生成这些输入
- save/load 或 checkpoint
- 运行时 Python 增量喂任务
- Python 与 C++ 的交互协议细节

因此，这个方案可以理解为：

- 输入方式接近“路线 3”：所有静态数据一次性交给 C++
- 运行控制接近“路线 2”：保留暂停、打印、恢复能力

---

## 2. Python 传给 C++ 的最小输入

### 2.1 设计原则

Python 应尽量把复杂信息预处理完，使 C++ 不需要理解以下上层语义：

- compute latency 如何计算
- layer / phase 如何从原始模型抽取
- 资源冲突如何从 tile 映射中推断
- 通信任务如何从原始边或流量矩阵转换出来

C++ 只接收顺序化、可直接构建连续数组的静态数据。

所有时间统一使用 `cycle`。C++ 不负责 `ns -> cycle` 或 `cycle -> ns` 推断。

### 2.2 三类核心输入

推荐固定为 3 类输入：`Phase`、`CommTask`、`PacketTemplate`。

#### `Phase`

表示“某个 input 的某个 layer/phase 的一次计算加后继通信”。

最小字段：

- `phase_id`
- `input_id`
- `layer_id`
- `compute_latency_cycles`
- `dep_phase_ids`
- `resource_ids`
- `comm_task_begin`
- `comm_task_end`

说明：

- `dep_phase_ids` 只表达数据依赖
- `resource_ids` 表达共享 tile / tile-group / compute-core 资源占用
- `comm_task_begin` 和 `comm_task_end` 表示在 `CommTask` 数组中的连续范围，采用左闭右开区间 `[begin, end)`

#### `CommTask`

表示一个 phase 派生出的一个通信任务。

最小字段：

- `task_id`
- `phase_id`
- `packet_begin`
- `packet_end`

说明：

- 一个 phase 可以有 0 个或多个通信任务
- 一个通信任务包含一段连续的 `PacketTemplate`
- 范围采用左闭右开区间 `[begin, end)`

#### `PacketTemplate`

表示运行时可直接展开为 NoC packet 的模板。

最小字段：

- `packet_id`
- `task_id`
- `src_node`
- `dst_node`
- `size_bytes`

说明：

- 这里不预生成绝对 `inject_cycle`
- packet 的实际注入时间由其所属 phase 在运行时 ready 后决定

### 2.3 输入要求

C++ 对输入做最基本的一致性检查，但不负责修正坏输入。

输入必须满足：

- `phase_id`、`task_id`、`packet_id` 全局唯一
- `dep_phase_ids` 无环；推荐 Python 直接输出拓扑有序的 `Phase` 数组
- `comm_task_begin/end` 与 `packet_begin/end` 对应连续可索引范围
- `resource_ids` 使用紧凑整数编号，例如 `[0, num_resources)`
- `compute_latency_cycles >= 0`
- `size_bytes > 0`
- `src_node != dst_node`，若允许相等则 Python 预先过滤或单独标记

### 2.4 推荐加载接口

文档中只定义最小抽象，不绑定最终文件格式：

```cpp
bool load_workload(const WorkloadPayload& payload);
```

其中 `WorkloadPayload` 仅需包含：

- `std::vector<Phase>`
- `std::vector<CommTask>`
- `std::vector<PacketTemplate>`
- `uint32_t num_inputs`
- `uint32_t num_resources`

是否通过 JSON、二进制文件或内存对象传入，不在本文范围内。

---

## 3. C++ 运行态设计

### 3.1 核心状态结构

#### `PhaseState`

运行时每个 phase 的状态。

建议字段：

- `phase_id`
- `input_id`
- `layer_id`
- `status`
- `dep_remaining`
- `remaining_task_count`
- `compute_latency_cycles`
- `compute_start_cycle`
- `compute_end_cycle`
- `comm_start_cycle`
- `comm_end_cycle`
- `comm_task_begin`
- `comm_task_end`
- `resource_ids`

#### `CommTaskState`

运行时每个通信任务的状态。

建议字段：

- `task_id`
- `phase_id`
- `status`
- `remaining_packet_count`
- `packet_begin`
- `packet_end`
- `inject_cycle`
- `finish_cycle`

#### `InputState`

用于统计和调试某个 input 的整体状态。

建议字段：

- `input_id`
- `total_phase_count`
- `done_phase_count`
- `total_task_count`
- `done_task_count`
- `first_injection_seen`
- `all_comm_done_seen`
- `first_injection_cycle`
- `all_comm_done_cycle`

#### `SimulatorState`

全局运行态。

建议字段：

- `current_cycle`
- `resource_release_cycle[]`
- `phase_states[]`
- `task_states[]`
- `input_states[]`
- `inflight_packets`
- `completed_packet_queue`
- `ready_compute_heap`
- `ready_inject_queue`

### 3.2 `Phase` 状态机

每个 `Phase` 采用如下固定状态：

- `waiting`
- `computing`
- `ready_to_inject`
- `communicating`
- `done`

状态转换如下：

1. `waiting -> computing`
   条件：数据依赖满足，且所需资源全部可用
2. `computing -> ready_to_inject`
   条件：`current_cycle >= compute_end_cycle`
3. `ready_to_inject -> communicating`
   条件：对应通信任务首次实际注入 packet
4. `communicating -> done`
   条件：该 phase 的全部通信任务完成

若某个 phase 没有通信任务，则：

- `computing -> done`

并且其 `comm_start_cycle == comm_end_cycle == compute_end_cycle`

### 3.3 关键时间字段

每个 phase 运行时维护：

- `compute_start_cycle`
- `compute_end_cycle`
- `comm_start_cycle`
- `comm_end_cycle`

每个 task 运行时维护：

- `inject_cycle`
- `finish_cycle`

全局资源维护：

- `resource_release_cycle[resource_id]`

资源规则：

- phase 进入 `computing` 时占用 `resource_ids`
- 这些资源持续占用到该 phase 的通信全部完成
- phase 完成时：
  - 对每个 `resource_id`
  - `resource_release_cycle[resource_id] = comm_end_cycle`

这与当前 Python 侧 `co_final` 中“tile 直到 comm_end 才释放”的逻辑保持一致。

---

## 4. 主循环设计

### 4.1 固定顺序

主循环应固定为以下顺序：

1. 处理已完成 packet，释放 packet 内存
2. 更新完成的 `CommTask` / `Phase`
3. 解锁可启动 compute 的 phase
4. 将 compute 完成的 phase 转为 `ready_to_inject`
5. 注入当前 cycle 可发出的 packets
6. 运行一拍网络
7. 检查并触发 pause
8. 若网络为空且未来无立即事件，则跳到下一个事件 cycle

### 4.2 伪代码

```cpp
while (!all_inputs_done()) {
    reclaim_completed_packets();
    update_completed_tasks_and_phases();
    start_new_compute_phases();
    promote_finished_compute_to_ready_inject();
    inject_ready_packets_for_current_cycle();
    run_one_network_cycle_if_needed();
    check_pause_events();
    fast_forward_if_network_idle();
}
```

### 4.3 关键步骤说明

#### Step 1: 回收已完成 packet

- 从完成队列中取出 packet
- 从其所属 task 的 `remaining_packet_count` 中减 1
- 立即释放 packet 运行时对象
- 不保留已完成 packet 的大块内存

#### Step 2: 更新完成的 task / phase

- 若某个 task 的 `remaining_packet_count == 0`
  - 标记 task 完成
  - 记录 `finish_cycle`
- 若某个 phase 的全部 task 已完成
  - 标记 phase 完成
  - 记录 `comm_end_cycle`
  - 更新 `resource_release_cycle`
  - 触发依赖该 phase 的后继 phase 的 `dep_remaining`

#### Step 3: 启动可计算的 phase

某个 `waiting` phase 可启动的条件：

- `dep_remaining == 0`
- 所有 `resource_ids` 均满足 `resource_release_cycle[res] <= current_cycle`

启动时：

- `compute_start_cycle = current_cycle`
- `compute_end_cycle = current_cycle + compute_latency_cycles`
- 状态置为 `computing`

#### Step 4: compute 完成转 ready

对于所有 `computing` phase：

- 若 `compute_end_cycle <= current_cycle`
  - 若无通信任务：直接完成
  - 否则转为 `ready_to_inject`

#### Step 5: 注入 packet

对于所有 `ready_to_inject` phase：

- 将其 task 范围内的 packet template 转为真实 packet
- 按当前 `current_cycle` 注入网络
- phase 第一次注入时设置：
  - `comm_start_cycle = current_cycle`
  - 状态变为 `communicating`
- 所属 input 第一次真实注入时更新：
  - `first_injection_seen = true`
  - `first_injection_cycle = current_cycle`

#### Step 6: 网络推进一拍

仅在以下任一条件满足时推进网络：

- 当前存在 in-flight packet
- 当前 cycle 刚注入了新 packet

否则不推进网络，直接进入 pause/fast-forward 检查。

#### Step 7: pause 检查

只定义两类 pause 事件：

- 某个 input 第一次真实注入
- 某个 input 的全部通信完成

#### Step 8: 跳时钟

仅当网络中**没有** in-flight packet 时允许跳时钟。

下一个候选事件取以下最小值：

- 所有 `computing` phase 的最小 `compute_end_cycle`
- 所有未来可启动 compute phase 的最早可启动 cycle
- 所有 `ready_to_inject` phase 的当前 cycle（通常立即注入，不需要未来值）

如果候选事件时间 `next_cycle > current_cycle`，则：

```cpp
current_cycle = next_cycle;
```

禁止在网络非空时跳过中间 cycle。

---

## 5. 暂停与打印

### 5.1 暂停事件

只定义两类暂停：

1. `PauseOnFirstInjection`
   - 某个 `input` 第一次发生真实 packet 注入
2. `PauseOnInputCommDone`
   - 某个 `input` 的全部通信任务完成

pause 的语义是：

- 仿真主循环暂停推进
- 进程常驻
- 内存状态保留
- 不追加任务
- 不修改输入
- 只做打印或导出

恢复后从当前状态继续，不需要 save/load。

### 5.2 最小打印内容

每次 pause 至少输出：

- `current_cycle`
- `input_id`
- `event_type`
- 当前 in-flight packet 数
- 已完成 phase 数 / 总 phase 数
- 已完成 task 数 / 总 task 数
- 当前资源占用摘要
- 当前网络是否为空

推荐额外输出：

- 当前处于 `computing` 的 phase 数
- 当前处于 `communicating` 的 phase 数
- 当前最早未来事件 cycle

### 5.3 推荐 pause 输出格式

推荐统一格式：

```text
[PAUSE] cycle=12345 input=1 event=first_injection
         inflight_packets=42 network_idle=false
         phases_done=18/96 tasks_done=120/640
         active_compute=3 active_comm=5
         busy_resources=64/256
```

---

## 6. 内存释放策略

本方案要求尽快释放无用运行态，避免长仿真下常驻内存持续增长。

### 6.1 packet

- packet 到达并被处理后立即释放
- 不保留其完整路由历史，除非显式打开调试开关

### 6.2 task

- task 的全部 packet 完成后：
  - 标记完成
  - 释放该 task 的临时注入缓存和统计缓存

### 6.3 phase

- phase 完成后：
  - 释放其 ready-to-inject 队列项
  - 释放其临时 packet 展开缓存
  - 仅保留最小结果字段用于统计或 pause 打印

### 6.4 input

- input 的全部 phase 完成后：
  - 标记 input 完成
  - 释放该 input 级别的所有可释放运行态
  - 仅保留最终摘要字段

### 6.5 验收目标

- 已完成 input 的数量增加时，常驻内存不应线性累计上升
- 长序列运行时，内存主要由当前 in-flight packet 和当前活跃 phase 决定

---

## 7. 实现约束与失败策略

### 7.1 C++ 不负责的工作

C++ 不负责：

- 修复错误依赖图
- 推断资源冲突
- 重新排序坏输入
- 修正越界 range
- 自动补齐缺失字段

### 7.2 建议的一致性检查

加载阶段只做快速失败检查：

- ID 唯一性
- range 合法性
- `phase_id -> task_id -> packet_id` 归属一致
- `dep_phase_ids` 引用存在
- `resource_ids` 编号合法
- `compute_latency_cycles` 非负

任一失败则：

- 输出错误原因
- 拒绝启动仿真

---

## 8. 测试与验收

### 8.1 必测场景

1. 单 `input` 串行层
2. 单 `input` 带并行依赖分支
3. 多 `input` 共享同一组 tile 资源
4. 第一个 `input` 通信未结束时第二个 `input` 开始注入
5. 长空窗场景：网络空闲后直接跳到远未来事件
6. 大量 packet 完成后的内存释放与稳定运行
7. pause 仅在两类指定事件触发

### 8.2 验收标准

必须满足：

- 无 packet 时不空转长时间 cycle
- 新注入 packet 能与旧 in-flight packet 正确竞争
- phase 完成时间由真实通信完成驱动，而不是静态近似
- 已完成 input 的运行时内存被及时回收
- pause 触发点与事件定义严格一致

---

## 9. 默认假设

- Python 已经把 compute latency、数据依赖、资源依赖、通信包模板全部预处理好
- C++ 输入时间单位统一为 `cycle`
- 第一版不支持运行时追加新任务
- 第一版 `pause` 仅用于调试输出与状态观察
- 第一版优先采用“简单顺序数组 + ID range”组织输入
- 资源占用持续到 phase 的通信完成，不在 compute 完成时提前释放

---

## 10. 结论

本方案的关键不是让 C++ 理解更多上层模型语义，而是让 Python 在运行前完成尽可能多的预处理，使 C++ 只做三件事：

- 根据依赖和资源约束启动 phase
- 在 phase ready 时将 packet 注入网络
- 在网络运行过程中依据真实通信完成时间推进后继 phase

在这个分工下：

- C++ 输入简单
- 状态机清晰
- pause 可用于调试
- 无 packet 时可以跳时钟
- 已完成对象可及时释放内存

这满足在线动态注入版 `chiplet-network-sim` 的最小可实现目标。
