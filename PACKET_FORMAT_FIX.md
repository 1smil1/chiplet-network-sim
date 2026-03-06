# Packet Format Fix for >255 Nodes Support

**Date**: 2026-03-06
**Status**: ✅ COMPLETED

---

## Problem Summary

When extending the NOC simulator to support >255 nodes (from 64 max to 576 nodes), we needed to change the packet format to use `unsigned short` (2 bytes) instead of `unsigned char` (1 byte) for src/dst fields.

However, after initial modification, the simulator crashed with segmentation fault due to **struct padding alignment issues**.

---

## Root Cause

### Initial Modification (INCORRECT)

**Python format**:
```python
struct.pack('<QIIBHHBB', ...)
# Q = cycle (8 bytes)
# I = id (4 bytes)
# I = addr (4 bytes)
# B = type (1 byte)
# H = src (2 bytes)    <- PROBLEM HERE
# H = dst (2 bytes)
# B = node_types (1 byte)
# B = num_deps (1 byte)
```

**C++ struct**:
```c
struct nt_packet_pack {
    unsigned long long int cycle;    // 8 bytes
    unsigned int id;                  // 4 bytes
    unsigned int addr;                // 4 bytes
    unsigned char type;               // 1 byte
    unsigned short src;               // 2 bytes  <- PROBLEM HERE
    unsigned short dst;               // 2 bytes
    unsigned char node_types;         // 1 byte
    unsigned char num_deps;           // 1 byte
};
```

### The Problem

Even with `#pragma pack(1)`, **the C++ compiler added 1 byte of padding** after `type` to align `src` (unsigned short requires 2-byte alignment).

**Expected file layout** (Python writes):
```
Offset  Field
0-7     cycle
8-11    id
12-15   addr
16      type
17      src byte 0  <- Python writes here
18      src byte 1
19      dst byte 0
20      dst byte 1
21      node_types
22      num_deps
```

**Actual C++ reading** (with implicit padding):
```
Offset  Field
0-7     cycle
8-11    id
12-15   addr
16      type
17      [PADDING]   <- C++ expects padding here
18-19   src         <- C++ reads from here
20-21   dst
22      node_types
23      num_deps
```

### Consequences

When reading a packet with `src=0, dst=1`:

**File bytes**:
```
Byte 16: 0x01 (type)
Byte 17: 0x00 (src byte 0)
Byte 18: 0x00 (src byte 1)
Byte 19: 0x01 (dst byte 0)
Byte 20: 0x00 (dst byte 1)
```

**C++ interprets as**:
```
Byte 16: type = 0x01
Byte 17: [padding, ignored]
Byte 18-19: src = 0x0000 + 0x0001 = 256  <- WRONG!
Byte 20-21: dst = 0x0002 + 0x0000 = 512  <- WRONG!
```

Result: Simulator tries to access non-existent nodes (256, 512), causing **segmentation fault**.

---

## Solution

### Add Explicit Padding Byte

**Modified Python format**:
```python
struct.pack('<QIIBxHHBB', ...)
#              ^
#              | x = explicit padding byte
```

**Modified C++ struct**:
```c
struct nt_packet_pack {
    unsigned long long int cycle;
    unsigned int id;
    unsigned int addr;
    unsigned char type;
    unsigned char pad1;  // <-- EXPLICIT PADDING
    unsigned short src;
    unsigned short dst;
    unsigned char node_types;
    unsigned char num_deps;
};
```

Now **both Python and C++ agree on the layout**:
```
Offset  Field
0-7     cycle
8-11    id
12-15   addr
16      type
17      pad1 (explicit padding)
18-19   src
20-21   dst
22      node_types
23      num_deps
Total: 24 bytes per packet
```

---

## Files Modified

### 1. Python Side

**File**: `chip/chiplet-network-sim/tools/create_netrace.py`

**Lines 109-122**:
```python
def to_bytes(self) -> bytes:
    """转换为二进制格式"""
    packet_pack = struct.pack(
        '<QIIBxHHBB',     # x = padding byte for alignment
        self.cycle,       # unsigned long long (8 bytes)
        self.id,          # unsigned int (4 bytes)
        self.addr,        # unsigned int (4 bytes)
        self.type,        # unsigned char (1 byte)
                         # [1 byte padding added by x]
        self.src,         # unsigned short (2 bytes) - CHANGED to support >255 nodes
        self.dst,         # unsigned short (2 bytes) - CHANGED to support >255 nodes
        self.node_types,  # unsigned char (1 byte)
        self.num_deps     # unsigned char (1 byte)
    )
```

**Old format**: `'<QIIBHHBB'` (23 bytes, INCORRECT)
**New format**: `'<QIIBxHHBB'` (24 bytes, CORRECT)

### 2. C++ Side

**File**: `chip/chiplet-network-sim/src/netrace/netrace.c`

**Modified 2 structs**:

#### (1) Reading packets (lines 210-222):
```c
#pragma pack(push,1)
struct nt_packet_pack {
    unsigned long long int cycle;
    unsigned int id;
    unsigned int addr;
    unsigned char type;
    unsigned char pad1;  // Explicit padding to align src
    unsigned short src;  // Changed from unsigned char to support >255 nodes
    unsigned short dst;  // Changed from unsigned char to support >255 nodes
    unsigned char node_types;
    unsigned char num_deps;
};
#pragma pack(pop)
```

#### (2) Writing packets (lines 764-776):
```c
#pragma pack(push,1)
struct nt_read_pack {
    unsigned long long int cycle;
    unsigned int id;
    unsigned int addr;
    unsigned char type;
    unsigned char pad1;  // Explicit padding to align src
    unsigned short src;  // Changed from unsigned char to support >255 nodes
    unsigned short dst;  // Changed from unsigned char to support >255 nodes
    unsigned char node_types;
    unsigned char num_deps;
};
#pragma pack(pop)
```

### 3. Header struct (already correct)

**File**: `chip/chiplet-network-sim/src/netrace/netrace.h` (line 65)
**File**: `chip/chiplet-network-sim/src/netrace/netrace.c` (line 86)

```c
unsigned short num_nodes;  // Changed from unsigned char to support >255 nodes
unsigned char pad;         // Already had explicit padding
```

The header struct already had explicit padding, so it worked correctly.

---

## Verification

### Test Case: debug_v2.tra.bz2

Created minimal trace file with 1 packet: `src=0, dst=1`

**Before fix**:
```
NT_TRACEFILE---------------------
  ID:0 CYC:0 SRC:256 DST:512 ADR:0x00001000 TYP:ReadReq NDEP:0
[Segmentation fault]
```

**After fix**:
```
NT_TRACEFILE---------------------
  ID:0 CYC:0 SRC:0 DST:1 ADR:0x00001000 TYP:ReadReq NDEP:0

Time elapsed: 0.0017204s
Injection rate:0.000173611 flits/(node*cycle)    Injected:1    Arrived:  1    Timeout:  0
Average latency: 6  Average receiving rate: 0.000173611
```

✅ **CORRECT!**

### Method 1 Test: Layer 0 (130 packets, 576 nodes)

**Command**:
```bash
./ChipletNetworkSim.exe input/config_auto.ini
```

**Result**:
```
Injected:130    Arrived:  130    Timeout:  0
Average latency: 131.477  Average receiving rate: 0.225694
Internal Hops: 8.55385   Parallel Hops: 0   Serial Hops: 2.24615
```

**Output CSV**:
```
0.225694,131.477,0.225694
```

✅ **Method 1 (c_node_id based) works perfectly!**

---

## Key Lessons

1. **`#pragma pack(1)` is NOT always sufficient**
   - Some compilers still add padding for data type alignment
   - **Always use explicit padding bytes** for cross-language binary formats

2. **Python `struct` format characters**:
   - `x` = pad byte (no corresponding value)
   - `X` is NOT valid (must be lowercase)

3. **Debugging binary formats**:
   - Use `od -A x -t x1z -v` to inspect raw bytes
   - Compare expected vs actual byte layout
   - Check `sizeof(struct)` matches file format

4. **Struct alignment rules**:
   - `unsigned char` (1 byte): no alignment requirement
   - `unsigned short` (2 bytes): must start at even address
   - `unsigned int` (4 bytes): must start at address divisible by 4
   - `unsigned long long` (8 bytes): must start at address divisible by 8

---

## Packet Format Summary

### Final Format (24 bytes per packet)

| Offset | Bytes | Type | Field | Description |
|--------|-------|------|-------|-------------|
| 0-7 | 8 | Q | cycle | Simulation cycle |
| 8-11 | 4 | I | id | Packet ID |
| 12-15 | 4 | I | addr | Memory address |
| 16 | 1 | B | type | Packet type (ReadReq, WriteReq, etc.) |
| **17** | **1** | **x** | **pad1** | **Explicit padding for alignment** |
| 18-19 | 2 | H | src | Source node ID (0-65535) |
| 20-21 | 2 | H | dst | Destination node ID (0-65535) |
| 22 | 1 | B | node_types | Source/dest node types |
| 23 | 1 | B | num_deps | Number of dependencies |

### Dependencies (if any)

After the 24-byte packet header, if `num_deps > 0`:
```
Offset 24+ : (num_deps × 4 bytes) dependency IDs
```

---

## Next Steps

- [x] Fix packet format (add explicit padding)
- [x] Test Method 1 (c_node_id based)
- [ ] Test Method 2 (py_node_id based with position file)
- [ ] Compare latency results between Method 1 and Method 2
- [x] Verify position file mapping matches C++ auto-assigned positions

---

## Method 1 vs Method 2 Overview ⭐

### 核心映射文件格式

**文件**: `trace_node_position_mapping.txt`

**重要**: 这是**唯一的映射文件**，用于所有场景！

**格式**:
```
# py_node_id  x  y  py_chip  noc_chip  type  c_node_id
0             14 7  0        9         real  182
1             15 5  0        9         real  135
517           0  0  33       33        virtual  0
```

**列说明**:
- `py_node_id`: Python的tile_id（文件按此列升序排序）
- `x, y`: 全局grid坐标
- `py_chip`: Python的tier_id
- `noc_chip`: NOC chip_id（按tier中心坐标排序后的编号）
- `type`: `real`（真实tile）或 `virtual`（虚拟节点）
- `c_node_id`: **最后一列**，grid坐标计算：`c_node_id = y * K_x + x`（K_x=24）

### 方法1：c_node_id Based（不需要position文件）

**工作流程**:
1. Python生成`.bz2`文件，使用**c_node_id**作为src/dst
   - 例如：`src=182, dst=135`（而不是tile_id）
2. C++**不加载**position文件
3. C++用`id2nodeid()`**自动计算**位置：
   ```cpp
   c_node_182 → x=182%24=14, y=182/24=7 → chip_id=?, node_id=? → global_pos=(14,7)
   ```

**调试模式的作用**:
- Python生成`trace_node_position_mapping.txt`，记录Python认为的c_node_id→(x,y)映射
- C++在调试模式**加载**这个txt文件（但**不使用**它来路由）
- C++同时用`id2nodeid()`自动计算位置
- **验证对比**：加载的位置 vs 自动计算的位置 → 应该完全一致！
- 如果一致 → Python的noc_chip排序和C++的grid顺序匹配 ✓
- 如果不一致 → 需要调整Python的排序逻辑 ✗

**配置**:
- 生产模式：`config.ini`不包含`position_file`参数
- 调试模式：`config.ini`包含`position_file = input\test_method1\trace_node_position_mapping.txt`

### 方法2：py_node_id Based（需要position文件）

**工作流程**:
1. Python生成`.bz2`文件，使用**py_node_id（tile_id）**作为src/dst
   - 例如：`src=0, dst=1`（直接用tile_id）
2. C++**必须加载**position文件：`trace_node_position_mapping.txt`
3. C++建立映射表：`py_node_id → (x, y)`
4. C++用这个映射表来查找节点位置进行路由

**配置**:
- 必须在`config.ini`中指定：`position_file = input\test_method2\trace_node_position_mapping.txt`

### 映射文件的使用场景

**同一个txt文件用于**:
1. **方法1调试模式** - C++加载来验证自动分配是否正确
2. **方法2生产模式** - C++加载来建立py_node_id→position映射
3. **Python验证脚本** - 读取c_node_id和(x,y)的对应关系
4. **.bz2文件生成** - Python读取来决定packet的src/dst字段

**关键区别**:
- **方法1**: .bz2用c_node_id，C++不依赖txt（调试时加载但不使用）
- **方法2**: .bz2用py_node_id，C++必须依赖txt来查找位置

---

## Common Mistakes and Fixes

### Issue #1: Distinguishing Real vs Virtual Nodes (WRONG)

**Date**: 2026-03-06

**Mistake**: In verification scripts, filtering nodes by `type == 'real'`:
```python
# WRONG - DO NOT DO THIS
if node_type == 'real':
    c_node_to_pos[c_node_id] = (x, y, py_node_id)
```

**Why It's Wrong**:
- Both real tiles AND virtual nodes are **nodes** in the NOC simulator
- C++ treats them identically when routing packets
- Filtering out virtual nodes leads to incomplete verification

**Correct Approach**:
```python
# CORRECT - Process all nodes
c_node_to_pos[c_node_id] = (x, y, py_node_id)
```

**Location**: Affects all verification and analysis scripts that read `trace_node_position_mapping.txt`

---

## References

- Original packet format: `<QIIBBBBB>` (21 bytes, max 255 nodes)
- Intermediate broken format: `<QIIBHHBB>` (23 bytes, alignment mismatch)
- **Final working format**: `<QIIBxHHBB>` (24 bytes, explicit padding)

**Struct alignment**: https://en.wikipedia.org/wiki/Data_structure_alignment
