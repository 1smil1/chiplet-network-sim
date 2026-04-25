# 在线模式 Python 接口接入问题清单

## 目的

本文用于交接给另一个 agent 修复当前 Python -> C++ 在线模式接入中的已确认问题。

范围限定在：

- `global_sim_manager.py`
- `chip/comm/online_workload_builder.py`
- 在线模式结果回读与 `comm_config.json` 生成逻辑

当前总体状态是：

- 主流程已经接上
- `manifest + .bz2 templates + .ini + output.json` 这条链路已存在
- 但仍有若干 correctness 问题和设计假设过强的问题需要修复

下面按严重度列出。

---

## 问题 1：在线结果 JSON 路径推导不一致

### 严重度

高，可能直接导致模拟成功但 Python 误判失败。

### 位置

- [chip/comm/online_workload_builder.py](d:/python_prj/neurosim_test/neurosim_win_dse/chip/comm/online_workload_builder.py:494)
- [chip/comm/online_workload_builder.py](d:/python_prj/neurosim_test/neurosim_win_dse/chip/comm/online_workload_builder.py:671)

### 现象

`OnlineWorkloadBuilder.build()` 在写 `.ini` 时：

- `workload_file`
- `output_file`

使用的是接近 repo-root-relative 的路径字符串。

但 `_derive_json_output_path()` 在回读时把 `.ini` 里的 `output_file` 当成“相对于 `.ini` 所在目录”的路径来解析。

这两个约定不一致。

因此当 `online_dir` 是类似：

```text
chip/workspaces/GPT2/online_sim/...
```

时，Python 可能会去找一个错误路径，例如：

```text
<ini_dir>/chip/workspaces/.../sim_output.json
```

而不是真正的输出文件位置。

### 风险

- `ChipletNetworkSim.exe` 实际已经跑完
- 结果 JSON 实际已经生成
- 但 `run_online_sim()` 因路径解析错误返回 `None`
- 上层误认为在线仿真失败

### 修复要求

必须统一 `.ini` 中路径和 Python 回读路径的语义。

推荐二选一：

1. 全部写成相对于 `.ini` 所在目录的相对路径
2. 全部写成绝对路径

推荐方案：

- `.ini` 中 `workload_file / output_file / log_file` 都统一写成相对于 `.ini` 所在目录的相对路径
- `_derive_json_output_path()` 保持按 `.ini` 目录解析

这样最稳，也最符合当前函数写法。

### 验收标准

- 不同 `online_dir` 下都能正确定位输出 JSON
- `run_online_sim()` 不再因路径问题误判失败
- Windows 路径分隔符和相对路径均正常

---

## 问题 2：缺失 layer tile 映射时默认落到 resource 0

### 严重度

高，会 silently 产生错误调度。

### 位置

- [chip/comm/online_workload_builder.py](d:/python_prj/neurosim_test/neurosim_win_dse/chip/comm/online_workload_builder.py:461)

### 现象

当前逻辑：

```python
tile_info = layer_tile_map.get(layer_id, {})
tile_start = tile_info.get("tile_id_start", 0)
tile_end   = tile_info.get("tile_id_end", tile_start + 1)
resource_ids = list(range(tile_start, tile_end))
```

如果某个 `layer_id` 没出现在 `design_result` 的 tile map 里，就会自动退化成：

```python
resource_ids = [0]
```

### 风险

这会导致两类错误：

1. 不相关 layer 被错误串行化到同一个 resource 0
2. 当整体 `num_resources` 很小或为 0 时，manifest 本身变成不合法

而且这个错误是 silent 的，很难排查。

### 修复要求

这里不能 fallback 到 0，必须 fail fast。

推荐修复：

- 若 `layer_id` 不在 `layer_tile_map`
  - 直接抛异常
  - 异常信息必须包含 `layer_id`
- 若 `tile_id_start/tile_id_end` 缺失或区间非法
  - 直接抛异常

### 验收标准

- 缺 layer tile 映射时，builder 明确失败
- 不再隐式生成 `resource_ids=[0]`
- 错误信息足够定位具体 layer

---

## 问题 3：依赖关系被 `dep < layer_id` 静默裁剪

### 严重度

中到高。

### 位置

- [chip/comm/online_workload_builder.py](d:/python_prj/neurosim_test/neurosim_win_dse/chip/comm/online_workload_builder.py:454)

### 现象

当前逻辑：

```python
dep_phase_ids = [
    phase_id_base + dep
    for dep in layer_deps_map.get(layer_id, [])
    if dep < layer_id
]
```

这相当于假设：

- layer ID 一定按拓扑顺序严格递增

否则依赖会被 silently 丢掉。

### 风险

如果未来出现：

- layer ID 不是严格拓扑序
- layer 被重编号
- parser 输出依赖关系不是简单“小编号依赖大编号”

那么这里会把合法依赖静默删掉，导致在线调度错误。

### 修复要求

不要用 `dep < layer_id` 来过滤依赖。

推荐修复方式：

1. 直接保留所有 parser 给出的依赖
2. 在 builder 里做显式合法性检查：
   - `dep` 是否存在于 layer 集合中
3. 如果需要 DAG 校验，单独加验证逻辑，不要靠这个比较运算做“sanity”

### 验收标准

- builder 不再静默删除依赖
- 非法依赖时报错
- 合法但编号不满足 `dep < layer_id` 的依赖仍能保留

---

## 问题 4：`comm_config.json` 仍退化成单 input、单层静态覆盖

### 严重度

中。

### 位置

- [global_sim_manager.py](d:/python_prj/neurosim_test/neurosim_win_dse/global_sim_manager.py:590)
- [global_sim_manager.py](d:/python_prj/neurosim_test/neurosim_win_dse/global_sim_manager.py:596)

### 现象

当前 `_write_online_comm_config()` 做的是：

- 只取 `input_id == 0`
- 假设 `group_id == layer_id`
- 把每层通信时间写回 `per_layer_overrides`

即：

```python
duration_cycles = grp.finish_cycle - grp.inject_cycle
layer_latency_ns = duration_cycles / freq
```

然后交给 `generate_co_final()`.

### 问题本质

这会把真实在线结果重新压缩回“单 input 的每层静态通信延迟”。

因此：

- 虽然在线 NoC 已经跑了多 input 共享网络
- 但 merge 阶段真正消费的仍然是 input 0 的单层静态延迟

也就是说：

- method 5 当前在仿真层面是 online 的
- 但在 `co_final` 消费层面仍然基本是单 input 近似

### 修复要求

这里先不要盲目大改，但必须明确分两步。

#### 第一步：短期最小修复

至少把当前代码里的假设显式化：

- 注释写清楚：这是“为了兼容现有 `generate_co_final()` 的临时降维”
- 如果 `group_id != layer_id`，必须显式报错，不要继续 silent 假设

#### 第二步：中期正确修复

后续需要让 merge 阶段直接消费在线 timeline，而不是退回 `per_layer_overrides`

可能方向：

- 新增 online-aware 的 `generate_co_final`
- 或直接让 `global_sim_manager` 基于 `sim_result.groups` / `sim_result.inputs` 构造新的 merge 输入

### 验收标准

短期：

- 代码和文档明确这是兼容层，不是真正的多 input merge
- 不再 silent 假设 `group_id == layer_id`

中期：

- merge 阶段能消费真实在线结果，而不是只消费 input 0

---

## 总体评价

当前另一个 agent 的接入方向是正确的，已经对上了 C++ 在线接口：

- 使用 `manifest + .bz2 templates + .ini`
- 走 `ChipletNetworkSim.exe`
- 读取在线 JSON 结果

也就是说，大方向没问题，主流程已经接通。

现在的问题主要集中在：

- 路径契约没有收紧
- 若干 fallback 太危险
- 部分设计假设写死太早
- merge 端仍然是临时兼容方案

---

## 建议修复顺序

按下面顺序修。

### 第一优先级

1. 修 JSON 输出路径推导不一致
2. 修缺失 tile map 时的 silent fallback

### 第二优先级

3. 去掉 `dep < layer_id` 的隐式依赖裁剪

### 第三优先级

4. 给 `comm_config.json` 降维兼容层加明确保护和注释
5. 再规划真正的 online merge

---

## 修复后需要补的验证

### 用例 1：不同 `online_dir`

- 把 `online_dir` 放到不同目录深度
- 确认 `run_online_sim()` 都能正确找到 JSON

### 用例 2：缺失 tile map

- 人工删掉某层 tile 映射
- builder 必须明确报错

### 用例 3：非单调 layer 依赖

- 构造一个 layer ID 编号不严格拓扑递增的最小例子
- 确认依赖不会被静默删掉

### 用例 4：多 input 在线结果

- 确认 `global_sim_manager` 至少能明确标识当前 merge 是临时兼容模式

---

## 交接结论

这不是“重写接口”的任务，而是“把已经接上的接口修到稳”的任务。

另一个 agent 修复时应优先保证：

- 不 silent fallback
- 不 silent 丢依赖
- 路径规则单一明确
- 临时兼容假设必须显式写明
