---
Status: draft
---

# ECSP 参考实现规格（L1–L4）

本规格定义 [`ecsp-spec.md`](ecsp-spec.md) 的可执行参考实现。规范正文是唯一的正确性来源：两者冲突时以规范为准，并提修订单。相关架构决定见 [`docs/adr/`](../adr/)。

## 1 目的与边界

**目的**：给设备组内的主机侧与从机侧提供同一份 C99 库，使"协议实现是否正确"这件事可以脱离具体板卡被测试与复现。

**非目标**：

* 不含任何板卡知识。0x30~0xEF 的语义一律由使用方以编译期 `const` 命令表注入（ADR-0001）。
* 不做业务逻辑、不做硬件驱动、不做 RTOS 适配、不做上位机 UI。
* 不定义产品行为：地址分配、拓扑、期望版本、低功耗策略均来自实例化文档。

**硬约束**：

| 约束 | 含义 | 出处 |
| --- | --- | --- |
| C99，无编译器扩展 | 不使用 VLA、`__attribute__`、嵌套函数 | ADR-0001 |
| 零动态分配 | 不调用 `malloc/free`；所有缓冲区由调用方提供 | ADR-0001 |
| 无可变全局状态 | 每个链路一个上下文；同一进程内可并存多个主/从实例 | ADR-0005 |
| 不取时间 | 库内不调用任何时钟；`now_ms` 由调用方每拍推入 | ADR-0005 |
| 非阻塞、事件驱动 | 无等待循环、无锁、无线程 | ADR-0005 |
| 字节级编解码 | 禁止结构体映射、`#pragma pack`、指针别名读多字节字段 | ADR-0006 |

## 2 分层与文件布局

| 层 | 职责 | 头文件 | 源文件 | 裁剪开关 |
| --- | --- | --- | --- | --- |
| L1 | 帧编解码、CRC、字节读写助手 | `include/ecsp/frame.h`、`bytes.h`、`crc.h` | `src/ecsp/core/{frame,crc,bytes}.c` | 总是编译 |
| L2 | 通用命令 0x01~0x12 的载荷编解码 | `include/ecsp/cmds.h` | `src/ecsp/core/cmds.c` | 总是编译 |
| L3 | 从机引擎：处理管线、会话、事务缓存、长耗时 | `include/ecsp/slave.h` | `src/ecsp/slave/*.c` | `ECSP_ENABLE_SLAVE` |
| L4 | 主机链路管理：双轴状态机、重发、掉线判定 | `include/ecsp/host.h` | `src/ecsp/host/*.c` | `ECSP_ENABLE_HOST` |

从机固件只开 L1~L3，主机侧只开 L1、L2、L4。L3 与 L4 互不引用。

```
include/ecsp/
  ecsp.h        伞形头，只做 include 与版本宏
  config.h      编译期配置与默认值
  frame.h bytes.h crc.h    L1
  cmds.h                    L2
  slave.h                   L3
  host.h                    L4
src/ecsp/
  core/  frame.c crc.c bytes.c cmds.c
  slave/ engine.c session.c txn_cache.c
  host/  link.c api.c
test/
  unit/  …按层分文件
  integ/ two_instance.c    同进程双实例互操作
  vectors/ vectors.h       golden vector（见第 9 章）
CMakeLists.txt             只用于本机测试，库源码不依赖 CMake
```

## 3 公共约定

**命名**：类型 `ecsp_*_t`，函数 `ecsp_<对象>_<动作>`，宏 `ECSP_*`。

**返回值**：库内函数返回 `int`，`0` 表示成功，负值为本地错误（`ECSP_E_ARG`、`ECSP_E_CAP`、`ECSP_E_CRC`…）。本地错误与协议应答码是两套东西，不互相转换；应答码只出现在帧里。

**缓冲区所有权**：库不拥有任何缓冲区。接收缓冲、发送缓冲、命令表、版本表全部由调用方在初始化时传入。库返回的 `payload` 指针只在本次调用期间有效。

**时间**：统一 `uint32_t now_ms`。所有时间比较写成回绕安全形式 `(int32_t)(now - mark) >= (int32_t)delta`，禁止 `now > deadline` 这种写法。

**字节序**：所有多字节读写走 `rd_u16le` / `wr_u16le` / `rd_u32le` / `wr_u32le`。定长字符串用 `ecsp_copy_fixed()`（不做 `strcpy`，不假设 `'\0'` 结尾）。

**配置**（`config.h`，均可被 `-D` 覆盖）：

| 宏 | 默认 | 说明 |
| --- | --- | --- |
| `ECSP_MAX_FRAME` | 128 | 单帧上限（字节）。板卡规范声明的最大帧长不得小于此值才可用本库 |
| `ECSP_ENABLE_SLAVE` / `ECSP_ENABLE_HOST` | 1 / 1 | 分层裁剪 |
| `ECSP_SLAVE_TXN_CACHE` | 4 | 事务缓存条目数；0 表示关闭去重（不推荐） |
| `ECSP_HOST_SLAVES` | 8 | 每个主机上下文可管理的从机槽位数 |

## 4 L1 帧层

```c
typedef struct {
    uint8_t        product_addr;
    uint8_t        board_addr;
    uint8_t        cmd;
    const uint8_t *payload;
    uint16_t       payload_len;
} ecsp_frame_t;

typedef enum {
    ECSP_SCAN_NEED_MORE = 0,  /* 缓冲区里还没有完整帧 */
    ECSP_SCAN_OK,             /* 得到一帧，*consumed 为整帧长度 */
    ECSP_SCAN_SKIPPED         /* 丢弃了 *consumed 字节（坏帧头/坏长度/CRC 错），继续扫 */
} ecsp_scan_t;

ecsp_scan_t ecsp_frame_scan(const uint8_t *buf, size_t len,
                            ecsp_frame_t *out, size_t *consumed);

size_t ecsp_frame_encode(uint8_t *buf, size_t cap,
                         uint8_t product_addr, uint8_t board_addr,
                         uint8_t cmd, const uint8_t *payload, uint16_t payload_len);
```

**解析规则**（严格按规范 3.1、3.2）：

1. 找 `0xFA`；找不到则 `NEED_MORE` 并丢弃已扫过的字节。
2. 长度字段 `< 9` 或 `> ECSP_MAX_FRAME`：`SKIPPED`，只前移 1 字节重找帧头。
3. 缓冲区不足一整帧：`NEED_MORE`。
4. 帧尾不是 `0x0D`：`SKIPPED`，前移 1 字节。
5. CRC16-MODBUS 覆盖"帧长度首字节至载荷末字节"，校验失败：`SKIPPED`，按帧长整帧丢弃。
6. 通过则填 `ecsp_frame_t`，`payload` 指向调用方缓冲区内部（零拷贝）。

解析失败一律静默丢弃、不产生应答（规范 6.2）。**扫描器不知道本节点地址**，地址过滤属于 L3/L4。

## 5 L2 通用命令编解码

每个通用命令一对编解码函数，函数只做字段级校验，不判断会话状态（那是 L3 的事）。

```c
typedef struct { uint8_t b[7]; } ecsp_version_t;   /* "X.Y.Z.N" 定长 7 字节 */

int ecsp_version_parse(const uint8_t *p, size_t n, ecsp_version_t *v);
int ecsp_version_equals(const ecsp_version_t *a, const ecsp_version_t *b); /* 逐字节 */
```

约定：

* 事务序号（非幂等动作类首字节）由 L2 的 `ecsp_txn_seq(req)` 取出，缓存键为 `(cmd, seq)`。
* 长耗时命令的受理应答载荷为 `[RS][T_exec_max u16le]`；非受理应答不带该字段。
* 结果事件帧不带应答码。
* 载荷长度校验返回 `ECSP_E_LEN`，由 L3 映射为 0x02；字段取值非法返回 `ECSP_E_VAL`，映射为 0x03。

命令按票据分摊，避免"谁都该做、谁都没做"：

* T3（管线 + 直通命令）：0x01、0x02、0x03、0x04、0x05、0x06、0x07、0x0B、0x12；
* T5（会话与时序）：0x10 心跳、0x11 故障上报；
* T6（长耗时）：0x08/0x09/0x0A、0x0C/0x0D/0x0E。

## 6 L3 从机引擎

```c
typedef int (*ecsp_slave_handler_t)(ecsp_slave_t *s, const ecsp_frame_t *req,
                                    uint8_t *resp, size_t cap, size_t *resp_len);

typedef enum {
    ECSP_CMD_NONE       = 0,
    ECSP_CMD_PREEMPTIVE = 1u << 0,  /* 长耗时执行中仍须受理（如停止命令） */
    ECSP_CMD_IDEMPOTENT = 1u << 1   /* 幂等动作：不带事务序号 */
} ecsp_cmd_flags_t;

typedef struct {
    uint8_t  cmd;
    uint8_t  flags;
    ecsp_slave_handler_t handler;
} ecsp_cmd_entry_t;

typedef struct {
    const ecsp_cmd_entry_t *table;   /* 使用方的编译期常量表，按 cmd 升序 */
    size_t table_len;
} ecsp_cmd_table_t;

int  ecsp_slave_init(ecsp_slave_t *s, const ecsp_slave_cfg_t *cfg);
void ecsp_slave_rx(ecsp_slave_t *s, const uint8_t *data, size_t len); /* 中断/收包回调里唯一允许的调用 */
void ecsp_slave_step(ecsp_slave_t *s, uint32_t now_ms);
int  ecsp_slave_send_event(ecsp_slave_t *s, uint8_t cmd, const uint8_t *payload, uint16_t len);
```

**处理管线顺序固定**（ADR-0002），一次只写一遍，handler 内不得重复：

| 序 | 判定 | 结果 |
| --- | --- | --- |
| 1 | 帧 CRC / 帧尾失败 | 静默丢弃 |
| 2 | 产品地址或板卡地址不匹配 | 静默丢弃 |
| 3 | 命令码未定义，或方向错误 | 0x04 |
| 4 | 会话未建立且命令不是 0x01 | 0x10 |
| 5 | 载荷长度不符 | 0x02 |
| 6 | 事务缓存命中 `(cmd, seq)` | 回放缓存应答 |
| 7 | 长耗时执行中，且该命令未标 `ECSP_CMD_PREEMPTIVE` | 0x11 |
| 8 | 参数校验失败 | 0x03 |
| 9 | 执行 handler，其返回值即应答码 | handler 的返回值 |

管线由 `engine.c` 一处实现；新增拒绝条件必须落到上表某一步。

**会话与心跳**：从机侧有两条轴——会话（`等待握手` / `就绪`）与执行（`空闲` / `忙`），对应主机侧看到的会话状态与事务状态。等待握手时以 1 Hz 上报 0x01；就绪后按 `ECSP_HEARTBEAT_MS`（默认 1000，±10%）上报 0x10；`last_send_ms` 在任一主动帧或应答帧发出后刷新（规范 7.2.2）。

**事务缓存**（ADR-0004）：FIFO，键 `(cmd, seq)`，存完整应答帧副本（长度受 `ECSP_MAX_FRAME` 约束）。**软复位后清空**，**每收到一次握手请求（0x01）也清空**。

**长耗时命令**：受理时置"忙"，启动 `now + T_exec_max`；到点未完成则后续获取结果应答 `0x00/0x11`（规范 4.3）；完成或中止时发结果事件并清"忙"。忙期间安全抢占类命令（如停止）必须放行——由命令表里的 `ecsp_cmd_flags` 标记，不硬编码命令码。系统复位（0x07）是通用层规定的抢占命令，不经命令表、不看标志位：忙期间到达时必须先走安全中止序列再应答复位（规范 4.4，ADR-0007）。

## 7 L4 主机链路管理

对每个从机槽位维护两条互不折叠的轴（ADR-0003）：

* **会话状态**：`未连接 / 握手中 / 已握手 / 正常通信 / 版本不符 / 掉线`（与规范表 4-1 一一对应）；
* **事务状态**：`空闲 / 待发送 / 等待应答 / 等待结果`（表 4-2），同一从机同时至多一个在途事务。

```c
typedef void (*ecsp_txn_cb_t)(ecsp_host_t *h, uint8_t board_addr, uint8_t cmd,
                              uint8_t response_code, const uint8_t *payload,
                              uint16_t len, void *user);

int  ecsp_host_init(ecsp_host_t *h, const ecsp_host_cfg_t *cfg);
int  ecsp_host_submit(ecsp_host_t *h, uint8_t board_addr, uint8_t cmd,
                      const uint8_t *payload, uint16_t len,
                      ecsp_txn_cb_t cb, void *user);
void ecsp_host_step(ecsp_host_t *h, uint32_t now_ms);
void ecsp_host_rx(ecsp_host_t *h, const uint8_t *data, size_t len);
```

**回调语义**（ADR-0005）：

* 回调在 `ecsp_host_step` 的调用栈上同步执行，每个事务恰好一次；
* 回调内**允许** `ecsp_host_submit`（只入队，不推进状态机）；
* 回调内**禁止**再次调用 `ecsp_host_step`（重入检测：置标志并返回 `ECSP_E_REENT`）。

**上电流程**（规范 4.1.1）：主动握手探测 3 次 → 失败则被动等待上报 → 收到 0x01 重入探测 → 超过会话建立总时限按掉线处理；握手成功后取版本并逐字节比较，不等则停在 `版本不符`。

**重发**：同一事务超时重发，含初发共 3 次，`attempt` 与 deadline 挂在事务上而非状态上；重发复用原事务序号。

**掉线判定**：连续 3 个心跳周期未收到该从机任何有效帧 → `掉线`；恢复通信后必须重新握手，不得直接恢复业务。

## 8 时序

| 项 | 默认 | 来源 |
| --- | --- | --- |
| 获取类应答 | ≤50 ms | 规范 7.1 |
| 设置类应答 | ≤100 ms | 规范 7.1 |
| 动作类首次应答 | ≤100 ms | 规范 7.1 |
| 在线升级首次应答 | ≤500 ms | 规范 7.1 |
| 心跳周期 | 1000 ms ±10% | 规范 5.2.16 |
| 掉线门限 | 3 个心跳周期 | 规范 7.2.1 |
| 会话建立总时限 | ≤5 s | 规范 4.1.1 |

库按"调用方每 `ECSP_TICK_MS`（建议 ≤10 ms）调一次 step"设计。tick 过大导致超时判定粗糙，由使用方负责；库不为此做补偿。

## 9 测试与 golden vector

**向量文件**：`test/vectors/vectors.h`，用 X-macro 列出 `{名称, 字节序列, 来源章节}`：

```c
ECSP_VECTOR(handshake_req, FA 09 00 03 01 02 88 50 0D,                        "spec 5.2.1")
ECSP_VECTOR(version_resp,  FA 11 00 03 02 02 00 31 2E 30 2E 30 2E 30 FA 0E 0D, "spec 5.2.2")
```

CRC 字节在文件里写成实际值，不写 `CRC16` 占位——向量必须是可直接断言的字节。上面的 `version_resp` 不是随手选的：它的 CRC 低字节恰好是 `0xFA`，扫描器若在 CRC 区里重新找帧头就会把这一帧切坏。`tools/check-vectors.ps1` 负责两件事：重新计算每个向量的 CRC 并比对；扫描 `ecsp-spec.md` 中写死具体字节的示例帧，报告"规范里有、向量里没有"的遗漏。

**三层测试**：

1. `test/unit/`：逐函数，输入输出确定性；
2. `test/unit/pipeline.c`：同一帧同时命中多个拒绝条件时返回哪个应答码（这是 ADR-0002 的可执行表述）；
3. `test/integ/two_instance.c`：一个主机上下文 + 一个从机上下文，经虚拟链路（字节数组 + 丢包/超时注入）跑完握手→版本核对→业务命令→掉线→重连。

**每个 PR 的三件套**（ADR-0001）：规范正文 diff（若行为有变）+ 向量 + 实现。缺一不合并。

## 10 完成判据

一张票算完成，需要同时满足：

* `cmake --build` 无警告（`-Wall -Wextra -Werror`，MSVC 用 `/W4 /WX`）；
* 新增行为有对应单测，且失败时给出可读断言信息；
* 若行为在规范中有对应条款，向量表里有一行指向该条款；
* 不引入 `malloc`、全局变量、时钟调用（CI 里用 `tools/check-no-alloc.ps1` 之类的文本检查兜底）。

## 11 不做清单

* 不做广播/组播寻址（规范 2.1：通用层不定义）；
* 不做 0x13~0x2F 的任何命令（附录 B 待评审）；
* 不在库里做浮点解析（载荷字节序由使用方按 3.3 处理）；
* 不实现升级数据通道（规范 5.2.12 明确由产品规范定义），只实现会话级控制与结果码；
* 不提供日志、断言、错误字符串——体积敏感，交给使用方。
