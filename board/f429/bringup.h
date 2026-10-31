#ifndef F429_BRINGUP_H
#define F429_BRINGUP_H

/*
 * STM32F429 bring-up board commands (ticket #15).
 *
 * This table belongs to the board, not to the protocol library. The library
 * answers a code in 0x30-0xEF with 0x04 unless this table lists it.
 * f429_board_init copies the 12-byte chip id and clears the test byte and
 * the counter. Call it at power-up and on every reset, before the slave is
 * stepped again. Tests pass a fixed stand-in; the firmware (ticket #18)
 * passes the die's unique id. This module does not read a peripheral.
 * Ticket #17's image leaves this table unregistered.
 */

#include "ecsp/slave.h"

#define F429_CHIP_ID_LEN 12u

#define F429_CMD_GET_CHIP_ID   0x30u
#define F429_CMD_GET_TEST_BYTE 0x31u
#define F429_CMD_SET_TEST_BYTE 0x32u
#define F429_CMD_BUMP_COUNTER  0x33u

void f429_board_init(const uint8_t chip_id[F429_CHIP_ID_LEN]);

const ecsp_cmd_table_t *f429_board_cmds(void);

#endif /* F429_BRINGUP_H */
