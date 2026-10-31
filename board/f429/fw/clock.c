#include "clock.h"

#include "stm32f429_regs.h"

void f429_clock_init(void)
{
    /* Power interface clock before voltage scaling / overdrive. */
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    (void)RCC->APB1ENR;

    /* Voltage scale 1 is required before locking 180 MHz. */
    PWR->CR = (PWR->CR & ~(0x3u << 14)) | PWR_CR_VOS_SCALE1;
    while ((PWR->CSR & PWR_CSR_VOSRDY) == 0u) {
    }

    RCC->CR |= RCC_CR_HSEON;
    while ((RCC->CR & RCC_CR_HSERDY) == 0u) {
    }

    /* PLL: HSE/25*360/2 = 180 MHz. PLLQ keeps USB-ish 48 MHz shape unused. */
    RCC->PLLCFGR = (25u << RCC_PLLCFGR_PLLM_Pos)
                 | (360u << RCC_PLLCFGR_PLLN_Pos)
                 | (0u << RCC_PLLCFGR_PLLP_Pos)   /* /2 */
                 | RCC_PLLCFGR_PLLSRC_HSE
                 | (7u << RCC_PLLCFGR_PLLQ_Pos);

    RCC->CR |= RCC_CR_PLLON;
    while ((RCC->CR & RCC_CR_PLLRDY) == 0u) {
    }

    /* Overdrive is mandatory at 180 MHz on this part. */
    PWR->CR |= PWR_CR_ODEN;
    while ((PWR->CSR & PWR_CSR_ODRDY) == 0u) {
    }
    PWR->CR |= PWR_CR_ODSWEN;
    while ((PWR->CSR & PWR_CSR_ODSWRDY) == 0u) {
    }

    /* 5 wait states at 2.7–3.6 V / 180 MHz; turn on the caches. */
    FLASH->ACR = FLASH_ACR_LATENCY_5WS
               | FLASH_ACR_PRFTEN
               | FLASH_ACR_ICEN
               | FLASH_ACR_DCEN;

    /* AHB = SYSCLK, APB1 = /4 (45 MHz), APB2 = /2 (90 MHz). */
    RCC->CFGR = (RCC->CFGR & ~0x0000FCFCu)
              | RCC_CFGR_HPRE_DIV1
              | RCC_CFGR_PPRE1_DIV4
              | RCC_CFGR_PPRE2_DIV2;

    RCC->CFGR = (RCC->CFGR & ~0x3u) | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS_Msk) != RCC_CFGR_SWS_PLL) {
    }
}
