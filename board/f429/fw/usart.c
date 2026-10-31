#include "usart.h"

#include "stm32f4xx_hal.h"
#include "stm32f4xx_ll_bus.h"
#include "stm32f4xx_ll_gpio.h"
#include "stm32f4xx_ll_usart.h"

#define USART1_BAUD 115200u

static usart_rx_fn g_on_rx;

void usart_init(usart_rx_fn on_rx)
{
    LL_GPIO_InitTypeDef gpio;
    LL_USART_InitTypeDef uart;

    g_on_rx = on_rx;

    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPIOA);
    LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_USART1);

    gpio.Pin = LL_GPIO_PIN_9 | LL_GPIO_PIN_10;
    gpio.Mode = LL_GPIO_MODE_ALTERNATE;
    gpio.Speed = LL_GPIO_SPEED_FREQ_HIGH;
    gpio.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
    gpio.Pull = LL_GPIO_PULL_NO;
    gpio.Alternate = LL_GPIO_AF_7;
    (void)LL_GPIO_Init(GPIOA, &gpio);

    LL_USART_Disable(USART1);
    uart.BaudRate = USART1_BAUD;
    uart.DataWidth = LL_USART_DATAWIDTH_8B;
    uart.StopBits = LL_USART_STOPBITS_1;
    uart.Parity = LL_USART_PARITY_NONE;
    uart.TransferDirection = LL_USART_DIRECTION_TX_RX;
    uart.HardwareFlowControl = LL_USART_HWCONTROL_NONE;
    uart.OverSampling = LL_USART_OVERSAMPLING_16;
    (void)LL_USART_Init(USART1, &uart);

    LL_USART_EnableIT_RXNE(USART1);
    LL_USART_Enable(USART1);

    NVIC_SetPriority(USART1_IRQn, 5u);
    NVIC_EnableIRQ(USART1_IRQn);
}

int usart_tx_ready(void)
{
    return LL_USART_IsActiveFlag_TXE(USART1) != 0u ? 1 : 0;
}

int usart_tx_complete(void)
{
    return LL_USART_IsActiveFlag_TC(USART1) != 0u ? 1 : 0;
}

void usart_write(uint8_t byte)
{
    LL_USART_TransmitData8(USART1, byte);
}

void USART1_IRQHandler(void)
{
    if (LL_USART_IsActiveFlag_RXNE(USART1) != 0u) {
        uint8_t b = LL_USART_ReceiveData8(USART1);
        if (g_on_rx != 0) {
            g_on_rx(b);
        }
    }
}
