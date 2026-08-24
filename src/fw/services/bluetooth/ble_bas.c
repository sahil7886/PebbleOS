/* SPDX-FileCopyrightText: 2025 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "applib/event_service_client.h"
#include "bluetooth/bas.h"
#include "kernel/event_loop.h"
#include "kernel/pebble_tasks.h"
#include "pbl/services/regular_timer.h"
#include "syscall/syscall.h"

#define BLE_BAS_REFRESH_INTERVAL_MINUTES 5

static EventServiceInfo s_bas_evt;
static RegularTimerInfo s_bas_refresh_timer;

static void prv_execute_on_kernel_main(CallbackEventCallback cb) {
  if (pebble_task_get_current() != PebbleTask_KernelMain) {
    launcher_task_add_callback(cb, NULL);
  } else {
    cb(NULL);
  }
}

static void prv_ble_bas_handle_event(PebbleEvent *e, void *context) {
  const PebbleBatteryStateChangeEvent *const battery_state_event = &e->battery_state;

  bt_driver_bas_handle_update(battery_state_event->new_state.pct);
}

static void prv_refresh_ble_bas_kernel_main(void *unused) {
  bt_driver_bas_handle_refresh();
}

// The battery service normally only notifies when the rounded percentage changes. A periodic
// refresh keeps a subscribed phone supplied with a useful local-history sample while a level is
// flat (which is common overnight).
static void prv_ble_bas_refresh_timer_cb(void *unused) {
  prv_execute_on_kernel_main(prv_refresh_ble_bas_kernel_main);
}

static void prv_start_ble_bas_kernel_main(void *unused) {
  BatteryChargeState battery_state;

  battery_state = sys_battery_get_charge_state();
  bt_driver_bas_handle_update(battery_state.charge_percent);

  s_bas_evt = (EventServiceInfo) {
    .type = PEBBLE_BATTERY_STATE_CHANGE_EVENT,
    .handler = prv_ble_bas_handle_event,
  };

  event_service_client_subscribe(&s_bas_evt);

  s_bas_refresh_timer = (RegularTimerInfo) {
    .cb = prv_ble_bas_refresh_timer_cb,
  };
  regular_timer_add_multiminute_callback(&s_bas_refresh_timer,
                                         BLE_BAS_REFRESH_INTERVAL_MINUTES);
}

static void prv_stop_ble_bas_kernel_main(void *unused) {
  if (regular_timer_is_scheduled(&s_bas_refresh_timer)) {
    regular_timer_remove_callback(&s_bas_refresh_timer);
  }
  event_service_client_unsubscribe(&s_bas_evt);
}

void ble_bas_init(void) {
  prv_execute_on_kernel_main(prv_start_ble_bas_kernel_main);
}

void ble_bas_deinit(void) {
  prv_execute_on_kernel_main(prv_stop_ble_bas_kernel_main);
}
