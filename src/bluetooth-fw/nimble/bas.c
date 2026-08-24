/* SPDX-FileCopyrightText: 2025 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "bluetooth/bas.h"
#include "host/ble_gatt.h"
#include "services/bas/ble_svc_bas.h"

void bt_driver_bas_handle_update(uint8_t percent) {
  ble_svc_bas_battery_level_set(percent);
}

void bt_driver_bas_handle_refresh(void) {
  uint16_t attr_handle;
  const int rc = ble_gatts_find_chr(BLE_UUID16_DECLARE(BLE_SVC_BAS_UUID16),
                                    BLE_UUID16_DECLARE(BLE_SVC_BAS_CHR_UUID16_BATTERY_LEVEL),
                                    NULL, &attr_handle);
  if (rc == 0) {
    ble_gatts_chr_updated(attr_handle);
  }
}
