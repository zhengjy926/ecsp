---
Status: accepted
---

# F429 从机固件使用 STM32Cube HAL/LL，协议库仍禁止硬件依赖

`board/f429/fw/` 从自写寄存器图改为 vendor 进仓的 STM32CubeF4：**RCC / Flash / PWR / UID 走 HAL，USART 收发与 RX 中断走 LL**；SysTick 由 HAL 时基驱动，`ecsp_slave_step` 的 `now_ms` 取 `HAL_GetTick()`。协议库（`src/ecsp/`）与板卡命令表（`bringup`）仍零 HAL、零外设。动机是可维护性，以及把 #13/#18 的扇区 11 身份、Sleep、芯片 UID 接到同一套外设实现上，而不是在协议库里引入可移植 port。

## Considered Options

- **继续自写寄存器**：#17 已能跑通 USART，但扇区擦写与 Sleep 保钟再手维护一份 regs 头，成本高于收益。
- **在 `src/ecsp/` 或公共 `platform/` 抽跨板 HAL**：违反实现规格「不做硬件驱动」，否决。
- **全程 Cube HAL（含 `HAL_UART`）**：句柄/回调模型容易把推进逻辑拉进中断，与 ADR-0005「RX 中断只塞字节」冲突，否决。
- **CubeMX 生成工程替换 `fw/`**：冲掉现有 Makefile、链接脚本（扇区 11 留空）与 `check-f429-fw.ps1` 纪律；时钟与 Sleep 约束已由 #13 写死，否决。
- **git submodule / 构建时下载 Cube 包**：AFK 与可选交叉编译更脆，否决；改为 `board/f429/fw/Drivers/` 按需 vendor。

## Consequences

- 删除 `stm32f429_regs.h`；外设只经 CMSIS device + HAL/LL。
- `fw/` 按子系统拆分：`clock.c`、`usart.c`、`flash_identity.c`、`power.c`、`uid.c`、`main.c`，无统一 `f429_` 前缀。
- 身份提交仍在 step 栈上阻塞擦写（#13）；低功耗仍为 Sleep 而非 Stop；USART 仍为 IRQ RX + 主循环轮询 TXE drain。
- #13 原文「本板固件裸寄存器 / 本 port 不写 ADR」由本 ADR 与修订后的 #13/#18 取代；「协议库内无 HAL」仍然成立。
