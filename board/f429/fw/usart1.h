#ifndef F429_FW_USART1_H
#define F429_FW_USART1_H

#include <stdint.h>

/*
 * USART1 on PA9 (TX) / PA10 (RX), AF7, 115200 8N1, no flow control.
 * RXNE interrupt delivers one byte at a time through the callback.
 */

typedef void (*f429_usart1_rx_fn)(uint8_t byte);

void f429_usart1_init(f429_usart1_rx_fn on_rx);
int  f429_usart1_tx_ready(void);
void f429_usart1_write(uint8_t byte);

void USART1_IRQHandler(void);

#endif /* F429_FW_USART1_H */
