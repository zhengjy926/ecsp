#include "long_run.h"

#if ECSP_ENABLE_SLAVE

#include "ecsp/bytes.h"
#include "ecsp/cmds.h"
#include "ecsp/config.h"

static uint16_t exec_max_of(uint8_t cmd)
{
    if (cmd == ECSP_CMD_START_UPGRADE) {
        return (uint16_t)ECSP_UPGRADE_EXEC_MAX_MS;
    }
    return (uint16_t)ECSP_SELFTEST_EXEC_MAX_MS;
}

static uint8_t event_cmd_of(uint8_t cmd)
{
    uint8_t event_cmd = 0u;
    uint8_t query_cmd = 0u;

    if (ecsp_catalog_long_run(cmd, &event_cmd, &query_cmd) != 0) {
        (void)query_cmd;
        return event_cmd;
    }
    return 0u;
}

static void store_result(ecsp_slave_t *s, uint8_t cmd,
                         uint8_t known, uint8_t result)
{
    if (cmd == ECSP_CMD_START_UPGRADE) {
        s->up_known = known;
        s->up_result = result;
    } else if (cmd == ECSP_CMD_CONTROL_SELFTEST) {
        s->st_known = known;
        s->st_result = result;
    } else {
        /* Not a long-running command this layer tracks. */
    }
}

/* Low 8 bits inverted. A widened `~` truncated back to uint8_t trips MSVC C4310. */
static uint8_t bit_not(uint8_t bits)
{
    return (uint8_t)(0xFFu ^ bits);
}

/* Latch bit0 when it rises, and remember the whole word for one 0x11 report
 * (spec 5.2.17: a bit that is already 1 is not reported again). */
static void raise_fault_bit0(ecsp_slave_t *s)
{
    uint8_t bits = ECSP_FAULT_SELFTEST;
    uint8_t raised = (uint8_t)(bits & bit_not(s->fault_word));

    s->fault_word = (uint8_t)(s->fault_word | bits);
    if (raised != 0u) {
        s->fault_report_word = s->fault_word;
        s->fault_report_pending = 1u;
    }
}

static void clear_selftest_fault(ecsp_slave_t *s)
{
    s->fault_word = (uint8_t)(s->fault_word & bit_not(ECSP_FAULT_SELFTEST));
}

static void apply_selftest_fault(ecsp_slave_t *s, uint8_t cmd, uint8_t result)
{
    if (cmd != ECSP_CMD_CONTROL_SELFTEST) {
        return;
    }
    if (result == ECSP_RS_OK) {
        clear_selftest_fault(s);
    } else if (result != ECSP_RS_ABORTED) {
        raise_fault_bit0(s);
    } else {
        /* A host stop is not a failed self-test judgement (table 5-3). */
    }
}

static void finish_running(ecsp_slave_t *s, uint8_t result)
{
    uint8_t cmd = s->lr_cmd;

    apply_selftest_fault(s, cmd, result);
    store_result(s, cmd, 1u, result);
    s->lr_event_cmd = event_cmd_of(cmd);
    s->lr_event_result = result;
    s->lr_event_pending = 1u;
    s->lr_cmd = 0u;
}

static void drop_running(ecsp_slave_t *s)
{
    uint8_t cmd = s->lr_cmd;

    if (cmd == 0u) {
        return;
    }
    store_result(s, cmd, 0u, 0u);
    if ((s->lr_event_pending != 0u) &&
        (s->lr_event_cmd == event_cmd_of(cmd))) {
        s->lr_event_pending = 0u;
    }
    s->lr_cmd = 0u;
}

/* 1 when the product's sequence left the actuator safe. */
static int abort_is_safe(ecsp_slave_t *s, uint8_t cmd)
{
    if (s->safe_abort == NULL) {
        return 0;
    }
    return (s->safe_abort(s, cmd) == ECSP_OK) ? 1 : 0;
}

static int write_exec_max(uint8_t *resp, size_t cap, size_t *resp_len,
                          uint8_t cmd)
{
    if (cap < 2u) {
        return (int)ECSP_RS_FAILED;
    }
    ecsp_wr_u16le(resp, exec_max_of(cmd));
    *resp_len = 2u;
    return (int)ECSP_RS_OK;
}

void ecsp_long_run_init(ecsp_slave_t *s)
{
    s->safe_abort = NULL;
    s->lr_cmd = 0u;
    s->st_known = 0u;
    s->st_result = 0u;
    s->up_known = 0u;
    s->up_result = 0u;
    s->lr_event_pending = 0u;
    s->lr_event_cmd = 0u;
    s->lr_event_result = 0u;
    s->fault_report_pending = 0u;
    s->fault_report_word = 0u;
}

int ecsp_long_run_is_busy(const ecsp_slave_t *s)
{
    return (s->lr_cmd != 0u) ? 1 : 0;
}

int ecsp_long_run_start(ecsp_slave_t *s, uint8_t cmd,
                        uint8_t *resp, size_t cap, size_t *resp_len)
{
    int rs;

    *resp_len = 0u;
    if (s->lr_cmd != 0u) {
        return (int)ECSP_RS_BUSY;
    }
    rs = write_exec_max(resp, cap, resp_len, cmd);
    if (rs != (int)ECSP_RS_OK) {
        return rs;
    }
    s->lr_cmd = cmd;
    return (int)ECSP_RS_OK;
}

int ecsp_long_run_stop(ecsp_slave_t *s, uint8_t cmd,
                       uint8_t *resp, size_t cap, size_t *resp_len)
{
    int rs;

    *resp_len = 0u;
    if (s->lr_cmd != cmd) {
        return (int)ECSP_RS_INVALID;
    }
    /* Halt the actuator before the 0x21 event is queued (spec 5.2.8).
     * The accept result stays 0x21: 0x20 is the failed reset preempt. */
    if (s->safe_abort != NULL) {
        (void)s->safe_abort(s, cmd);
    }
    rs = write_exec_max(resp, cap, resp_len, cmd);
    if (rs != (int)ECSP_RS_OK) {
        return rs;
    }
    finish_running(s, ECSP_RS_ABORTED);
    return (int)ECSP_RS_OK;
}

int ecsp_long_run_query(const ecsp_slave_t *s, uint8_t query_cmd,
                        uint8_t *resp, size_t cap, size_t *resp_len)
{
    uint8_t cmd;
    uint8_t known;
    uint8_t result;

    *resp_len = 0u;
    if (query_cmd == ECSP_CMD_GET_UPGRADE) {
        cmd = ECSP_CMD_START_UPGRADE;
        known = s->up_known;
        result = s->up_result;
    } else if (query_cmd == ECSP_CMD_GET_SELFTEST) {
        cmd = ECSP_CMD_CONTROL_SELFTEST;
        known = s->st_known;
        result = s->st_result;
    } else {
        return (int)ECSP_RS_UNSUPPORTED;
    }

    if (s->lr_cmd == cmd) {
        if (cap < 1u) {
            return (int)ECSP_RS_FAILED;
        }
        resp[0] = ECSP_RS_BUSY;
        *resp_len = 1u;
        return (int)ECSP_RS_OK;
    }
    if (known == 0u) {
        return (int)ECSP_RS_INVALID;
    }
    if (cap < 1u) {
        return (int)ECSP_RS_FAILED;
    }
    resp[0] = result;
    *resp_len = 1u;
    return (int)ECSP_RS_OK;
}

void ecsp_long_run_on_handshake(ecsp_slave_t *s)
{
    uint8_t cmd;

    if (s->lr_cmd == 0u) {
        return;
    }
    cmd = s->lr_cmd;
    /* Spec 4.1.4: stop the business and still answer the handshake 0x00.
     * A failed sequence does not latch a fault here; that report belongs
     * to a failed 0x07 preempt (spec 4.4). */
    if (s->safe_abort != NULL) {
        (void)s->safe_abort(s, cmd);
    }
    drop_running(s);
}

int ecsp_long_run_preempt_for_reset(ecsp_slave_t *s)
{
    uint8_t cmd;

    if (s->lr_cmd == 0u) {
        return (int)ECSP_RS_OK;
    }
    cmd = s->lr_cmd;
    if (abort_is_safe(s, cmd) == 0) {
        raise_fault_bit0(s);
        return (int)ECSP_RS_FAILED;
    }
    drop_running(s);
    return (int)ECSP_RS_OK;
}

int ecsp_slave_long_run_complete(ecsp_slave_t *s, uint8_t result)
{
    if ((s == NULL) || (s->lr_cmd == 0u)) {
        return ECSP_E_ARG;
    }
    if (result == ECSP_RS_BUSY) {
        return ECSP_E_VAL;
    }
    finish_running(s, result);
    return ECSP_OK;
}

uint8_t ecsp_slave_long_run_cmd(const ecsp_slave_t *s)
{
    if (s == NULL) {
        return 0u;
    }
    return s->lr_cmd;
}

void ecsp_slave_set_safe_abort(ecsp_slave_t *s, ecsp_slave_safe_abort_t fn)
{
    if (s == NULL) {
        return;
    }
    s->safe_abort = fn;
}

void ecsp_long_run_flush(ecsp_slave_t *s, uint32_t now_ms,
                         ecsp_long_run_emit_t emit)
{
    if (emit == NULL) {
        return;
    }
    if (s->fault_report_pending != 0u) {
        uint8_t word = s->fault_report_word;

        if (emit(s, ECSP_CMD_FAULT_REPORT, &word, 1u, now_ms) == 0) {
            return;
        }
        s->fault_report_pending = 0u;
    }
    if (s->lr_event_pending != 0u) {
        uint8_t result = s->lr_event_result;

        if (emit(s, s->lr_event_cmd, &result, 1u, now_ms) == 0) {
            return;
        }
        s->lr_event_pending = 0u;
    }
}

#endif /* ECSP_ENABLE_SLAVE */
