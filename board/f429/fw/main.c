/*
 * STM32F429IGT6 slave firmware (ticket #18 / ADR-0009).
 *
 * Cube HAL for RCC/Flash/PWR/UID; LL for USART RX IRQ + polled TX drain.
 * Sector-11 identity commit, Sleep after 0x0B, board commands 0x30-0x33,
 * and reset only after the 0x07 answer has left the pin.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ecsp/slave.h"
#include "stm32f4xx_hal.h"

#include "bringup.h"
#include "clock.h"
#include "flash_identity.h"
#include "power.h"
#include "uid.h"
#include "usart.h"

#define PRODUCT_ADDR 0x03u
#define BOARD_ADDR   0x02u

static uint8_t g_rx[ECSP_MAX_FRAME * 2u];
static uint8_t g_tx[ECSP_MAX_FRAME * 2u];
static ecsp_slave_t g_slave;
static ecsp_slave_identity_t g_identity;
static uint8_t g_chip_id[F429_CHIP_ID_LEN];

static volatile uint8_t g_tick;

static const uint8_t k_sw_version[ECSP_VERSION_LEN] = {
    '1', '.', '0', '.', '0', '.', '0'
};
static const ecsp_slave_identity_t k_blank_identity = {
    { { '1', '.', '0', '.', '0', '.', '0' } },
    { { 'S', 'N', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0' } }
};

static void on_usart_rx(uint8_t byte)
{
    ecsp_slave_rx(&g_slave, &byte, 1u);
}

void SysTick_Handler(void)
{
    HAL_IncTick();
    g_tick = 1u;
}

static int on_safe_abort(ecsp_slave_t *s, uint8_t cmd)
{
    (void)s;
    (void)cmd;
    /* No actuator to park (#13 / #18). */
    return ECSP_OK;
}

static int on_identity_commit(ecsp_slave_t *s,
                              const ecsp_slave_identity_t *next)
{
    (void)s;
    return flash_identity_commit(next);
}

static void slave_init(void)
{
    ecsp_slave_cfg_t cfg;
    const ecsp_cmd_table_t *cmds;

    flash_identity_load(&g_identity, &k_blank_identity);
    uid_read(g_chip_id);
    f429_board_init(g_chip_id);
    cmds = f429_board_cmds();

    memset(&cfg, 0, sizeof cfg);
    cfg.product_addr = PRODUCT_ADDR;
    cfg.board_addr = BOARD_ADDR;
    memcpy(cfg.version.b, k_sw_version, ECSP_VERSION_LEN);
    cfg.identity = &g_identity;
    cfg.fault_word = 0u;
    cfg.cmds = *cmds;
    cfg.rx_buf = g_rx;
    cfg.rx_cap = sizeof g_rx;
    cfg.tx_buf = g_tx;
    cfg.tx_cap = sizeof g_tx;

    (void)ecsp_slave_init(&g_slave, &cfg);
    ecsp_slave_set_identity_commit(&g_slave, on_identity_commit);
    ecsp_slave_set_safe_abort(&g_slave, on_safe_abort);
}

static void drain_tx(void)
{
    while (ecsp_slave_tx_pending(&g_slave) > 0u && usart_tx_ready() != 0) {
        uint8_t b = g_tx[0];
        usart_write(b);
        ecsp_slave_tx_drain(&g_slave, 1u);
    }
}

static int link_idle(void)
{
    return (ecsp_slave_tx_pending(&g_slave) == 0u &&
            usart_tx_complete() != 0) ? 1 : 0;
}

int main(void)
{
    (void)HAL_Init();
    clock_init();
    /* SysTick was programmed for the pre-PLL clock; rebind at 180 MHz. */
    if (HAL_InitTick(TICK_INT_PRIORITY) != HAL_OK) {
        for (;;) {
        }
    }

    usart_init(on_usart_rx);
    slave_init();

    for (;;) {
        if (g_tick != 0u) {
            g_tick = 0u;
            ecsp_slave_step(&g_slave, HAL_GetTick());
        }
        drain_tx();

        if (ecsp_slave_reset_requested(&g_slave) != 0 && link_idle() != 0) {
            NVIC_SystemReset();
        }

        if (ecsp_slave_low_power_mode(&g_slave) == ECSP_LOW_POWER_ENTER &&
            link_idle() != 0) {
            power_enter_sleep();
        }
    }
}
