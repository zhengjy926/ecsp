#ifndef STM32F429_REGS_H
#define STM32F429_REGS_H

/*
 * Minimal STM32F429 register map for this board's clock, GPIOA, USART1 and
 * SysTick. No CMSIS, no HAL: the protocol library stays free of these
 * symbols (ticket #17).
 */

#include <stdint.h>

#define PERIPH_BASE      0x40000000u
#define APB1PERIPH_BASE  PERIPH_BASE
#define APB2PERIPH_BASE  (PERIPH_BASE + 0x00010000u)
#define AHB1PERIPH_BASE  (PERIPH_BASE + 0x00020000u)
#define PPB_BASE         0xE0000000u

#define PWR_BASE         (APB1PERIPH_BASE + 0x7000u)
#define USART1_BASE      (APB2PERIPH_BASE + 0x1000u)
#define GPIOA_BASE       (AHB1PERIPH_BASE + 0x0000u)
#define RCC_BASE         (AHB1PERIPH_BASE + 0x3800u)
#define FLASH_R_BASE     (AHB1PERIPH_BASE + 0x3C00u)
#define SYSTICK_BASE     (PPB_BASE + 0x0000E010u)
#define NVIC_ISER_BASE   (PPB_BASE + 0x0000E100u)

typedef struct {
    volatile uint32_t CR;
    volatile uint32_t PLLCFGR;
    volatile uint32_t CFGR;
    volatile uint32_t CIR;
    volatile uint32_t AHB1RSTR;
    volatile uint32_t AHB2RSTR;
    volatile uint32_t AHB3RSTR;
    uint32_t RESERVED0;
    volatile uint32_t APB1RSTR;
    volatile uint32_t APB2RSTR;
    uint32_t RESERVED1[2];
    volatile uint32_t AHB1ENR;
    volatile uint32_t AHB2ENR;
    volatile uint32_t AHB3ENR;
    uint32_t RESERVED2;
    volatile uint32_t APB1ENR;
    volatile uint32_t APB2ENR;
} RCC_TypeDef;

typedef struct {
    volatile uint32_t ACR;
    volatile uint32_t KEYR;
    volatile uint32_t OPTKEYR;
    volatile uint32_t SR;
    volatile uint32_t CR;
    volatile uint32_t OPTCR;
    volatile uint32_t OPTCR1;
} FLASH_TypeDef;

typedef struct {
    volatile uint32_t CR;
    volatile uint32_t CSR;
} PWR_TypeDef;

typedef struct {
    volatile uint32_t MODER;
    volatile uint32_t OTYPER;
    volatile uint32_t OSPEEDR;
    volatile uint32_t PUPDR;
    volatile uint32_t IDR;
    volatile uint32_t ODR;
    volatile uint32_t BSRR;
    volatile uint32_t LCKR;
    volatile uint32_t AFR[2];
} GPIO_TypeDef;

typedef struct {
    volatile uint32_t SR;
    volatile uint32_t DR;
    volatile uint32_t BRR;
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t CR3;
    volatile uint32_t GTPR;
} USART_TypeDef;

typedef struct {
    volatile uint32_t CTRL;
    volatile uint32_t LOAD;
    volatile uint32_t VAL;
    volatile uint32_t CALIB;
} SysTick_TypeDef;

#define RCC     ((RCC_TypeDef *)RCC_BASE)
#define FLASH   ((FLASH_TypeDef *)FLASH_R_BASE)
#define PWR     ((PWR_TypeDef *)PWR_BASE)
#define GPIOA   ((GPIO_TypeDef *)GPIOA_BASE)
#define USART1  ((USART_TypeDef *)USART1_BASE)
#define SysTick ((SysTick_TypeDef *)SYSTICK_BASE)
#define NVIC_ISER ((volatile uint32_t *)NVIC_ISER_BASE)

/* RCC */
#define RCC_CR_HSEON                 (1u << 16)
#define RCC_CR_HSERDY                (1u << 17)
#define RCC_CR_PLLON                 (1u << 24)
#define RCC_CR_PLLRDY                (1u << 25)

#define RCC_PLLCFGR_PLLM_Pos         0u
#define RCC_PLLCFGR_PLLN_Pos         6u
#define RCC_PLLCFGR_PLLP_Pos         16u
#define RCC_PLLCFGR_PLLSRC_HSE       (1u << 22)
#define RCC_PLLCFGR_PLLQ_Pos         24u

#define RCC_CFGR_SW_PLL              0x2u
#define RCC_CFGR_SWS_Pos             2u
#define RCC_CFGR_SWS_Msk             (0x3u << RCC_CFGR_SWS_Pos)
#define RCC_CFGR_SWS_PLL             (RCC_CFGR_SW_PLL << RCC_CFGR_SWS_Pos)
#define RCC_CFGR_HPRE_DIV1           0u
#define RCC_CFGR_PPRE1_DIV4          (0x5u << 10)
#define RCC_CFGR_PPRE2_DIV2          (0x4u << 13)

#define RCC_AHB1ENR_GPIOAEN          (1u << 0)
#define RCC_APB1ENR_PWREN            (1u << 28)
#define RCC_APB2ENR_USART1EN         (1u << 4)

/* FLASH */
#define FLASH_ACR_LATENCY_5WS        5u
#define FLASH_ACR_PRFTEN             (1u << 8)
#define FLASH_ACR_ICEN               (1u << 9)
#define FLASH_ACR_DCEN               (1u << 10)

/* PWR */
#define PWR_CR_VOS_SCALE1            (0x3u << 14)
#define PWR_CR_ODEN                  (1u << 16)
#define PWR_CR_ODSWEN                (1u << 17)
#define PWR_CSR_VOSRDY               (1u << 14)
#define PWR_CSR_ODRDY                (1u << 16)
#define PWR_CSR_ODSWRDY              (1u << 17)

/* GPIO */
#define GPIO_MODER_AF                2u
#define GPIO_OSPEEDR_HIGH            3u
#define GPIO_AF7                     7u

/* USART */
#define USART_SR_RXNE                (1u << 5)
#define USART_SR_TXE                 (1u << 7)
#define USART_CR1_RE                 (1u << 2)
#define USART_CR1_TE                 (1u << 3)
#define USART_CR1_RXNEIE             (1u << 5)
#define USART_CR1_UE                 (1u << 13)

/* SysTick */
#define SysTick_CTRL_ENABLE          (1u << 0)
#define SysTick_CTRL_TICKINT         (1u << 1)
#define SysTick_CTRL_CLKSOURCE       (1u << 2)

/* NVIC: USART1 is IRQ 37 */
#define USART1_IRQn                  37u

#define HSE_HZ                       25000000u
#define SYSCLK_HZ                    180000000u
#define APB2_HZ                      90000000u
#define USART1_BAUD                  115200u

#endif /* STM32F429_REGS_H */
