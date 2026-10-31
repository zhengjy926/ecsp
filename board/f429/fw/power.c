#include "power.h"

#include "stm32f4xx_hal.h"

void power_enter_sleep(void)
{
    /* Sleep keeps the APB clock, so USART1 can still wake on RXNE.
     * Stop would gate that clock and corrupt the next frame (#13). */
    HAL_PWR_EnterSLEEPMode(PWR_MAINREGULATOR_ON, PWR_SLEEPENTRY_WFI);
}
