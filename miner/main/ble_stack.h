/**
 * @file ble_stack.h
 * @brief One NimBLE stack, shared by the ring and the machine (SDD-001).
 *
 * Each module used to bring the whole stack up and down for its own job, so
 * the ring and the AirSense 11 could never be connected at once. This owner
 * holds the stack for as long as anyone needs it (refcounted around
 * nimble_port_init / nimble_port_deinit), runs the one host task, sets
 * ble_hs_cfg once, and syncs once. Each module keeps its own connection
 * handle and its own GAP callback: NimBLE routes events per connection, so
 * two connections need nothing more than CONFIG_BT_NIMBLE_MAX_CONNECTIONS=2.
 *
 * What NimBLE does NOT share is the master procedure: there is one scan-or-
 * connect state, and a second ble_gap_disc or ble_gap_connect while one runs
 * is refused (EALREADY / EBUSY). The master token below serializes them:
 * take it before a scan or a connect, give it when the procedure ends (the
 * scan completed with no connect following, the connect came back either
 * way, or the procedure was cancelled). Giving a token one does not hold is
 * harmless, and the holder may take it again, so a scan that turns into a
 * connect keeps it throughout.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BLE_USER_NONE = 0,
    BLE_USER_RING = 1,   /* o2ring_ble_nimble.c */
    BLE_USER_AS11 = 2,   /* as11_ble.c */
} ble_user_t;

/* Bring the stack up for `who` (refcounted). The first caller starts the
 * host and waits up to 5 s for its sync; later callers find it synced. A
 * second acquire by the same user is counted once. Returns false when the
 * host did not come up or sync; nothing is held then. Call from a task,
 * never from the host task. */
bool ble_stack_acquire(ble_user_t who);

/* Let go of the stack. The last user out stops and deinits the host. The
 * caller has already closed its own connection and cancelled its own scan
 * or connect, and given the master token. */
void ble_stack_release(ble_user_t who);

bool    ble_stack_is_up(void);
int     ble_stack_users(void);
uint8_t ble_stack_own_addr_type(void);

/* The master token (see above). `wait_ms` 0 never blocks, which is what a
 * GAP callback on the host task must use. */
bool ble_stack_master_take(ble_user_t who, uint32_t wait_ms);
void ble_stack_master_give(ble_user_t who);
ble_user_t ble_stack_master_holder(void);
