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
* 不定义产品行为：地址分配、拓扑、期望版本均来自实例化文档。芯片如何进入低功耗硬件态由实例化文档定义；链路静默与掉线暂停见规范 5.2.11 / 7.2。

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
| L2 | 通用命令目录，以及版本、序列号、低功耗取值的字段解析 | `include/ecsp/cmds.h` | `src/ecsp/core/cmds.c` | 总是编译 |
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
  slave/ engine.c session.c txn_cache.c long_run.c
  host/  link.c
board/f429/
  bringup.c/.h             板卡功能命令表 0x30–0x33（不进协议库）
  fw/                      从机固件（USART1；Cube HAL/LL，见 ADR-0009；
                           `make -C board/f429/fw`，不开 L4）
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
| `ECSP_SLAVE_TXN_CACHE` | 4 | 事务缓存条目数（每条目约占 `ECSP_MAX_FRAME` + 4 字节上下文 RAM）；0 表示关闭去重（不推荐） |
| `ECSP_HOST_SLAVES` | 8 | 每个主机上下文可管理的从机槽位数 |
| `ECSP_HEARTBEAT_MS` | 1000 | 就绪后 0x10 心跳的周期，规范允许 ±10%；产品覆盖时须同步调整掉线门限（规范 5.2.16、7.2.1）。等待握手时的 0x01 上报由规范 4.1.2 固定为 1 Hz，没有对应宏 |
| `ECSP_RESP_DEFAULT_MS` | 50 | 目录里获取类、设置类、动作类和握手共用的应答时限（规范 7.1）。0x07、0x03、0x05、0x0C 不用它。产品只允许放宽。只放宽别的命令不改变它 |
| `ECSP_RESP_RESET_MS` | 50 | 目录里系统复位（0x07）自己的应答时限。数值与默认值相同。只放宽它不改变 `ECSP_RESP_DEFAULT_MS`，也不拉长其它命令 |
| `ECSP_RESP_UNLISTED_MS` | 100 | 目录没有的命令（板卡命令）主机仍等待的毫秒数。板卡命令表写明应答时限之前，保持这些命令今天的上限 |
| `ECSP_RESP_IDENTITY_MS` | 3000 | 目录里设置硬件版本（0x03）与设置序列号（0x05）的应答时限（规范 7.1）。产品只允许放宽。等待该应答时，3 s 掉线判据不截断含初发共 3 次的重发 |
| `ECSP_RESP_UPGRADE_MS` | 500 | 目录里在线升级（0x0C）第一次应答的时限（规范 7.1） |
| `ECSP_SESSION_SETUP_MS` | 5000 | 上电至可下发业务命令的会话建立总时限（规范 4.1.1、表 7-2）。产品只允许放宽 |

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

## 5 L2 通用命令目录

从机引擎和主机链路向同一张目录询问一条通用命令是什么。两边不互相引用。目录对一条通用命令给出：请求载荷长度、应答数据上限（不含应答码）、命令类别（`ecsp_cmd_flags_t`）、载荷是否以事务序号开头（`ECSP_CMD_TXN_SEQ`）、应答时限，以及长耗时三元组（动作命令、结果事件、获取结果）。板卡命令 0x30~0xEF 不进这张目录。

0x0A、0x0E、0x10、0x11 在目录里记为事件，不是请求。主机拿这些命令码来发请求，从机仍应答 0x04。0x0F 在目录里，是设置类请求：控制字节 1 个，应答数据上限 0，无事务序号，应答时限 50 ms（`ECSP_RESP_DEFAULT_MS`），不写入非易失存储。控制字节 0x00 关闭上传模式，0x01 开启，都应答 0x00；再发当前值仍应答 0x00。其它控制字节应答 0x03，模式不变。

从机管线读命令类别、请求长度、事务序号标志和应答数据上限。主机只读应答时限和长耗时三元组，不读命令类别。从机发出结果事件时，事件码也取自三元组：0x08 的结果事件是 0x0A、获取结果是 0x09；0x0C 的结果事件是 0x0E、获取结果是 0x0D。

应答时限是静默上限，提前到达的应答立即结束等待。目录里的毫秒数就是下面这些宏，不是按命令类别另算的一档：

| 命令 | 应答时限 |
| --- | --- |
| 获取类 0x02、0x04、0x06、0x09、0x0D、0x12，握手 0x01，0x08 的受理应答，0x0B，0x0F | 50 ms，`ECSP_RESP_DEFAULT_MS` |
| 0x07 | 50 ms，`ECSP_RESP_RESET_MS`。单独保存，放宽它不改变上一行 |
| 0x03、0x05 | 3 s，`ECSP_RESP_IDENTITY_MS` |
| 0x0C 的第一次应答 | 500 ms，`ECSP_RESP_UPGRADE_MS` |
| 目录没有的命令 | 100 ms，`ECSP_RESP_UNLISTED_MS` |

获取类、设置类、动作类和握手引用同一个宏。产品用 `-D` 放宽该宏时，目录里引用它的条目一起放宽。0x07 不引用它。字段解析只做字段级校验，不判断会话状态（那是 L3 的事）。

```c
typedef struct { uint8_t b[7]; } ecsp_version_t;   /* "X.Y.Z.N" 定长 7 字节 */
typedef struct { uint8_t b[12]; } ecsp_serial_t;    /* "SN"+10 位数字，定长 12 字节 */

int ecsp_version_parse(const uint8_t *p, size_t n, ecsp_version_t *v);
int ecsp_version_equals(const ecsp_version_t *a, const ecsp_version_t *b); /* 逐字节 */
int ecsp_serial_parse(const uint8_t *p, size_t n, ecsp_serial_t *sn);
int ecsp_low_power_mode_valid(uint8_t mode);                                /* 0x00 / 0x01 */
```

约定：

* 事务序号（非幂等动作类首字节）由 L3 的第 6 步从 `req->payload[0]` 取出，缓存键为 `(cmd, seq)`。通用命令是否带事务序号看目录里的 `ECSP_CMD_TXN_SEQ`，板卡命令看命令表的同一标志。`ECSP_CMD_LEN_ANY` 的命令若收到空载荷，就没有序号可作键，此时按普通请求处理（不缓存）。
* 长耗时命令的受理应答载荷为 `[RS][T_exec_max u16le]`；非受理应答不带该字段。
* 结果事件帧不带应答码。
* 载荷长度校验返回 `ECSP_E_LEN`，由 L3 映射为 0x02；字段取值非法返回 `ECSP_E_VAL`，映射为 0x03。硬件版本与软件版本同用 7 字节 `X.Y.Z.N` 布局；序列号必须是 `SN` + 10 位 ASCII 数字。

命令按票据分摊，避免"谁都该做、谁都没做"：

* T3（管线 + 直通命令）：0x01、0x02、0x03、0x04、0x05、0x06、0x07、0x0B、0x12；
* T5（会话与时序）：0x10 心跳、0x11 故障上报；
* T6（长耗时）：0x08/0x09/0x0A、0x0C/0x0D/0x0E。

## 6 L3 从机引擎

`ecsp_cmd_flags_t` 定义在 L2 的 `cmds.h`，从机头文件包含它。板卡命令表与通用命令目录用同一组比特。

```c
typedef int (*ecsp_slave_handler_t)(ecsp_slave_t *s, const ecsp_frame_t *req,
                                    uint8_t *resp, size_t cap, size_t *resp_len);

typedef enum {
    ECSP_CMD_NONE       = 0,
    ECSP_CMD_PREEMPTIVE = 1u << 0,  /* 长耗时执行中仍须受理（停止控制因此能进 handler） */
    ECSP_CMD_IDEMPOTENT = 1u << 1,  /* 幂等动作：不带事务序号 */
    ECSP_CMD_TXN_SEQ    = 1u << 2,  /* 非幂等动作：载荷首字节即事务序号，应答按 (cmd, seq) 缓存并回放 */
    ECSP_CMD_QUERY      = 1u << 3,  /* 获取类：忙期间仍受理，但不抢占（规范 4.3） */
    ECSP_CMD_CONCURRENT = 1u << 4   /* 资源正交的动作：忙期间仍受理，不抢占（规范 4.3.1） */
} ecsp_cmd_flags_t;

typedef int (*ecsp_cmd_validate_t)(const ecsp_frame_t *req);  /* ECSP_OK / ECSP_E_LEN / ECSP_E_VAL */

typedef struct {
    uint8_t  cmd;
    uint8_t  flags;
    uint16_t req_len;  /* 请求载荷精确长度；ECSP_CMD_LEN_ANY = 由 validate 校验 */
    uint16_t resp_max; /* 应答数据（不含应答码）最大长度 */
    ecsp_cmd_validate_t validate;  /* 可选的第 8 步参数校验 */
    ecsp_slave_handler_t handler;
} ecsp_cmd_entry_t;

typedef struct {
    const ecsp_cmd_entry_t *table;   /* 使用方的编译期常量表（0x30~0xEF） */
    size_t table_len;
} ecsp_cmd_table_t;

typedef struct {
    ecsp_version_t hardware_version; /* 0x03 写、0x04 读 */
    ecsp_serial_t  serial_number;    /* 0x05 写、0x06 读 */
} ecsp_slave_identity_t;

int  ecsp_slave_init(ecsp_slave_t *s, const ecsp_slave_cfg_t *cfg);
void ecsp_slave_rx(ecsp_slave_t *s, const uint8_t *data, size_t len); /* 中断/收包回调里唯一允许的调用 */
void ecsp_slave_step(ecsp_slave_t *s, uint32_t now_ms);
int  ecsp_slave_send_event(ecsp_slave_t *s, uint8_t cmd, const uint8_t *payload, uint16_t len);
uint8_t ecsp_slave_low_power_mode(const ecsp_slave_t *s);
int  ecsp_slave_reset_requested(const ecsp_slave_t *s);
typedef int (*ecsp_slave_safe_abort_t)(ecsp_slave_t *s, uint8_t cmd);
void    ecsp_slave_set_safe_abort(ecsp_slave_t *s, ecsp_slave_safe_abort_t fn);
typedef int (*ecsp_slave_identity_commit_t)(ecsp_slave_t *s,
                                            const ecsp_slave_identity_t *next);
void    ecsp_slave_set_identity_commit(ecsp_slave_t *s,
                                       ecsp_slave_identity_commit_t fn);
uint8_t ecsp_slave_long_run_cmd(const ecsp_slave_t *s);
int     ecsp_slave_long_run_complete(ecsp_slave_t *s, uint8_t result);
```

`ecsp_slave_cfg_t` 还传入：`identity`（调用方持有的标识存储）、`fault_word`（0x12 返回的注入故障字）、`upload_mode`（上电上传模式）、`cmds`（0x30~0xEF 板卡命令表）。`upload_mode` 只接受 `ECSP_UPLOAD_ON` 或 `ECSP_UPLOAD_OFF`；其它值（含 0）使 `ecsp_slave_init` 返回 `ECSP_E_ARG`，因此未填写的配置不能变成关闭。产品未另作声明时填写 `ECSP_UPLOAD_ON`。发送缓冲至少容纳目录中最大的通用应答数据，或板卡表声明的 `resp_max`；`ecsp_slave_init` 会拒绝过小的缓冲。命令可以声明 `validate`（第 8 步）：返回 `ECSP_E_LEN` 映射 0x02，返回 `ECSP_E_VAL` 映射 0x03。handler 只写应答载荷的**数据**部分（不含应答码），返回值即应答码；框架不覆盖该返回值，非 0x00 应答不携带数据。板卡命令表仍只描述 0x30~0xEF。通用命令的请求长度、命令类别、事务序号和应答数据上限来自第 5 章的目录；校验函数和 handler 留在引擎里。

**处理管线顺序固定**（ADR-0002），一次只写一遍，handler 内不得重复：

| 序 | 判定 | 结果 |
| --- | --- | --- |
| 1 | 帧 CRC / 帧尾失败 | 静默丢弃 |
| 2 | 产品地址或板卡地址不匹配 | 静默丢弃 |
| 3 | 命令码未定义，或方向错误 | 0x04 |
| 4 | 会话未建立且命令不是 0x01 | 0x10 |
| 5 | 载荷长度不符 | 0x02 |
| 6 | 事务缓存命中 `(cmd, seq)` | 回放缓存应答 |
| 7 | 长耗时执行中，且该命令未标 `ECSP_CMD_PREEMPTIVE`、`ECSP_CMD_QUERY`、`ECSP_CMD_CONCURRENT` 之一 | 0x11 |
| 8 | 参数校验失败 | 0x03 |
| 9 | 执行 handler，其返回值即应答码 | handler 的返回值 |

管线由 `engine.c` 一处实现；新增拒绝条件必须落到上表某一步。第 5 步的长度，通用命令取目录的请求长度，板卡命令取命令表的 `req_len`。`ECSP_CMD_LEN_ANY` 表示命令自有变长载荷，由该命令的 `validate` 在第 8 步校验并返回 `ECSP_E_LEN`（映射 0x02）。第 6 步是否带事务序号，通用命令看目录里的 `ECSP_CMD_TXN_SEQ`，板卡命令看命令表的同一标志。第 7 步的放行标志同样来自目录或板卡表，不来自应答时限。第 8 步对通用命令使用库内校验函数，对板卡命令使用命令表注入的 `validate`；handler 不重复上述任何判定。

**0x07 / 0x0B 的平台边界**：库不做硬件驱动（第 1 章）。0x07 在应答入队后只置位 `ecsp_slave_reset_requested`；产品必须在发送 FIFO 排空后执行实际复位，该复位重新初始化上下文，会话回到等待握手、事务缓存清空都发生在那一刻（见 ADR-0008）。库内不提前清空会话与缓存：复位真正发生前，重传的同一 `(0x07, seq)` 必须回放原应答，若先清会话就会回 0x10，主机无法与"被拒绝"区分；窗口内不同事务序号的新 0x07 则应答 0x11（规范 5.2.7、4.4 第 5 条），不执行并发复位。0x0B 把受理的模式记入 `ecsp_slave_low_power_mode`：进入（0x01）之后库内不再排队心跳、故障上报或结果事件，退出（0x00）或握手请求被受理后恢复；产品另用该值执行进入/退出硬件低功耗。

**0x03 / 0x05 的提交**：这两条是非易失写入，库不做扇区擦除。`ecsp_slave_set_identity_commit` 注册提交回调，库把“这一项替换后的整份身份”交给它。回调返回 `ECSP_OK` 之后，库才把这份身份写入 `identity` 并应答 0x00；其它返回值应答 0x01，`identity` 保持原样。未注册回调时仍只改 RAM 并应答 0x00。回调在 `ecsp_slave_step` 的栈上执行，不得再次调用 `ecsp_slave_step`。重新初始化会清掉回调。格式非法应答 0x03、长度不符应答 0x02、会话未建立应答 0x10、长耗时执行中应答 0x11，地址不匹配和 CRC 失败则静默丢弃；这些路径都不调用提交。这两条命令没有事务序号，相同内容的重发会再提交一次，身份仍是这一份值。

**会话与心跳**：从机侧有两条轴——会话（`等待握手` / `就绪`）与执行（`空闲` / `忙`），对应主机侧看到的会话状态与事务状态。`session.c` 维护两个回绕安全的时间戳，都在第一次 `ecsp_slave_step` 时锁存（库内取不到时钟），因此首个周期帧在一个完整周期之后才到期，而不是上电立刻发出：

* `last_send`：任一帧入队即刷新，`就绪` 后据此判心跳——距上次发送满 `ECSP_HEARTBEAT_MS` 才发 0x10（规范 5.2.16 允许 ±10%，产品覆盖时同步调整 7.2.1 的掉线门限）；低功耗进入后不发 0x10；
* `last_report`：只在 0x01 上报真正入队后刷新，因此 `等待握手` 期间的 1 Hz 上报是固定节拍，不被应答或 0x10 拒绝帧饿死（规范 4.1.2"以 1 Hz 频率……直至握手成功"）。规范 4.1.2 未给该周期容差或产品覆盖权，所以它不是配置宏。

"发送成功"在库内只能观察到自己把整帧放进调用方的发送 FIFO，因此入队即视为发出（规范 7.2.2 的 `last_send_time`）；FIFO 放不下时该帧顺延到下一拍，且不推进对应时间戳。每个 step 至多发出一帧周期帧，因此 `now_ms` 回绕或时钟跳跃既不会把 0x01/0x10 永久拖没，也不会一次补发多帧。已就绪后再收到握手请求只应答 0x00 并保持就绪，不恢复 0x01 上报，并离开低功耗；若此时有长耗时命令在执行，先调用安全中止回调再结束协议侧执行，不发送结果事件（规范 4.1.4）。中止回调缺失或失败时握手仍应答 0x00，不因此置故障位——置位上报只属于 0x07 无法安全中止的路径（规范 4.4）。周期上报不受 `reset_requested` 影响，复位真正发生前心跳照发（ADR-0008），但低功耗期间仍抑制。低功耗期间 `ecsp_long_run_flush` 不发出已排队的故障上报与结果事件，退出后再发。

**事务缓存**（ADR-0004）：FIFO，键 `(cmd, seq)`，存完整应答帧副本，每条目 `ECSP_MAX_FRAME` 字节。缓存容量耗尽时覆盖最旧条目。**软复位后清空**（在本库里就是重新初始化上下文），**每收到一次被受理的握手请求（0x01）也清空**（被管线拒绝的握手不算——一帧坏帧不该把在途事务的保护清掉）。超过一个缓存槽长度的应答照常发给主机，只是不缓存；`ecsp_frame_encode` 已把帧限制在 `ECSP_MAX_FRAME` 内，因此该分支是防御性的，不构成可达状态。

**长耗时命令**（0x08/0x09/0x0A 与 0x0C/0x0D/0x0E，同一时刻至多一条在执行）：

* 启动受理应答 `0x00` + `T_exec_max`（u16le）。自检默认 `ECSP_SELFTEST_EXEC_MAX_MS`（2000），升级默认 `ECSP_UPGRADE_EXEC_MAX_MS`（60000）。拒绝应答只有应答码，不带该字段。
* 受理后 `ecsp_slave_long_run_cmd` 返回该命令码。产品做完后调用 `ecsp_slave_long_run_complete(result)`（`result` 不得为 0x11）。下一次 `step` 在上传模式开启时发送一次结果事件（0x0A 或 0x0E），不重发。关闭时不把这一帧排进发送队列；0x09 或 0x0D 仍能读到结果。
* 未完成时获取结果应答 `0x00/0x11`，包括已经超过 `T_exec_max`；到点不自动完成、也不自动失败。从未启动，或结局被复位/握手丢弃时，获取结果应答 `0x13` 且不带结果码。
* 停止（控制码 0x01，新事务序号）先调用安全中止回调（若已注册），再受理并携带 `T_exec_max`，在途命令结果码变为 `0x21`，并按上一则规则发送一次结果事件。回调缺失或失败不改这个结果码；`0x20` 只用于复位抢占失败。当前没有该命令的在途实例时应答 `0x13`。
* 忙期间：目录里标了 `ECSP_CMD_QUERY` 的获取类放行；`ECSP_CMD_PREEMPTIVE` 放行（0x08/0x0C 因此放行，由 handler 拒绝第二次启动）；`ECSP_CMD_CONCURRENT` 放行且不结束在途命令（规范 4.3.1 的资源正交类）；其余动作/设置应答 `0x11`。带 `ECSP_CMD_TXN_SEQ` 的动作在第 6 步命中缓存时回放原应答，不执行第二次。0x07 在目录里带 `ECSP_CMD_PREEMPTIVE`，所以第 7 步放行；安全中止本身在 handler 里完成，不进入板卡命令表。结果事件码取目录的三元组。
* 忙期间 0x07：先调用 `ecsp_slave_set_safe_abort` 注册的序列，且在应答入队之前调用。返回 `ECSP_OK` 则丢弃在途命令（不发结果事件）、应答 `0x00` 并置 `reset_requested`。回调为 NULL 或返回非 `ECSP_OK`：应答 `0x20`、不复位、在途命令继续，并将故障字 bit0 置位；该位由 0 变 1 且上传模式开启时发送一帧 0x11。上传模式关闭时不入队，故障字仍更新，0x12 仍能读到。bit0 同时是表 5-3 的“系统自检失败”：自检以非 0x00 且非 0x21 的结果结束时置位，以 0x00 结束时清除（清除不上报）。通用层没有别的可写故障位。0x0F 在这一步没有放行标志，所以长耗时执行期间应答 0x11，模式不变。会话未建立时 0x0F 应答 0x10，模式也不变。0x0F 不调用身份提交回调。重新初始化把上传模式设回配置初值。心跳和等待握手的 0x01 不看上传模式。
* 回调在 `ecsp_slave_step` 的栈上同步执行，不得再次调用 `ecsp_slave_step`。

## 7 L4 主机链路管理

对每个从机槽位维护两条互不折叠的轴（ADR-0003）：

* **会话状态**：`未连接 / 握手中 / 已握手 / 正常通信 / 版本不符 / 掉线`（与规范表 4-1 一一对应）；
* **事务状态**：`空闲 / 待发送 / 等待应答 / 等待结果`（表 4-2），同一从机同时至多一个在途事务。

```c
typedef enum {
    ECSP_SESS_DISCONNECTED = 0,
    ECSP_SESS_HANDSHAKING,
    ECSP_SESS_HANDSHAKEN,
    ECSP_SESS_NORMAL,
    ECSP_SESS_VERSION_MISMATCH,
    ECSP_SESS_OFFLINE
} ecsp_session_t;

typedef enum {
    ECSP_HOST_EV_PROBE_FAILED = 1, /* 三次握手或三次取版本仍无应答 */
    ECSP_HOST_EV_VERSION_MISMATCH,
    ECSP_HOST_EV_REJECTED,         /* 对端已应答，但应答码不是 0x00；不按超时重发（表 6-1） */
    ECSP_HOST_EV_LINK_FAULT,       /* 整帧放不进发送 FIFO（规范 6.2），与“无应答”分开报 */
    ECSP_HOST_EV_OFFLINE           /* 心跳掉线，或会话建立总时限用尽 */
} ecsp_host_event_t;

typedef void (*ecsp_host_event_cb_t)(ecsp_host_t *h, uint8_t board_addr,
                                     ecsp_host_event_t event, void *user);
typedef enum {
    ECSP_TXN_IDLE = 0,
    ECSP_TXN_QUEUED,
    ECSP_TXN_WAIT_RESP,
    ECSP_TXN_WAIT_RESULT
} ecsp_txn_state_t;

typedef void (*ecsp_txn_cb_t)(ecsp_host_t *h, uint8_t board_addr, uint8_t cmd,
                              int status, uint8_t response_code,
                              const uint8_t *payload, uint16_t len, void *user);

typedef struct {
    uint8_t        board_addr;
    ecsp_version_t expect_version; /* 固化的 7 字节期望版本，规范 9.2 */
} ecsp_host_slave_cfg_t;

typedef struct {
    uint8_t product_addr;
    const ecsp_host_slave_cfg_t *slaves;
    size_t slave_count;            /* 不超过 ECSP_HOST_SLAVES */
    uint8_t *rx_buf;
    size_t   rx_cap;               /* 至少 ECSP_MAX_FRAME */
    uint8_t *tx_buf;
    size_t   tx_cap;               /* 至少一帧最小帧；装不下的业务帧在 submit 返回 ECSP_E_CAP */
    ecsp_host_event_cb_t on_event; /* 可为 NULL */
    void *user;
} ecsp_host_cfg_t;

int  ecsp_host_init(ecsp_host_t *h, const ecsp_host_cfg_t *cfg);
int  ecsp_host_submit(ecsp_host_t *h, uint8_t board_addr, uint8_t cmd,
                      const uint8_t *payload, uint16_t len,
                      ecsp_txn_cb_t cb, void *user);
int  ecsp_host_step(ecsp_host_t *h, uint32_t now_ms);
void ecsp_host_rx(ecsp_host_t *h, const uint8_t *data, size_t len);
ecsp_session_t ecsp_host_session(const ecsp_host_t *h, uint8_t board_addr);
ecsp_txn_state_t ecsp_host_txn(const ecsp_host_t *h, uint8_t board_addr);
size_t ecsp_host_tx_pending(const ecsp_host_t *h);
void ecsp_host_tx_drain(ecsp_host_t *h, size_t n);
```

`ecsp_host_submit` 在会话不是“正常通信”时返回 `ECSP_E_STATE`，不入队、不发帧。会话为“正常通信”且该从机已确认低功耗时，除 0x0B（进入/退出）与 0x07 外的业务命令返回 `ECSP_E_LOW_POWER`，不入队、不发帧（与 `ECSP_E_STATE` 区分）。会话为“正常通信”且当前事务空闲时，把业务命令标为“待发送”；下一拍 `ecsp_host_step` 才写入发送 FIFO 并进入“等待应答”。同一从机同时只保留一个在途事务，第二个 `submit` 返回 `ECSP_E_CAP`。事务在途时会话状态保持“正常通信”。`cb` 在事务结束时于 step 栈上调用恰好一次：应答到达为 `ECSP_OK`（`payload` 不含应答码），重发耗尽为 `ECSP_E_TIMEOUT`，心跳掉线取消在途事务为 `ECSP_E_OFFLINE`，发送 FIFO 装不下为 `ECSP_E_LINK`。step 进行中提交的请求留到下一次 step 再发，多台从机按提交先后写入发送 FIFO。收到进入低功耗的 0x00 后主机暂停该从机掉线判定；退出 0x00、握手重建或复位清掉该暂停。

**上电与掉线（#9、#10）**：第一次 `ecsp_host_step` 对每个已配置从机主动下发 0x01，会话进入“握手中”。应答时限取目录里 0x01 的毫秒数（`ECSP_RESP_DEFAULT_MS`，50 ms），含初发最多 3 次，重发字节与初发相同。三次仍无应答则回到“未连接”并上报 `ECSP_HOST_EV_PROBE_FAILED`，进入被动等待：收到该从机的空载荷 0x01 上报后，同一拍重新下发握手请求。自第一次 step 起超过 `ECSP_SESSION_SETUP_MS` 仍未进入“正常通信”或“版本不符”，会话改为“掉线”并上报 `ECSP_HOST_EV_OFFLINE`。应答码不是 0x00 则停止并上报 `ECSP_HOST_EV_REJECTED`，不把这次应答当成静默再重发。发送 FIFO 装不下这一帧则上报 `ECSP_HOST_EV_LINK_FAULT`。收到 0x00 后会话为“已握手”，同一拍下发 0x02；0x02 的时限取目录里的 50 ms，三次无应答回到“未连接”并上报失败。版本 7 字节相等进入“正常通信”，不相等停在“版本不符”。进入“正常通信”后，业务命令按目录里该命令的应答时限重发，主机不读命令类别。含初发最多 3 次，重发复用提交时的载荷（事务序号不变）；次数用尽则会话“掉线”、事务回“空闲”。目录没有的命令（板卡命令）用 `ECSP_RESP_UNLISTED_MS`（100 ms）。设置硬件版本（0x03）与设置序列号（0x05）的时限是目录里的 3 s，不是 50 ms。0x08 的受理应答和 0x0B 是 50 ms。0x07 是它自己的 50 ms（`ECSP_RESP_RESET_MS`）。0x0C 的第一次应答是 500 ms。应答一旦到达事务立即结束。等待 0x03/0x05 时，3 s 掉线判据不截断那 3 次重发；三次仍无应答才以 `ECSP_E_TIMEOUT` 结束。应答码不是 0x00 则结束事务且不重发。长耗时命令的结果事件和获取结果取目录的三元组：0x08 对应 0x0A 与 0x09，0x0C 对应 0x0E 与 0x0D。连续 `3 * ECSP_HEARTBEAT_MS` 未收到该从机任何有效帧同样“掉线”；若当时有在途事务，回调只触发这一次，不再另发一帧重试。掉线后业务命令被本地拒绝。收到握手上报才重新探测，版本核对通过后才回到“正常通信”。一台从机掉线不影响其它槽位。

**上传模式**：主机链路不保存这个开关。0x0F 的静默上限是目录里的 50 ms，不是 3 s。长耗时命令受理后，即使结果事件不会到来，主机仍等到 `T_exec_max` 才发配对的获取结果，不提前轮询。

**回调语义**（ADR-0005，#11）：

* 回调在 `ecsp_host_step` 的调用栈上同步执行，每个事务恰好一次（成功、超时、掉线都只这一次）；
* 回调内**允许** `ecsp_host_submit`（只入队，不推进状态机）；同一回调里提交的多条请求，下一次 step 按提交顺序发出；
* 回调内**禁止**再次调用同一个主机的 `ecsp_host_step`：返回 `ECSP_E_REENT`，不推进，会话状态与重发计数都不变。另一个主机上下文不共享这面标志，回调里仍可推进它。

`ecsp_host_step` 正常返回 `ECSP_OK`。`h` 为 NULL 时返回 `ECSP_E_ARG`。

**上电流程**（规范 4.1.1）：主动握手探测 3 次 → 失败则被动等待上报 → 收到 0x01 重入探测 → 超过会话建立总时限按掉线处理；握手成功后取版本并逐字节比较，不等则停在 `版本不符`。

**重发**：同一事务超时重发，含初发共 3 次，`attempt` 与 deadline 挂在事务上而非状态上；重发复用原事务序号。

**掉线判定**：连续 3 个心跳周期未收到该从机任何有效帧 → `掉线`；恢复通信后必须重新握手，不得直接恢复业务。设置硬件版本（0x03）或设置序列号（0x05）正在等待应答时，这 3 次各 3 s 的重发不被该判据截断。已确认低功耗的从机暂停该判据，直至退出或会话重建。

## 8 时序

| 项 | 默认 | 来源 |
| --- | --- | --- |
| 获取类、握手、0x08 受理、0x0B、0x0F | ≤50 ms | 规范 7.1，目录条目 `ECSP_RESP_DEFAULT_MS` |
| 0x07 | ≤50 ms | 规范 7.1，目录条目 `ECSP_RESP_RESET_MS` |
| 设置硬件版本 / 设置序列号首次应答 | ≤3 s | 规范 7.1，目录条目 `ECSP_RESP_IDENTITY_MS` |
| 目录没有的命令 | ≤100 ms | `ECSP_RESP_UNLISTED_MS` |
| 在线升级首次应答 | ≤500 ms | 规范 7.1，目录条目 |
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

CRC 字节在文件里写成实际值，不写 `CRC16` 占位——向量必须是可直接断言的字节。上面的 `version_resp` 不是随手选的：它的 CRC 低字节恰好是 `0xFA`，扫描器若在 CRC 区里重新找帧头就会把这一帧切坏。`tools/check-vectors.ps1` 负责四件事：重新计算每个向量的 CRC 并比对；扫描 `ecsp-spec.md` 中写死具体字节的示例帧，报告"规范里有、向量里没有"的遗漏；命令节的时序若整段写成「同某一节」，而被指向的那节声明了和本命令类别不同的时限，则失败；重算 `docs/specs/boards/` 里括号内全是十六进制的 `[FA]…[0D]` 示例帧 CRC。含 `CRC16`、`P`、`SYNC` 这类占位的帧跳过，章节覆盖仍只统计通用规范。

**三层测试**：

1. `test/unit/`：逐函数，输入输出确定性；
2. `test/unit/test_pipeline.c`：同一帧同时命中多个拒绝条件时返回哪个应答码（这是 ADR-0002 的可执行表述）；
3. `test/integ/two_instance.c`：一个主机上下文 + 一个从机上下文，经虚拟链路（字节数组 + 丢包/超时注入）跑完握手→版本核对→业务命令→掉线→重连。向从机注入原始字节后须再推一拍主机 `step`，会话轴才会看见那帧；主机事务非空闲时用注入造从机忙，不要再 `submit`。

**每个 PR 的三件套**（ADR-0001）：规范正文 diff（若行为有变）+ 向量 + 实现。缺一不合并。

## 10 完成判据

一张票算完成，需要同时满足：

* 运行 `tools/check.ps1`。它执行板卡锚点与通用命令漂移检查、`cmake --build` 再执行 `ctest`，警告即失败（`-Wall -Wextra -Werror`，MSVC 用 `/W4 /WX`）。推送和拉取请求由 GitHub Actions 跑同一脚本；本地可用 `tools/install-hooks.ps1` 把该检查接到 `pre-commit`；
* 新增行为有对应单测，且失败时给出可读断言信息；
* 若行为在规范中有对应条款，向量表里有一行指向该条款；
* 不引入 `malloc`、全局变量、时钟调用。

## 11 不做清单

* 不做广播/组播寻址（规范 2.1：通用层不定义）；
* 不做 0x13~0x2F 的任何命令（附录 B 待评审）；
* 不在库里做浮点解析（载荷字节序由使用方按 3.3 处理）；
* 不实现升级数据通道（规范 5.2.12 明确由产品规范定义），只实现会话级控制与结果码；
* 不提供日志、断言、错误字符串——体积敏感，交给使用方。
