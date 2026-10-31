#ifndef USART_H
#define USART_H

#include <stdint.h>

/*
 * USART1 on PA9 (TX) / PA10 (RX), AF7, 115200 8N1, no flow control.
 * RXNE interrupt delivers one byte at a time through the callback.
 * TX is polled: TXE to accept a byte, TC when the shift register is empty.
 */

typedef void (*usart_rx_fn)(uint8_t byte);

void usart_init(usart_rx_fn on_rx);
int  usart_tx_ready(void);
int  usart_tx_complete(void);
void usart_write(uint8_t byte);

void USART1_IRQHandler(void);

#endif /* USART_H */
