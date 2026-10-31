#include "usart1.h"

#include "stm32f429_regs.h"

static f429_usart1_rx_fn g_on_rx;

void f429_usart1_init(f429_usart1_rx_fn on_rx)
{
    uint32_t moder;
    uint32_t ospeed;
    uint32_t afrh;

    g_on_rx = on_rx;

    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
    (void)RCC->AHB1ENR;
    (void)RCC->APB2ENR;

    /* PA9 / PA10 alternate function AF7, high speed. */
    moder = GPIOA->MODER;
    moder &= ~((3u << (9u * 2u)) | (3u << (10u * 2u)));
    moder |= (GPIO_MODER_AF << (9u * 2u)) | (GPIO_MODER_AF << (10u * 2u));
    GPIOA->MODER = moder;

    ospeed = GPIOA->OSPEEDR;
    ospeed &= ~((3u << (9u * 2u)) | (3u << (10u * 2u)));
    ospeed |= (GPIO_OSPEEDR_HIGH << (9u * 2u))
            | (GPIO_OSPEEDR_HIGH << (10u * 2u));
    GPIOA->OSPEEDR = ospeed;

    afrh = GPIOA->AFR[1];
    afrh &= ~((0xFu << ((9u - 8u) * 4u)) | (0xFu << ((10u - 8u) * 4u)));
    afrh |= (GPIO_AF7 << ((9u - 8u) * 4u)) | (GPIO_AF7 << ((10u - 8u) * 4u));
    GPIOA->AFR[1] = afrh;

    /* OVER8=0: BRR = round(pclk / baud). APB2 is 90 MHz after clock_init. */
    USART1->CR1 = 0u;
    USART1->CR2 = 0u;
    USART1->CR3 = 0u;
    USART1->BRR = (APB2_HZ + (USART1_BAUD / 2u)) / USART1_BAUD;
    USART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE | USART_CR1_UE;

    /* Enable USART1 IRQ in NVIC (IRQ 37 -> ISER[1] bit 5). */
    NVIC_ISER[USART1_IRQn / 32u] = 1u << (USART1_IRQn % 32u);
}

int f429_usart1_tx_ready(void)
{
    return (USART1->SR & USART_SR_TXE) != 0u ? 1 : 0;
}

void f429_usart1_write(uint8_t byte)
{
    USART1->DR = byte;
}

void USART1_IRQHandler(void)
{
    if ((USART1->SR & USART_SR_RXNE) != 0u) {
        uint8_t b = (uint8_t)USART1->DR;
        if (g_on_rx != 0) {
            g_on_rx(b);
        }
    }
}
