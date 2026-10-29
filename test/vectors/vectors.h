#ifndef ECSP_TEST_VECTORS_H
#define ECSP_TEST_VECTORS_H

/*
 * Golden frames (ADR-0001: spec diff + vector + implementation).
 *
 * X-macro table; each row is
 *
 *     ECSP_VECTOR(name, "hex bytes", "spec <section>")
 *
 * The hex bytes are the COMPLETE frame: header 0xFA ... tail 0x0D. The two
 * CRC bytes are real (CRC16-MODBUS over [length..payload], spec 3.2) - never a
 * placeholder. `tools/check-vectors.ps1` recomputes and compares every CRC and
 * asserts that each referenced section is covered, so deleting a row is
 * reported as a missing spec section.
 *
 * Addresses use the spec 1.5 convention P=0x03, B=0x02.
 */

/* 3.2 CRC rules: the minimal frame; CRC16-MODBUS over 09 00 03 01 02 = 0x5088. */
ECSP_VECTOR(crc_modbus_minimal, "FA 09 00 03 01 02 88 50 0D", "spec 3.2")

/* 3.4.1 basic request / response frames (P/B/RS concretized). */
ECSP_VECTOR(req_basic,  "FA 09 00 03 01 02 88 50 0D", "spec 3.4.1")
ECSP_VECTOR(resp_basic, "FA 0A 00 03 01 02 00 50 55 0D", "spec 3.4.1")

/* 5.2.1 handshake: report frame, request frame, response frame. The report and
 * request frames are byte-identical; direction is the only difference. */
ECSP_VECTOR(handshake_report, "FA 09 00 03 01 02 88 50 0D", "spec 5.2.1")
ECSP_VECTOR(handshake_req,    "FA 09 00 03 01 02 88 50 0D", "spec 5.2.1")
ECSP_VECTOR(handshake_resp,   "FA 0A 00 03 01 02 00 50 55 0D", "spec 5.2.1")

/* 4.1.2 power-up session: the waiting slave repeats the 5.2.1 report frame at a
 * fixed 1 Hz until the handshake succeeds, so the same bytes are the golden
 * frame for this clause too. */
ECSP_VECTOR(session_report, "FA 09 00 03 01 02 88 50 0D", "spec 4.1.2")

/* 5.2.2 software version request / response ("1.0.0.0"). The response CRC low
 * byte is 0xFA - the scanner must not break this frame on it. */
ECSP_VECTOR(version_req,  "FA 09 00 03 02 02 88 A0 0D", "spec 5.2.2")
ECSP_VECTOR(version_resp, "FA 11 00 03 02 02 00 31 2E 30 2E 30 2E 30 FA 0E 0D", "spec 5.2.2")

/* 5.2.3 set hardware version ("1.0.0.0") request / response. */
ECSP_VECTOR(hw_set_req,  "FA 10 00 03 03 02 31 2E 30 2E 30 2E 30 B2 C1 0D", "spec 5.2.3")
ECSP_VECTOR(hw_set_resp, "FA 0A 00 03 03 02 00 F1 95 0D", "spec 5.2.3")

/* 5.2.4 get hardware version request / response ("1.0.0.0"). */
ECSP_VECTOR(hw_get_req,  "FA 09 00 03 04 02 8B 00 0D", "spec 5.2.4")
ECSP_VECTOR(hw_get_resp, "FA 11 00 03 04 02 00 31 2E 30 2E 30 2E 30 1A 11 0D", "spec 5.2.4")

/* 5.2.5 set serial number ("SN0000000000") request / response. */
ECSP_VECTOR(sn_set_req,  "FA 15 00 03 05 02 53 4E 30 30 30 30 30 30 30 30 30 30 B3 49 0D", "spec 5.2.5")
ECSP_VECTOR(sn_set_resp, "FA 0A 00 03 05 02 00 11 94 0D", "spec 5.2.5")

/* 5.2.6 get serial number request / response. */
ECSP_VECTOR(sn_get_req,  "FA 09 00 03 06 02 8A 60 0D", "spec 5.2.6")
ECSP_VECTOR(sn_get_resp, "FA 16 00 03 06 02 00 53 4E 30 30 30 30 30 30 30 30 30 30 CC 72 0D", "spec 5.2.6")

/* 5.2.7 system reset request (transaction seq 0x01) / response. */
ECSP_VECTOR(reset_req,  "FA 0A 00 03 07 02 01 71 94 0D", "spec 5.2.7")
ECSP_VECTOR(reset_resp, "FA 0A 00 03 07 02 00 B0 54 0D", "spec 5.2.7")

/* 5.2.10 self-test result report (response code 0x00 = success). */
ECSP_VECTOR(selftest_report, "FA 0A 00 03 0A 02 00 21 97 0D", "spec 5.2.10")

/* 5.2.11 set low-power mode (mode 0x01 = enter) request / response. */
ECSP_VECTOR(low_power_req,  "FA 0A 00 03 0B 02 01 B1 97 0D", "spec 5.2.11")
ECSP_VECTOR(low_power_resp, "FA 0A 00 03 0B 02 00 70 57 0D", "spec 5.2.11")

/* 5.2.14 upgrade result report (response code 0x00 = success). */
ECSP_VECTOR(upgrade_report, "FA 0A 00 03 0E 02 00 60 56 0D", "spec 5.2.14")

/* 5.2.16 heartbeat. */
ECSP_VECTOR(heartbeat, "FA 09 00 03 10 02 84 00 0D", "spec 5.2.16")

/* 7.2.2 slave liveness: after a heartbeat period without sending anything the
 * slave repeats the 5.2.16 heartbeat frame; the same bytes are the golden frame
 * for this clause. */
ECSP_VECTOR(session_heartbeat, "FA 09 00 03 10 02 84 00 0D", "spec 7.2.2")

/* 5.2.17 fault report (fault word 0x01 = bit0 self-test failure). */
ECSP_VECTOR(fault_report, "FA 0A 00 03 11 02 01 90 50 0D", "spec 5.2.17")

/* 5.2.18 fault query request / response (code 0x00 + fault word 0x01). */
ECSP_VECTOR(fault_query_req,  "FA 09 00 03 12 02 85 60 0D", "spec 5.2.18")
ECSP_VECTOR(fault_query_resp, "FA 0B 00 03 12 02 00 01 80 B8 0D", "spec 5.2.18")

#endif /* ECSP_TEST_VECTORS_H */
