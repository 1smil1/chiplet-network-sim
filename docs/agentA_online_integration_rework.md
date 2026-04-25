# AgentA 在线接入重构说明

## 1. 目的

本文给 AgentA，说明本轮 Python 在线接入层已经调整了什么，以及 AgentA 后续应当基于什么契约继续接 method-5。

本轮修改范围只在 Python 接入层：

- `chip/phase/layer_constraint_builder.py`
- `chip/comm/online_workload_builder.py`
- `global_sim_manager.py`

本轮**没有修改** C++ 在线接口契约。

所以 C++ 侧仍然要求：

- `manifest + .bz2 templates + .ini + output.json`

---

## 2. 本轮已经改掉的核心问题

### 2.1 层依赖不再由 builder 临时猜

新增：

- `chip/phase/layer_constraint_builder.py`

它会把 `Network_*.csv` 解析成标准化层约束结果：

- `layer_id`
- `deps_absolute`
- `succs_absolute`
- `topo_level`
- `frontier_id`
- `topological_order`
- `dependency_frontiers`

并做严格校验：

- 依赖列解析失败默认报错
- 依赖越界报错
- 自依赖报错
- 图中有环报错

也就是说，后续不要再在 builder 里自己从 `_parse_network_csv()` 拼 `dep_phase_ids`。

### 2.2 在线 builder 已改成消费标准化约束结果

`online_workload_builder.py` 现在不再直接依赖旧的 `_parse_network_csv()`。

当前流程是：

1. 读取 `performance_per_layer.json`
2. 读取 `design_result.json`
3. 读取 `topology.json`
4. 读取 `layout.json`
5. 调用 `build_layer_constraint_graph(...)`
6. 基于标准化 graph 生成：
   - `.bz2` 模板
   - `manifest.json`
   - `sim_config.ini`

同时会额外写：

- `layer_constraint_graph.json`

位置：

- 与 `manifest.json` 同目录

这个文件是给后续 merge / debug / 其他 agent 直接复用的。

### 2.3 路径契约已经统一

现在统一采用：

- `.ini` 中 `workload_file / output_file / log_file`
  - 都是相对于 `.ini` 所在目录
- `manifest.json` 中 `bz2_path`
  - 相对于 `manifest.json` 所在目录
- `run_online_sim()`
  - 运行 exe 时会把 `cwd` 设成 `.ini` 所在目录

所以后续不要再写 repo-root-relative 的 `.ini` 路径。

### 2.4 tile/resource 映射改成 fail fast

`online_workload_builder.py` 里现在要求：

- 每个 layer 必须在 `design_result` 里有 tile 映射
- `tile_id_start / tile_id_end` 必须存在且区间合法

否则直接报错。

已经去掉了“缺失时默认 resource 0”的危险 fallback。

### 2.5 method-5 的 comm_config 降维逻辑已显式化

`global_sim_manager.py` 里保留了对现有 `generate_co_final()` 的兼容层，但现在明确写清楚了：

- 这是把在线结果临时投影成 `input 0` 的 per-layer latency
- 不是完整的多 input online merge

并且 `comm_config.json` 里新增了：

- `online_result_mode = compat_single_input_projection`
- `source_num_inputs`
- `projection_input_id`
- `layer_constraint_graph_path`

如果未来 AgentA 要继续改 merge，请基于这些字段，而不是假设 method-5 已经是完整多 input merge。

---

## 3. 当前必须遵守的输入契约

### 3.1 一个 `.bz2` = 一个包群模板

保持不变。

### 3.2 一个 phase 当前只对应一个 `group_id`

保持不变。

### 3.3 builder 当前仍采用 `group_id == layer_id`

这是当前 Python 侧的明确约定。

不是 C++ 强制要求，但本轮 Python builder 就是这么构造的。

如果后续 AgentA 要改：

- 一个 layer 对应多个 group
- 或 group_id 不等于 layer_id

那么必须同时修改：

- builder
- `_write_online_comm_config()`
- 下游 merge 的 layer/group 映射逻辑

当前代码已经在这些位置加了显式校验，不允许 silent assumption。

---

## 4. 新的层约束结果长什么样

当前 `layer_constraint_graph.json` 结构大致如下：

```json
{
  "layers": [
    {
      "layer_id": 0,
      "deps_absolute": [],
      "succs_absolute": [1, 2],
      "topo_level": 0,
      "frontier_id": 0
    },
    {
      "layer_id": 1,
      "deps_absolute": [0],
      "succs_absolute": [3],
      "topo_level": 1,
      "frontier_id": 1
    }
  ],
  "topological_order": [0, 1, 2, 3],
  "dependency_frontiers": [
    [0],
    [1, 2],
    [3]
  ]
}
```

字段含义：

- `deps_absolute`
  - 当前层直接依赖谁

- `succs_absolute`
  - 当前层完成后会解锁谁

- `topo_level / frontier_id`
  - 依赖意义上的并行层级

- `dependency_frontiers`
  - 只看依赖关系时，可同时 ready 的层集合

当前这份结果还没有把资源约束算进去。

资源约束仍由 builder 通过：

- `resource_ids`

在 manifest 里提供给 C++ 在线调度器。

---

## 5. AgentA 后续该怎么接

### 5.1 如果继续做在线输入构建

请直接消费：

- `layer_constraint_graph.json`
或
- `build_layer_constraint_graph(...)`

不要再重复写一套 CSV 依赖解析。

### 5.2 如果继续改 merge

当前 `global_sim_manager.py` 只是兼容层。

真正要做多 input online merge，建议下一步：

- 不再只取 `input_id == 0`
- 直接消费 `OnlineSimResult.groups` 和 `OnlineSimResult.inputs`
- 结合 `layer_constraint_graph.json` 做真实时间轴 merge

### 5.3 如果继续改 group 语义

当前代码默认：

- `group_id == layer_id`

这是 builder 约定，不是最终真理。

但只要你改掉这个约定，就必须同步处理：

- `comm_config.json` 写出逻辑
- `generate_co_final()` 的兼容输入
- layer/group 映射存储方式

不要只改 builder 一处。

---

## 6. 本轮没有改的内容

下面这些内容本轮没有动，AgentA 不要误判。

### 6.1 C++ 接口没变

没有改：

- `manifest` 字段契约
- `.bz2` 模板语义
- `ChipletNetworkSim.exe` 入口
- 在线输出 JSON 格式

### 6.2 没有实现真正多 input merge

当前仍然只是兼容层投影。

### 6.3 没有引入运行时追加任务

在线模式仍然是：

- 一次生成全部 workload
- 一次性交给 C++

---

## 7. 当前必须通过的验收样例

AgentA 如果继续修改，至少要保证下面这些样例不回退。

### 7.1 层约束解析

必须支持：

- 顺序依赖：`0 -> 1 -> 2`
- 并行依赖：`0 -> 1, 0 -> 2, 1/2 -> 3`
- 合法 DAG 但不只是简单顺序

必须报错：

- 依赖越界
- 自依赖
- 图中成环
- CSV 依赖列格式错误（严格模式）

### 7.2 builder

必须保证：

- 缺 layer tile map 时报错
- 非法 tile range 时报错
- `.ini`、`manifest`、`.bz2` 路径在不同输出目录深度下都能跑通

### 7.3 method-5

必须保证：

- `global_sim_manager.py` 还能跑 method-5
- `comm_config.json` 能被写出
- 其中包含兼容投影标记字段

---

## 8. 一句话结论

本轮已经把：

- 依赖解析
- 路径契约
- tile/resource 严格校验
- method-5 兼容降维标识

这几个最容易出错的点收紧了。

AgentA 后续应该把这套“层约束结果”当成唯一真相源继续扩展，而不是再重新写一套 CSV 推断逻辑。
