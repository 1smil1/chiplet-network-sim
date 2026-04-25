# Python 到 C++ 在线模式接口交接文档

## 1. 文档目的

本文交接给下一位负责 Python 侧接入的 agent。目标不是继续改 C++，而是把 Python 侧真正需要准备的输入、文件组织、字段含义、结果消费方式一次讲清楚。

当前 C++ 已经支持两种模式：

- 离线：`traffic = netrace`
- 在线：`traffic = online_workload`

在线模式的主方案已经确定为：

- 一个 `.bz2` 文件表示一个“同时间注入的包群模板”
- 一个 `manifest.json` 描述 phase 级调度信息
- C++ 运行时根据 compute/dependency/resource 关系决定何时把哪个包群模板注入网络

所以 Python 侧的任务是：

1. 生成若干个包群模板 `.bz2`
2. 生成一个在线 `manifest.json`
3. 生成或复用 `.ini`
4. 调用 `ChipletNetworkSim.exe`
5. 解析在线 JSON 结果

---

## 2. Python 最终需要给 C++ 的东西

在线模式最少需要 4 类输入。

### 2.1 `manifest.json`

这是在线调度表，是最核心的入口文件。

当前 C++ 读取的最小格式如下：

```json
{
  "num_inputs": 2,
  "num_resources": 4,
  "template_groups": [
    {
      "group_id": 0,
      "bz2_path": "group_0.bz2"
    },
    {
      "group_id": 1,
      "bz2_path": "group_1.bz2"
    }
  ],
  "phases": [
    {
      "phase_id": 0,
      "input_id": 0,
      "layer_id": 0,
      "compute_latency_cycles": 123,
      "dep_phase_ids": [],
      "resource_ids": [0, 1],
      "group_id": 0
    }
  ]
}
```

字段含义如下。

#### 顶层字段

- `num_inputs`
  - 当前 workload 一共包含多少个 input 实例
  - 例如 `num_inputs = 8` 表示一次在线仿真里有 8 个 input 在共享 compute 和 NoC

- `num_resources`
  - Python 为 compute 资源分配的紧凑编号总数
  - C++ 不理解 tile 语义本身，只理解 `resource_ids`
  - 这些 resource 可以是 tile、tile group、core cluster，具体抽象由 Python 决定，但必须全局一致

#### `template_groups`

每个元素表示一个包群模板定义。

- `group_id`
  - 模板 ID，全局唯一
  - 一个 `group_id` 对应一个 `.bz2`

- `bz2_path`
  - 相对于 `manifest.json` 所在目录的相对路径，推荐这样用
  - C++ 当前会基于 manifest 所在目录做路径拼接

#### `phases`

每个元素表示一个在线调度 phase。

- `phase_id`
  - 全局唯一

- `input_id`
  - 当前 phase 属于哪个 input
  - 取值范围应为 `[0, num_inputs)`

- `layer_id`
  - 方便 Python 侧回溯，C++ 当前只存储，不做业务解释

- `compute_latency_cycles`
  - 该 phase 的纯计算延迟
  - 单位必须已经是 cycle
  - C++ 不做 ns/cycle 换算

- `dep_phase_ids`
  - phase 的数据依赖，只放 phase 级前驱
  - 当前 C++ 使用它决定 `dep_remaining`
  - 必须无环

- `resource_ids`
  - 当前 phase 占用哪些共享 compute 资源
  - 用来表达“input1 的 layer1 计算必须等到 input0 的 layer1 释放 tile”这类约束
  - C++ 当前按“资源独占，直到该 phase 通信全部完成才释放”处理

- `group_id`
  - 当前 phase 计算完成后要注入的包群模板
  - 若该 phase 没有通信，可暂时约定为 `-1`

---

### 2.2 包群模板 `.bz2`

在线模式里，一个 `.bz2` 文件表示一个“同时注入的包群模板”。

这个 `.bz2` 的用途是：

- 存 packet 级内容
- 复用离线模式已有 `.bz2/netrace` 生态
- 让 Python 不再手写 packet JSON

#### `.bz2` 当前需要包含的 packet 信息

C++ 在线模式当前实际使用这些字段：

- `packet.id`
- `packet.src`
- `packet.dst`
- `packet.custom_size`

当前不会使用模板里的绝对 `cycle`。

推荐 Python 生成模板时统一写：

- `cycle = 0`

因为在线模式会在运行时把整个包群的实际注入时刻设置为当前 `inject_cycle`。

#### 一个 `.bz2` 的语义

当前实现固定采用：

- 一个 `.bz2` = 一个 `group_id` = 一个包群模板

第一版不支持：

- 一个 `.bz2` 文件里再拆多个包群

如果将来需要这种能力，应当扩展 manifest，而不是在当前接口上做隐式推断。

#### 模板复用

可以复用。

例如：

- `input0/layer1` 使用 `group_7.bz2`
- `input1/layer1` 也使用 `group_7.bz2`

C++ 当前不会复用运行态 packet 对象，而是：

1. 启动时把 `.bz2` 读成只读模板
2. 每次 phase 真正 ready 时 clone 一批新的 packet 到运行态

所以同一个 `.bz2` 可被多次安全引用。

---

### 2.3 `.ini`

在线模式当前最小 `.ini` 要求如下：

```ini
[Network]
topology = MultiChipMesh
routing_algorithm = XY
k_node = 2
k_chip = 2
buffer_size = 20
vc_number = 2
d2d_IF = off_chip_serial

[Workload]
traffic = online_workload

[Simulation]
threads = 1
pause_on_first_injection = true
pause_on_input_done = true
interactive_pause = false

[Files]
workload_file = input/online_dynamic_test/minimal_online_manifest.json
output_file = ./output/output_online_dynamic.csv
log_file = ./output/log_online_dynamic.txt
```

关键字段如下。

- `traffic = online_workload`
  - 必须，否则不会进入在线调度器

- `workload_file`
  - 指向 `manifest.json`

- `output_file`
  - C++ 当前仍写传统 csv，同时会自动额外写一个同名 `.json`
  - 例如 `output_online_dynamic.csv` 对应 `output_online_dynamic.json`

- `pause_on_first_injection`
  - 是否打印某 input 的首次注入事件

- `pause_on_input_done`
  - 是否打印某 input 的全部通信完成事件

---

### 2.4 `position_file`

在线模式当前不额外发明位置输入。

只要网络拓扑本身仍然走原来的配置路径，离线/在线都继续复用原来已有的节点位置逻辑。

因此 Python 侧不需要为在线模式单独再做一套位置系统。

若现有离线链路本来就依赖：

- `position_file`
- topology 配置
- `k_node/k_chip`

那么在线模式继续按原来方式提供即可。

也就是说，在线模式和离线模式在“网络拓扑 / 节点位置 / 路由配置”层面应保持一致。

---

## 3. Python 侧必须先做好的预处理

当前 C++ 故意保持“轻调度、轻语义”，因此 Python 必须把上层复杂信息提前消化掉。

### 3.1 必须由 Python 决定的内容

- 一共有多少个 input
- 每个 input 拆成哪些 phase
- 每个 phase 的 `compute_latency_cycles`
- phase 间的数据依赖
- compute 资源冲突如何编码成 `resource_ids`
- 每个 phase 对应哪个包群模板 `.bz2`
- 哪些 phase 复用同一个模板

### 3.2 不应交给 C++ 推断的内容

- 从 `co_final` 反推 phase DAG
- 从 tile 映射自动推断资源冲突
- 从原始层通信边直接推断 packet group 划分
- 从 ns 自动换算成 cycle

这些都应该在 Python 里先完成。

---

## 4. Python 侧建议的数据组织方式

推荐 Python 先在内存里整理成下面这套抽象，再落地成 manifest 和 `.bz2`。

### 4.1 `GroupTemplate`

建议 Python 维护：

- `group_id`
- `packets`
  - `src_node`
  - `dst_node`
  - `size_bytes`
  - 可选调试字段

然后把 `GroupTemplate` 落成一个 `.bz2` 模板文件。

### 4.2 `PhaseRecord`

建议 Python 维护：

- `phase_id`
- `input_id`
- `layer_id`
- `compute_latency_cycles`
- `dep_phase_ids`
- `resource_ids`
- `group_id`

然后直接写入 manifest 的 `phases`。

### 4.3 `ResourceId` 设计

最重要的不是 resource 的名字，而是 resource 编码在整个 workload 内必须一致。

例如可以有两种策略。

#### 策略 A：每个 tile 一个 resource_id

优点：

- 最精细
- 约束真实

缺点：

- `resource_ids` 会长

#### 策略 B：每个 tile group / compute cluster 一个 resource_id

优点：

- 简洁

缺点：

- 约束更粗

当前 C++ 不关心你选哪一种，只要求：

- 编号紧凑
- 语义全局一致

---

## 5. 当前 C++ 在线调度语义

Python 侧接入前必须理解当前 C++ 的运行语义，否则会误判时间。

### 5.1 phase 启动条件

一个 phase 只有在以下条件都满足后才能开始 compute：

- 所有 `dep_phase_ids` 已完成
- 所有 `resource_ids` 当前可用

### 5.2 resource 的释放时刻

当前实现采用：

- resource 从 phase compute 开始时占用
- 一直到该 phase 的通信全部完成后才释放

这点很关键。

也就是说，当前不是：

- compute 完成就释放

而是：

- compute + communication 整个 phase done 才释放

如果这不符合未来真实建模，你需要改 Python 约束或后续再改 C++ 语义。

### 5.3 在线注入时刻

当前实现采用：

- `phase.compute_end_cycle <= current_cycle`
- 则该 phase 进入 `ready_to_inject`
- 注入时整组 packet 使用当前 `current_cycle` 作为统一 `inject_cycle`

也就是说，一个 group 内所有 packet 的注入时刻相同。

这正是“一个 `.bz2` 是一个同时间注入的包群”的当前定义。

---

## 6. Python 侧生成文件时的明确要求

### 6.1 manifest 要求

- `phase_id` 全局唯一
- `group_id` 全局唯一
- `input_id` 范围合法
- `dep_phase_ids` 引用的 phase 必须存在
- `group_id` 引用的模板必须存在
- `resource_ids` 必须落在 `[0, num_resources)` 内

### 6.2 `.bz2` 模板要求

- 推荐所有 packet 的 `cycle = 0`
- `src`、`dst` 必须是网络合法节点
- `custom_size > 0`
- 模板文件不能为空

### 6.3 相对路径要求

当前建议：

- `.bz2` 路径写成相对于 manifest 的相对路径

例如：

```json
{
  "group_id": 7,
  "bz2_path": "templates/group_7.bz2"
}
```

C++ 当前会按：

- `manifest.json` 所在目录 + `bz2_path`

来拼路径。

---

## 7. C++ 输出结果，Python 如何消费

在线模式当前会生成一个 JSON 结果文件。

路径规则：

- 若 `.ini` 中 `output_file = ./output/output_online_dynamic.csv`
- 则结果 JSON 为 `./output/output_online_dynamic.json`

### 7.1 在线 JSON 当前字段

示例结构：

```json
{
  "mode": "online",
  "final_cycle": 167,
  "inputs": [
    {
      "input_id": 0,
      "first_injection_cycle": 2,
      "all_comm_done_cycle": 84
    }
  ],
  "groups": [
    {
      "run_id": 0,
      "group_id": 0,
      "phase_id": 0,
      "input_id": 0,
      "template_bz2": "input/online_dynamic_test/group_a.bz2",
      "inject_cycle": 2,
      "finish_cycle": 41,
      "packet_count": 2
    }
  ],
  "message_arrived": 8,
  "average_latency": 36
}
```

### 7.2 字段解释

- `final_cycle`
  - 所有 packet 中最后一个到达包的 cycle

- `inputs[].first_injection_cycle`
  - 某个 input 第一次真正开始发通信包的 cycle

- `inputs[].all_comm_done_cycle`
  - 某个 input 全部 phase 的通信完成时刻

- `groups[].inject_cycle`
  - 一个包群的统一注入时刻

- `groups[].finish_cycle`
  - 这个包群最后一个 packet 的完成时刻
  - 这是在线模式最关键的口径

### 7.3 Python 侧建议怎么用

推荐 Python 侧拿这个 JSON 做：

- 每个 phase 的真实通信完成时间回填
- 每个 input 的 timeline 对齐
- 与 NeuroSim compute timeline 合并
- 生成新的 `co_final` 或其替代结构

---

## 8. 推荐的 Python 侧落地步骤

推荐按下面顺序接入。

### 第一步：先接一个最小样例

目标：

- 不是接全流程
- 而是让 Python 先能生成：
  - 两个 `.bz2`
  - 一个 manifest
  - 一个 `.ini`

然后跑通 `ChipletNetworkSim.exe`

### 第二步：把现有离线包群生成逻辑改成模板生成逻辑

如果你们 Python 现在已经能按 layer/group 生成离线 `.bz2`，那就尽量复用。

只要改成：

- 在线模板 `.bz2` 中统一把 `cycle` 写成 `0`

即可。

### 第三步：从 `neurosim_result_parser.py` 导出 phase 调度表

重点不是导出 packet，而是导出：

- `input_id`
- `layer_id`
- `compute_latency_cycles`
- `dep_phase_ids`
- `resource_ids`
- `group_id`

### 第四步：跑完后读在线 JSON 结果并回填

回填建议至少做：

- `phase_id -> group finish_cycle`
- `input_id -> all_comm_done_cycle`

---

## 9. 当前实现的限制

下一位 agent 在接 Python 前必须知道这些限制。

### 9.1 一个 phase 目前只支持一个 `group_id`

当前 C++ 的 `phases` 结构是：

- 一个 phase 对应一个 `group_id`

如果未来一个 phase 需要分多批通信，则要扩接口。

### 9.2 当前没有运行时追加任务

当前是：

- 一次加载完整 manifest
- C++ 自己在线调度

不是：

- Python 跑一会再 append 新任务

### 9.3 当前 pause 只是打印

当前 `pause_on_first_injection` 和 `pause_on_input_done` 只是打印：

- `[PAUSE] cycle=...`

没有真正的交互式暂停/恢复。

### 9.4 当前 resource 语义偏保守

resource 在 phase 通信完成前不会释放。

这会让约束更严格，但实现简单。

---

## 10. 交接结论

下一位 agent 的核心任务不是改 C++，而是做好这三件事：

1. 从 Python 侧生成“包群模板 `.bz2`”
2. 从 Python 侧生成“在线调度 manifest”
3. 消费在线结果 JSON 并回填到上层 timeline

最重要的接口事实只有三条：

- 一个 `.bz2` = 一个包群模板
- 一个 `phase` = 一个 compute + 一个包群注入
- 一个 `group.finish_cycle` = 该包群最后一个包的真实完成时刻

只要 Python 严格按这个契约准备输入，当前 C++ 在线模式就能直接工作。
