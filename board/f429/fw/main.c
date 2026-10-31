/*
 * STM32F429IGT6 slave firmware (ticket #17).
 *
 * Clock and USART1 bring-up, then the existing slave engine on a 1 ms tick.
 * RX IRQ only calls ecsp_slave_rx; the main loop steps and drains TX into
 * USART1. No identity commit, no sector-11 programming, no Sleep in this
 * ticket. Board commands 0x30-0x33 arrive in #18.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ecsp/slave.h"

#include "clock.h"
#include "stm32f429_regs.h"
#include "usart1.h"

#define PRODUCT_ADDR 0x03u
#define BOARD_ADDR   0x02u

static uint8_t g_rx[ECSP_MAX_FRAME * 2u];
static uint8_t g_tx[ECSP_MAX_FRAME * 2u];
static ecsp_slave_t g_slave;
static ecsp_slave_identity_t g_identity;

static volatile uint32_t g_now_ms;
static volatile uint8_t g_tick;

static void on_usart_rx(uint8_t byte)
{
    ecsp_slave_rx(&g_slave, &byte, 1u);
}

void SysTick_Handler(void)
{
    g_now_ms++;
    g_tick = 1u;
}

static void systick_init(void)
{
    SysTick->LOAD = (SYSCLK_HZ / 1000u) - 1u;
    SysTick->VAL = 0u;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE
                  | SysTick_CTRL_TICKINT
                  | SysTick_CTRL_ENABLE;
}

/* Compile-time bring-up identity (ticket #17). Software version is baked into
 * the image separately from the RAM hardware-version / serial defaults. */
static const uint8_t k_sw_version[ECSP_VERSION_LEN] = {
    '1', '.', '0', '.', '0', '.', '0'
};
static const uint8_t k_hw_version[ECSP_VERSION_LEN] = {
    '1', '.', '0', '.', '0', '.', '0'
};
static const uint8_t k_serial[ECSP_SERIAL_LEN] = {
    'S', 'N', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0'
};

static void slave_init(void)
{
    ecsp_slave_cfg_t cfg;

    memcpy(g_identity.hardware_version.b, k_hw_version, ECSP_VERSION_LEN);
    memcpy(g_identity.serial_number.b, k_serial, ECSP_SERIAL_LEN);

    memset(&cfg, 0, sizeof cfg);
    cfg.product_addr = PRODUCT_ADDR;
    cfg.board_addr = BOARD_ADDR;
    memcpy(cfg.version.b, k_sw_version, ECSP_VERSION_LEN);
    cfg.identity = &g_identity;
    cfg.fault_word = 0u;
    cfg.cmds.table = NULL;
    cfg.cmds.table_len = 0u;
    cfg.rx_buf = g_rx;
    cfg.rx_cap = sizeof g_rx;
    cfg.tx_buf = g_tx;
    cfg.tx_cap = sizeof g_tx;

    (void)ecsp_slave_init(&g_slave, &cfg);
    /* No identity_commit and no safe_abort in this ticket. */
}

static void drain_tx(void)
{
    while (ecsp_slave_tx_pending(&g_slave) > 0u && f429_usart1_tx_ready() != 0) {
        uint8_t b = g_tx[0];
        f429_usart1_write(b);
        ecsp_slave_tx_drain(&g_slave, 1u);
    }
}

int main(void)
{
    f429_clock_init();
    f429_usart1_init(on_usart_rx);
    slave_init();
    systick_init();

    for (;;) {
        if (g_tick != 0u) {
            g_tick = 0u;
            /* Parent #13: RX IRQ only appends; the 1 ms tick steps. */
            ecsp_slave_step(&g_slave, g_now_ms);
        }
        drain_tx();
    }
}
