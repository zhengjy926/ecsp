# ECSP

设备组串行协议的 C99 实现。一条链路上一台主机、若干从机；同一份库可以编进从机固件，也可以编进主机。协议正文是 [`docs/specs/ecsp-spec.md`](docs/specs/ecsp-spec.md)，本文件只说明怎么把库接到产品上。

库负责帧、通用命令（0x01–0x12）、从机会话、事务重放、主机重发和掉线判定。板卡命令（0x30–0xEF）、UART、时钟、复位、低功耗硬件、升级数据通道都由产品提供。库不调用 `malloc`，不读时钟，内部没有锁。

词汇以 [`GLOSSARY.md`](GLOSSARY.md) 为准。实现分层见 [`docs/specs/ecsp-impl-spec.md`](docs/specs/ecsp-impl-spec.md)。

## 先选角色

| 角色 | 编进固件的层 | 头文件 | 产品要做的事 |
| --- | --- | --- | --- |
| 从机 | L1–L3 | `ecsp/slave.h` | 收字节、推进、把待发字节交给 UART，并实现板卡命令 |
| 主机 | L1、L2、L4 | `ecsp/host.h` | 收字节、推进、把待发字节交给 UART，并在会话就绪后提交命令 |
| 两条链路、角色不同 | 两个上下文 | 两个头都要 | 每条链路一套缓冲、一套 `step` |

从机固件用 `-DECSP_ENABLE_HOST=0`，主机侧用 `-DECSP_ENABLE_SLAVE=0`。默认两个开关都是 1。L3 与 L4 互不引用，一个进程里可以同时放一个主机上下文和一个从机上下文。

推荐物理层（两端必须相同，具体取值写在板卡规范里）：UART、115200、8 数据位、1 停止位、无校验、无流控。

## 编译

头文件目录是 `include/`。对外只需要：

```c
#include "ecsp/ecsp.h"   /* 从机、主机、帧、命令都在这里 */
```

只做从机时包含 `ecsp/slave.h` 即可，只做主机时包含 `ecsp/host.h`。

源文件：

| 始终编译 | 从机再加 | 主机再加 |
| --- | --- | --- |
| `src/ecsp/core/bytes.c` | `src/ecsp/slave/engine.c` | `src/ecsp/host/link.c` |
| `src/ecsp/core/crc.c` | `src/ecsp/slave/session.c` | |
| `src/ecsp/core/frame.c` | `src/ecsp/slave/txn_cache.c` | |
| `src/ecsp/core/cmds.c` | `src/ecsp/slave/long_run.c` | |

C99，无编译器扩展。仓库里的 `CMakeLists.txt` 只用来跑本机测试（`cmake -S . -B build && cmake --build build && ctest --test-dir build`），产品工程直接把上表源文件加进去即可。

可覆盖的编译期宏都在 `include/ecsp/config.h`，用 `-D` 覆盖。从机和主机若改了 `ECSP_HEARTBEAT_MS`，两边必须是同一个值：主机的掉线门限是三个心跳周期。时间宏只允许放宽，见文末「时间」。

## 两边共用的接法

每个链路一个上下文，放静态存储。上下文里嵌着事务缓冲（默认从机缓存 4 帧，主机 8 个从机槽，每槽一帧载荷），不要放进很小的任务栈。

接收缓冲、发送缓冲、命令表、身份存储都由调用方持有，并且要比上下文活得久。库只保存指针。

```
接收中断或 DMA 完成
        |
        v
   ecsp_*_rx(字节)          中断里只做这一件事
        |
主循环或同一个任务，建议每 <= 10 ms 一次
        |
        +-- ecsp_*_step(now_ms)     now_ms 是调用方的毫秒时钟，回绕安全
        +-- 把 tx_buf 开头的待发字节交给 UART
        +-- ecsp_*_tx_drain(已交出的字节数)
        +-- 从机：看复位 / 低功耗 / 长耗时是否做完
        +-- 主机：会话就绪且该从机事务空闲时再 submit
```

`step` 过大时，50 ms 的应答时限会被判得比较粗，由产品自己保证节拍。库内部没有临界区。接收中断若会打断 `step` 或 `tx_drain`，产品在这两处调用期间关掉该接收中断，或者中断只写入自己的环形缓冲，回到主循环后再调用 `ecsp_*_rx`。

发送缓冲是一条 FIFO，下标 0 是最老的字节。交出 `n` 字节后调用 `tx_drain(n)`，库把剩余字节前移。`tx_pending` 为 0 只说明 FIFO 空了；复位和睡眠还要等 UART 移位寄存器也发完。

放不进接收缓冲的字节会被丢掉。接收缓冲至少 `ECSP_MAX_FRAME`（默认 128）字节；参考固件用了两倍，给 `step` 晚到时留余量。

## 从机

上电后从机处于等待握手。它以 1 Hz 主动发送空载荷的握手上报（0x01），直到主机的握手请求被接受。就绪之后，距上次发出任意帧满 `ECSP_HEARTBEAT_MS`（默认 1000 ms）再发一帧心跳（0x10）。第一次 `ecsp_slave_step` 只锁存时钟，第一帧周期上报在一个完整周期之后。

业务命令在握手成功之前会得到应答码 0x10。地址不匹配或 CRC 错误的帧被静默丢掉。

### 初始化

身份、软件版本、故障字初值、上传模式、板卡命令表和两块缓冲都在配置里给出。`upload_mode` 只能是 `ECSP_UPLOAD_ON` 或 `ECSP_UPLOAD_OFF`；产品没有另行声明时填 `ECSP_UPLOAD_ON`。填 0 会使 `ecsp_slave_init` 返回 `ECSP_E_ARG`。

```c
#include "ecsp/slave.h"

#define PRODUCT_ADDR 0x03u
#define BOARD_ADDR   0x02u

static uint8_t g_rx[ECSP_MAX_FRAME * 2u];
static uint8_t g_tx[ECSP_MAX_FRAME * 2u];
static ecsp_slave_t g_slave;
static ecsp_slave_identity_t g_identity;

static const uint8_t k_sw[ECSP_VERSION_LEN] = {
    '1', '.', '0', '.', '0', '.', '0'   /* 定长 7 字节，没有结尾 '\0' */
};

static int on_identity_commit(ecsp_slave_t *s,
                              const ecsp_slave_identity_t *next)
{
    (void)s;
    return nvm_write(next);             /* 落盘成功才返回 ECSP_OK */
}

static int on_safe_abort(ecsp_slave_t *s, uint8_t cmd)
{
    (void)s;
    (void)cmd;
    actuator_park();                    /* 执行器进入安全态后返回 ECSP_OK */
    return ECSP_OK;
}

static void slave_init(void)
{
    ecsp_slave_cfg_t cfg;
    const ecsp_cmd_table_t *board = board_cmds();

    memset(&cfg, 0, sizeof cfg);
    cfg.product_addr = PRODUCT_ADDR;
    cfg.board_addr = BOARD_ADDR;
    memcpy(cfg.version.b, k_sw, ECSP_VERSION_LEN);
    cfg.identity = &g_identity;         /* 硬件版本 + 序列号，调用方持有 */
    cfg.fault_word = 0u;
    cfg.upload_mode = ECSP_UPLOAD_ON;
    cfg.cmds = *board;
    cfg.rx_buf = g_rx;
    cfg.rx_cap = sizeof g_rx;
    cfg.tx_buf = g_tx;
    cfg.tx_cap = sizeof g_tx;

    if (ecsp_slave_init(&g_slave, &cfg) != ECSP_OK) {
        /* 缓冲太小、身份指针为空、命令表不一致，或上传模式取值非法 */
    }
    ecsp_slave_set_identity_commit(&g_slave, on_identity_commit);
    ecsp_slave_set_safe_abort(&g_slave, on_safe_abort);
}
```

`ecsp_slave_init` 会清空事务缓存并回到等待握手。软复位在本库里就是再次调用它。重新初始化也会清掉上面两个回调，所以复位后的初始化路径要再注册一次。

配置结构本身可以是栈上的临时变量。`identity`、两块缓冲、命令表里的函数指针指向的表必须一直有效。

发送缓冲要装得下通用命令和板卡表里声明的最大应答。装不下时 `ecsp_slave_init` 返回 `ECSP_E_ARG`。

### 主循环

参考固件是 `board/f429/fw/main.c`：USART 接收中断每字节调用 `ecsp_slave_rx`，SysTick 置标志，主循环里 `step` 并排空发送 FIFO。

```c
static void on_uart_rx(uint8_t byte)
{
    ecsp_slave_rx(&g_slave, &byte, 1u);
}

static void drain_tx(void)
{
    while (ecsp_slave_tx_pending(&g_slave) > 0u && uart_tx_ready()) {
        uart_write(g_tx[0]);
        ecsp_slave_tx_drain(&g_slave, 1u);
    }
}

void poll(uint32_t now_ms)
{
    ecsp_slave_step(&g_slave, now_ms);
    drain_tx();

    /* 0x07 的应答已经入队。FIFO 和移位寄存器都空了再复位。 */
    if (ecsp_slave_reset_requested(&g_slave) && uart_tx_complete()) {
        hardware_reset();
    }

    /* 0x0B 进入低功耗后库不再发心跳。链路空闲再进硬件睡眠。 */
    if (ecsp_slave_low_power_mode(&g_slave) == ECSP_LOW_POWER_ENTER &&
        ecsp_slave_tx_pending(&g_slave) == 0u && uart_tx_complete()) {
        hardware_sleep();
    }

    run_long_command_if_any();
}
```

`ecsp_slave_step` 在自己的栈上跑命令处理函数、安全中止回调和身份提交回调。这些回调里再调用 `ecsp_slave_step` 会被直接丢掉，外层那一拍继续做完。回调里可以读状态、做硬件操作，把耗时工作记下来留给主循环。

### 板卡命令

0x30–0xEF 放在产品自己的 `const` 表里，协议库不包含板卡语义。表里没有的命令码，从机应答 0x04。`board/f429/bringup.c` 是一份可编译的例子。新板卡规范可以用 `tools/new-board-spec.ps1` 生成骨架。

处理函数只填写应答**数据**，返回值就是应答码。返回值是 `ECSP_RS_OK` 时数据才会发出；其它应答码只有应答码本身。管线在调用处理函数之前已经检查过地址、命令、会话、长度、事务缓存和忙状态，处理函数不要再做这些判断。

```c
static int on_get_test(ecsp_slave_t *s, const ecsp_frame_t *req,
                       uint8_t *resp, size_t cap, size_t *resp_len)
{
    (void)s;
    (void)req;
    (void)cap;
    resp[0] = g_test_byte;
    *resp_len = 1u;
    return ECSP_RS_OK;
}

static int on_set_test(ecsp_slave_t *s, const ecsp_frame_t *req,
                       uint8_t *resp, size_t cap, size_t *resp_len)
{
    (void)s;
    (void)resp;
    (void)cap;
    g_test_byte = req->payload[0];
    *resp_len = 0u;
    return ECSP_RS_OK;
}

static int on_bump(ecsp_slave_t *s, const ecsp_frame_t *req,
                   uint8_t *resp, size_t cap, size_t *resp_len)
{
    (void)s;
    (void)req;                          /* payload[0] 是事务序号，重放时不会进这里 */
    (void)cap;
    g_counter++;
    ecsp_wr_u32le(resp, g_counter);     /* 多字节字段用小端助手 */
    *resp_len = 4u;
    return ECSP_RS_OK;
}

static const ecsp_cmd_entry_t g_table[] = {
    /* cmd, flags, req_len, resp_max, validate, handler, resp_ms */
    { 0x31, ECSP_CMD_QUERY, 0u, 1u, NULL, on_get_test, 0u },
    { 0x32, 0u,             1u, 0u, NULL, on_set_test, 100u },
    { 0x33, ECSP_CMD_TXN_SEQ, 1u, 4u, NULL, on_bump, 100u },
};

static const ecsp_cmd_table_t g_cmds = {
    g_table, sizeof g_table / sizeof g_table[0]
};
```

标志决定这条命令在「长耗时执行中」是否还能被受理：

| 标志 | 含义 |
| --- | --- |
| `ECSP_CMD_QUERY` | 获取类。忙时仍受理，不打断正在执行的命令 |
| `ECSP_CMD_TXN_SEQ` | 非幂等动作。载荷第 0 字节是事务序号，应答按 (命令, 序号) 缓存；相同序号的重传回放原应答，处理函数只跑一次 |
| `ECSP_CMD_IDEMPOTENT` | 幂等动作。无事务序号。与 `ECSP_CMD_TXN_SEQ` 互斥 |
| `ECSP_CMD_PREEMPTIVE` | 忙时仍受理，并且可以打断在途命令（停止控制走这条） |
| `ECSP_CMD_CONCURRENT` | 忙时仍受理，不打断在途命令 |

一条命令至多带 `ECSP_CMD_IDEMPOTENT` 和 `ECSP_CMD_TXN_SEQ` 中的一个。没带上面任一放行标志的动作或设置，在长耗时执行期间应答 0x11。

`req_len` 是请求载荷的精确字节数。变长载荷填 `ECSP_CMD_LEN_ANY`，在 `validate` 里返回 `ECSP_E_LEN`（应答 0x02）或 `ECSP_E_VAL`（应答 0x03）。`resp_max` 是应答数据的上限，不含应答码。`resp_ms` 是主机等待这条命令的静默上限；0 表示用 50 ms 默认值。从机自己不读 `resp_ms`。

### 长耗时：自检与升级

0x08（自检）和 0x0C（升级）由库受理：先应答 `0x00` 加上 `T_exec_max`（u16 小端，毫秒），再由产品执行。同一时刻只有一条在执行。`ecsp_slave_long_run_cmd` 返回正在执行的命令码，空闲时返回 0。

```c
static void run_long_command_if_any(void)
{
    uint8_t cmd = ecsp_slave_long_run_cmd(&g_slave);
    uint8_t result;

    if (cmd == 0u) {
        return;
    }
    if (!work_finished(cmd, &result)) {
        return;                         /* 0x11 表示仍在执行，不能当作完成码 */
    }
    (void)ecsp_slave_long_run_complete(&g_slave, result);
}
```

下一次 `step` 在上传模式开启时发出一次结果事件：自检是 0x0A，升级是 0x0E。这一帧不重传。上传模式关闭时，这次才就绪的事件不入队，主机稍后用 0x09 或 0x0D 仍能读到结果。再次打开上传模式、在上传模式仍关闭时退出低功耗、或重新握手把上传模式设回上电初值，都不会补发这些没入队的事件。上传模式开启时已经排上、只因发送 FIFO 满或当时处于低功耗而还没发出的帧，之后照常发出，包括握手之后。已经进入发送 FIFO 的字节不会撤回。

主机发来停止（控制字节 `ECSP_CTRL_STOP`，新的事务序号）时，库先调用 `on_safe_abort`，把结果记为 0x21，并排队结果事件。产品看到 `ecsp_slave_long_run_cmd` 变为 0 就停手，不必再调用 `ecsp_slave_long_run_complete`。

到达 `T_exec_max` 不会自动完成，也不会自动失败。超时之后主机来取结果，从机仍应答「仍在执行」（结果码 0x11），直到产品调用 `ecsp_slave_long_run_complete` 或主机停止它。

升级的分包、校验和签名由产品规范定义。库只做启动、停止和结果码。自检最长执行时间默认 2000 ms（`ECSP_SELFTEST_EXEC_MAX_MS`），升级默认 60000 ms（`ECSP_UPGRADE_EXEC_MAX_MS`），都要放进产品规范。

### 复位、低功耗、身份、上传模式

**复位（0x07）。** 库把应答放进发送 FIFO，并让 `ecsp_slave_reset_requested` 返回 1。产品排空 FIFO 和 UART 之后做硬件复位。复位后的上电路径再次调用 `ecsp_slave_init` 并重新注册回调。复位真正发生之前，同一事务序号的 0x07 重传会回放原来的应答。若当时有长耗时命令，库先调用安全中止：回调返回 `ECSP_OK` 才应答 0x00 并请求复位；回调缺失或失败则应答 0x20，命令继续执行，并把故障字 bit0 置位。

**低功耗（0x0B）。** 载荷 1 字节：`ECSP_LOW_POWER_ENTER`（0x01）或 `ECSP_LOW_POWER_EXIT`（0x00）。进入之后库不发心跳，也不把已经排上的故障上报和结果事件交给发送 FIFO。产品用 `ecsp_slave_low_power_mode` 决定何时进硬件睡眠。下一次被接受的握手，或退出命令，都会恢复心跳；那时已经排上的帧可以发出。上传模式关闭期间才就绪、因而没有排上的帧仍然不发。低功耗期间心跳仍然不发，上传模式不能代替低功耗。

**身份（0x03 / 0x05）。** 硬件版本是 7 字节 `X.Y.Z.N`，序列号是 `SN` 加 10 位数字。格式用 `ecsp_version_parse` / `ecsp_serial_parse` 检查。注册了提交回调时，只有回调返回 `ECSP_OK`，库才更新 `identity` 并应答 0x00；其它返回值应答 0x01，存储保持原样。未注册回调时，成功的命令只改 RAM 并应答 0x00。这两条没有事务序号，内容相同的重发会再次调用提交回调。

**上传模式（0x0F）。** 关闭时不发 0x0A、0x0E、0x11。心跳和等待握手期间的 0x01 照常发送。上电值来自配置里的 `upload_mode`；从机再次接受握手请求时回到这个初值，即使因此从关闭变回开启，关闭期间没入队的那三帧也不补发。本命令不写非易失存储，应答时限是 50 ms。

**故障字。** 0x12 读的是上下文里的故障字。上电初值来自 `cfg.fault_word`。bit0（`ECSP_FAULT_SELFTEST`）固定为系统自检失败，只有通用层改它：自检以非 0x00 且非 0x21 的结果结束时置位，以 0x00 结束时清除；复位无法安全中止时也会置位。bit1–bit7 由板卡规范定义，用 `ecsp_slave_set_fault_bits` 写入。掩码用 `ECSP_FAULT_BOARD_MASK` 或其子集；掩码为 0 或含有 bit0 时调用失败，故障字不变。选中的位从 0 变为 1，且上传模式开启时，下一次 `step` 发一帧 0x11，载荷是变化后的整个故障字。只清除不上报。上传模式关闭时字仍更新，0x11 不入队，0x12 仍能读到。

```c
/* Board bit2 rose. The mask does not include bit0. */
if (ecsp_slave_set_fault_bits(&g_slave, 0x04u, 0x04u) == ECSP_OK) {
    ecsp_slave_step(&g_slave, now_ms);
}
```

## 主机

主机不解析业务本身。它按配置里的从机表做握手和版本核对，把 `ecsp_host_submit` 提交的请求发出去，超时则用同一载荷重发，含初发最多 3 次，并在事务结束时调用一次回调。

每台从机有两条独立的轴，用 `ecsp_host_session` 和 `ecsp_host_txn` 读：

| 会话 | 含义 |
| --- | --- |
| `ECSP_SESS_DISCONNECTED` | 未连接。三次握手无应答后停在这里，等从机的握手上报 |
| `ECSP_SESS_HANDSHAKING` | 已发出 0x01，在等应答 |
| `ECSP_SESS_HANDSHAKEN` | 握手应答 0x00 已到，正在读软件版本 |
| `ECSP_SESS_NORMAL` | 版本逐字节一致。只有这时业务命令才会离开发送队列 |
| `ECSP_SESS_VERSION_MISMATCH` | 版本不一致。提交被本地拒绝 |
| `ECSP_SESS_OFFLINE` | 三个心跳周期内没有任何有效帧，或 5 s 内没建立好会话 |

| 事务 | 含义 |
| --- | --- |
| `ECSP_TXN_IDLE` | 可以提交下一条 |
| `ECSP_TXN_QUEUED` | 已提交，下一拍 `step` 才写入发送 FIFO |
| `ECSP_TXN_WAIT_RESP` | 已发送，含超时重发 |
| `ECSP_TXN_WAIT_RESULT` | 长耗时命令已受理，在等结果事件 |

一台从机同时只有一个在途事务。一台掉线不影响其它槽位。

### 初始化

`slaves` 在 `ecsp_host_init` 时被复制，之后可以释放。`board_cmds` 是一整张 `ecsp_cmd_table_t`：条目数组不复制。`table` 为 NULL 且 `table_len` 为 0 时，每条板卡命令都按 50 ms 等待；有表时数组要一直有效。主机只读 `cmd` 和 `resp_ms`，处理函数可以是 NULL。`table` 为空而 `table_len` 非 0 时，`ecsp_host_init` 返回 `ECSP_E_ARG`。`expect_version` 是产品要求的 7 字节软件版本，按原始字节比较。

```c
#include "ecsp/host.h"

static uint8_t g_rx[ECSP_MAX_FRAME * 2u];
static uint8_t g_tx[ECSP_MAX_FRAME * 2u];
static ecsp_host_t g_host;

static const ecsp_host_slave_cfg_t g_slaves[] = {
    { 0x02u, { { '1', '.', '0', '.', '0', '.', '0' } } },
    { 0x03u, { { '1', '.', '0', '.', '0', '.', '0' } } },
};

static void on_link_event(ecsp_host_t *h, uint8_t board_addr,
                          ecsp_host_event_t event, void *user)
{
    (void)h;
    (void)user;
    log_link(board_addr, event);        /* 探测失败、版本不符、被拒绝、链路故障、掉线 */
}

static void host_init(void)
{
    ecsp_host_cfg_t cfg;

    memset(&cfg, 0, sizeof cfg);
    cfg.product_addr = 0x03u;
    cfg.slaves = g_slaves;
    cfg.slave_count = sizeof g_slaves / sizeof g_slaves[0];
    cfg.rx_buf = g_rx;
    cfg.rx_cap = sizeof g_rx;           /* 至少 ECSP_MAX_FRAME */
    cfg.tx_buf = g_tx;
    cfg.tx_cap = sizeof g_tx;           /* 至少装得下要发的最大帧 */
    cfg.on_event = on_link_event;
    cfg.board_cmds = *board_cmds();     /* 没有表时写成 { NULL, 0 } */

    if (ecsp_host_init(&g_host, &cfg) != ECSP_OK) {
        /* 槽位超限、地址重复、缓冲过小，或命令表指针与长度矛盾 */
    }
}
```

从机槽位数不能超过 `ECSP_HOST_SLAVES`（默认 8）。同一上下文里板卡地址不能重复。

### 主循环

第一次 `ecsp_host_step` 对每台已配置从机发送握手请求。应答 0x00 之后，同一拍发送获取软件版本（0x02）。版本一致则进入正常通信。三次握手或三次读版本都没有应答时，会话回到未连接，并回调 `ECSP_HOST_EV_PROBE_FAILED`；之后收到该从机的空载荷 0x01 上报，主机会再探一次。自第一次 `step` 起超过 `ECSP_SESSION_SETUP_MS`（默认 5000 ms）仍未进入正常通信或版本不符，会话变为掉线。

```c
static void on_uart_rx(const uint8_t *data, size_t len)
{
    ecsp_host_rx(&g_host, data, len);
}

static void drain_tx(void)
{
    size_t n = ecsp_host_tx_pending(&g_host);
    if (n > 0u && uart_write(g_tx, n)) {
        ecsp_host_tx_drain(&g_host, n);
    }
}

void poll(uint32_t now_ms)
{
    (void)ecsp_host_step(&g_host, now_ms);
    drain_tx();

    if (ecsp_host_session(&g_host, 0x02u) == ECSP_SESS_NORMAL &&
        ecsp_host_txn(&g_host, 0x02u) == ECSP_TXN_IDLE) {
        submit_next_command();
    }
}
```

链路事件回调在 `ecsp_host_step` 的栈上执行，描述的是调用方没有提交、由链路自己得出的结论：

| 事件 | 何时 |
| --- | --- |
| `ECSP_HOST_EV_PROBE_FAILED` | 三次握手或三次读版本都没有应答 |
| `ECSP_HOST_EV_VERSION_MISMATCH` | 0x02 回来的 7 字节与期望版本不同 |
| `ECSP_HOST_EV_REJECTED` | 对端有应答，但应答码不是 0x00。库按缺陷处理，不再把这次当成静默重发 |
| `ECSP_HOST_EV_LINK_FAULT` | 整帧放不进发送 FIFO。这是本端发送路径满了 |
| `ECSP_HOST_EV_OFFLINE` | 心跳掉线，或会话建立总时限用尽 |

心跳（0x10）、故障上报（0x11）和握手上报被库消费，用来刷新存活时间和重建会话。它们没有单独的应用回调。要读故障字，在正常通信且事务空闲时提交 0x12。

### 提交一条命令

```c
static void on_fault(ecsp_host_t *h, uint8_t board_addr, uint8_t cmd,
                     int status, uint8_t response_code,
                     const uint8_t *payload, uint16_t len, void *user)
{
    (void)h;
    (void)board_addr;
    (void)cmd;
    (void)user;
    if (status == ECSP_OK && response_code == ECSP_RS_OK && len >= 1u) {
        remember_fault(payload[0]);     /* 应答码已剥掉，payload[0] 是故障字 */
    }
}

static void submit_next_command(void)
{
    int rc = ecsp_host_submit(&g_host, 0x02u, ECSP_CMD_GET_FAULT,
                              NULL, 0u, on_fault, NULL);
    (void)rc;
}
```

`payload` 在回调返回后就失效，需要的字节在回调里拷走。回调对每个事务恰好执行一次。

`ecsp_host_submit` 的返回值：

| 返回值 | 含义 |
| --- | --- |
| `ECSP_OK` | 已排队。下一拍 `step` 发送；若这次调用发生在 `step` 的回调里，则留到再下一拍 |
| `ECSP_E_STATE` | 会话还不是正常通信，没有发帧 |
| `ECSP_E_LOW_POWER` | 该从机已确认进入低功耗。这时只有 0x0B 和 0x07 可以提交 |
| `ECSP_E_CAP` | 该从机已有在途事务，或这一帧比发送缓冲大 |
| `ECSP_E_ARG` | 主机、地址或载荷指针非法 |

回调里可以再 `ecsp_host_submit`（只入队）。回调里再调用**同一个**主机的 `ecsp_host_step` 会得到 `ECSP_E_REENT`，状态机不动。另一个主机上下文有自己的标志，可以在回调里推进。

事务回调里的 `status` 是库的本地结果，`response_code` 才是帧里的应答码：

| `status` | 含义 |
| --- | --- |
| `ECSP_OK` | 应答到了。`payload` 是应答码之后的数据 |
| `ECSP_E_TIMEOUT` | 含初发 3 次都没有应答 |
| `ECSP_E_OFFLINE` | 等待期间会话掉线，在途事务被取消 |
| `ECSP_E_LINK` | 发送 FIFO 装不下这一帧 |
| `ECSP_E_RESET` | 在途命令被复位打断。结果事件不会再来，结局以后面的查询为准 |

应答码不是 0x00 时，库结束事务并且不重发。常见应答码：0x02 长度错误，0x03 参数非法，0x04 命令未定义，0x10 尚未握手，0x11 正忙，0x13 前提不成立，0x20 执行失败，0x21 已被主机中止。完整表在规范 3.6。

### 命令载荷怎么填

事务序号由调用方放进载荷。库重发时原样使用提交时的字节，不会分配下一个序号。新的一次非幂等动作要换一个新序号。

| 命令 | 载荷 |
| --- | --- |
| 0x02 软件版本、0x04 硬件版本、0x06 序列号、0x09 自检结果、0x0D 升级结果、0x12 故障 | 空 |
| 0x03 设置硬件版本 | 7 字节 `X.Y.Z.N` |
| 0x05 设置序列号 | 12 字节 `SN` + 10 位数字 |
| 0x07 复位 | 1 字节事务序号 |
| 0x08 自检、0x0C 升级 | 事务序号 + 控制字节（`ECSP_CTRL_START` 0x00，`ECSP_CTRL_STOP` 0x01） |
| 0x0B 低功耗 | 1 字节模式（`ECSP_LOW_POWER_EXIT` / `ECSP_LOW_POWER_ENTER`） |
| 0x0F 上传模式 | 1 字节（`ECSP_CTRL_UPLOAD_OFF` 0x00，`ECSP_CTRL_UPLOAD_ON` 0x01） |
| 板卡命令 | 按板卡规范。带 `ECSP_CMD_TXN_SEQ` 时第 0 字节是事务序号 |

静默上限（提前到达的应答会立刻结束等待）：

| 命令 | 默认上限 |
| --- | --- |
| 握手、获取类、0x08 的受理、0x0B、0x0F | 50 ms，`ECSP_RESP_DEFAULT_MS` |
| 0x07 | 50 ms，`ECSP_RESP_RESET_MS`。放宽它不影响上一行 |
| 0x03、0x05 | 3000 ms，`ECSP_RESP_IDENTITY_MS` |
| 0x0C 的第一次应答 | 500 ms，`ECSP_RESP_UPGRADE_MS` |
| 板卡命令 | 表上的 `resp_ms`；0 或主机没有这张表则 50 ms |

等待 0x03 / 0x05 时，3 秒掉线判据不会截断那 3 次重发。

### 长耗时命令在主机上

提交 0x08 或 0x0C 且启动被 `0x00` 加 `T_exec_max` 受理后，事务进入 `ECSP_TXN_WAIT_RESULT`，**这时还不调用事务回调**。回调在结果已知时调用一次：

- 结果事件到达：`status` 为 `ECSP_OK`，应答码 0x00，`payload[0]` 是结果码。
- 到了 `T_exec_max` 仍没有事件：库自动发送配对的获取结果（0x09 或 0x0D），不提前轮询。上传模式是从机上的开关，主机不保存它，所以即使事件不会来，也是等到这个时限再问。
- 获取结果应答 `0x00` 且结果字节是 0x11：从机还在执行。这次事务到此结束，回调里的 `payload[0]` 就是 0x11。调用方用**新的事务序号**再提交停止，或过一会儿再取一次结果。
- 获取结果应答 0x13：没有可报告的结局（从未启动，或被复位 / 重新握手丢掉）。应答码是 0x13，载荷为空。

等待结果期间，再提交普通命令会得到 `ECSP_E_CAP`。此时期待打断的办法是提交 0x07：复位应答 0x00 时，长耗时回调以 `ECSP_E_RESET` 结束，复位自己的回调以 `ECSP_OK` 结束；复位应答 0x20 时，复位回调拿到这个应答码，原来的等待继续。

### 掉线之后

掉线后对该从机的 `submit` 返回 `ECSP_E_STATE`。恢复路径是：从机重新上电或复位后会再发 0x01 上报，主机收到后重新握手、重新核对版本，进入 `ECSP_SESS_NORMAL` 之后才能再发业务命令。已经确认低功耗的从机，沉默不算掉线，直到退出低功耗、复位或重新握手。

## 同一节点两条链路

一块板可以在上行链路做从机、在下行链路做主机。两个上下文、两套 UART、两块缓冲，主循环里按顺序各 `step` 一次。主机事务回调里允许推进**另一个**上下文的 `step`。

## 本地错误码

函数返回 0（`ECSP_OK`）或负数。这些值只出现在函数返回值和主机回调的 `status` 里，不会写进帧。帧里的失败用应答码（`ECSP_RS_*`）。

| 值 | 名称 | 何时 |
| --- | --- | --- |
| 0 | `ECSP_OK` | 成功 |
| -1 | `ECSP_E_ARG` | 参数或配置非法 |
| -2 | `ECSP_E_LEN` | 载荷长度不符（从机管线把它应答成 0x02） |
| -3 | `ECSP_E_VAL` | 取值非法（应答成 0x03），或长耗时完成码用了 0x11 |
| -4 | `ECSP_E_CAP` | 缓冲或在途事务装不下 |
| -5 | `ECSP_E_STATE` | 会话不是正常通信，命令被本地拒绝 |
| -6 | `ECSP_E_TIMEOUT` | 重发次数用尽仍无应答 |
| -7 | `ECSP_E_OFFLINE` | 会话掉线，在途事务取消 |
| -8 | `ECSP_E_LINK` | 发送 FIFO 装不下整帧 |
| -9 | `ECSP_E_REENT` | 在自己的 `ecsp_host_step` 里又调用了它 |
| -10 | `ECSP_E_RESET` | 在途事务被复位取消 |
| -11 | `ECSP_E_LOW_POWER` | 从机已确认低功耗，该业务命令被本地拒绝 |

## 时间

| 项 | 默认 | 宏 |
| --- | --- | --- |
| 单帧上限 | 128 字节 | `ECSP_MAX_FRAME` |
| 从机事务缓存 | 4 条 | `ECSP_SLAVE_TXN_CACHE`（0 会让非幂等动作的重传再执行一次） |
| 主机从机槽位 | 8 | `ECSP_HOST_SLAVES` |
| 心跳周期 | 1000 ms | `ECSP_HEARTBEAT_MS`。改它时主机掉线门限（三倍）一起变 |
| 多数命令的应答时限 | 50 ms | `ECSP_RESP_DEFAULT_MS` |
| 复位应答时限 | 50 ms | `ECSP_RESP_RESET_MS` |
| 身份写入应答时限 | 3000 ms | `ECSP_RESP_IDENTITY_MS` |
| 升级首次应答 | 500 ms | `ECSP_RESP_UPGRADE_MS` |
| 上电到可发业务命令 | 5000 ms | `ECSP_SESSION_SETUP_MS` |
| 自检 `T_exec_max` | 2000 ms | `ECSP_SELFTEST_EXEC_MAX_MS` |
| 升级 `T_exec_max` | 60000 ms | `ECSP_UPGRADE_EXEC_MAX_MS` |

等待握手时的 0x01 上报固定 1 Hz，没有对应的宏。

## 帧（只有自己组帧时才需要）

正常主从路径不用手写帧，`step` 会编解码。抓包或测试时布局如下，多字节字段小端：

```
FA | 帧长 u16 | 产品地址 | 命令 | 板卡地址 | 载荷 | CRC16-MODBUS | 0D
```

CRC 覆盖帧长的第一个字节到载荷的最后一个字节，不含帧头。最小合法帧 9 字节。`ecsp_frame_encode` 组帧，`ecsp_frame_scan` 从字节流里取出一帧；扫到坏帧只丢弃，不应答。

## 参考

- 从机固件整例：`board/f429/fw/main.c`（USART1、扇区身份、0x07 后复位、0x0B 后睡眠）
- 板卡命令表：`board/f429/bringup.c`（0x30–0x33）
- 主机行为的可执行说明：`test/unit/test_host.c`
- 一主一从经虚拟链路跑通：`test/integ/two_instance.c`
- 协议：[`docs/specs/ecsp-spec.md`](docs/specs/ecsp-spec.md)
- 库的分层与 API 契约：[`docs/specs/ecsp-impl-spec.md`](docs/specs/ecsp-impl-spec.md)
