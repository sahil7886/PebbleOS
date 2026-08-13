/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "sleep_capture.h"

#include <pbl/drivers/rng.h>
#include "kernel/pbl_malloc.h"
#include "pbl/services/data_logging/data_logging_service.h"
#include "pbl/services/activity/activity_private.h"
#include "pbl/services/system_task.h"
#include "util/time/time.h"
#include "util/units.h"

#include <pbl/logging/logging.h>

#include <stdlib.h>

PBL_LOG_MODULE_DECLARE(service_activity, CONFIG_SERVICE_ACTIVITY_LOG_LEVEL);

// Matches the existing overnight sleep window.
#define SLEEP_CAPTURE_START_MINUTE (21 * MINUTES_PER_HOUR)
#define SLEEP_CAPTURE_END_MINUTE   (12 * MINUTES_PER_HOUR)
#define SLEEP_CAPTURE_EPOCH_SECONDS 30
#define SLEEP_CAPTURE_BPM_PERIOD_SECONDS 30

typedef enum {
  SleepCaptureRecordType_Invalid = 0,
  SleepCaptureRecordType_Ppi = 1,
  SleepCaptureRecordType_Bpm = 2,
  SleepCaptureRecordType_Motion = 3,
  SleepCaptureRecordType_Session = 4,
} SleepCaptureRecordType;

// One resumable DataLogging stream for a complete night.
typedef struct PACKED {
  uint32_t session_id;
  uint16_t sequence;
  uint32_t timestamp_utc;
  uint16_t value;
  int8_t quality;
  uint8_t type_flags;
} SleepCaptureDataLoggingRecord;
_Static_assert(sizeof(SleepCaptureDataLoggingRecord) == 14,
               "Sleep capture DataLogging record must remain wire-compatible");

#define SLEEP_CAPTURE_LOGGING_VERSION 1
#define SLEEP_CAPTURE_TYPE_MASK 0x07
#define SLEEP_CAPTURE_FLAG_COMPLETE (1 << 3)
#define SLEEP_CAPTURE_FLAG_DROPPED (1 << 4)
#define SLEEP_CAPTURE_VERSION_SHIFT 6
#define SLEEP_CAPTURE_MOTION_QUALITY_UNKNOWN (-128)

typedef struct {
  bool active;
  uint32_t session_id;
  uint32_t next_sequence;
  uint32_t dropped_records;
  DataLoggingSession *dls_session;
  bool storage_full;
  bool reported_log_error;
  time_t last_logged_bpm_utc;

  time_t motion_epoch_start_utc;
  uint32_t motion_energy;
  uint16_t motion_sample_count;
  AccelRawData previous_accel;
  bool has_previous_accel;
} SleepCaptureState;

// Keep the capture session alive until the terminal record has been persisted. Buffered
// DataLogging can reject the first terminal append while it drains preceding PPI records.
typedef struct {
  DataLoggingSession *session;
  uint32_t session_id;
  uint16_t sequence;
  time_t end_utc;
  uint16_t dropped_records;
  bool terminal_record_queued;
} SleepCaptureDlsFinishData;

static SleepCaptureState s_sleep_capture;

static uint8_t prv_type_flags(SleepCaptureRecordType type, uint8_t flags) {
  return ((SLEEP_CAPTURE_LOGGING_VERSION << SLEEP_CAPTURE_VERSION_SHIFT) |
          ((uint8_t)type & SLEEP_CAPTURE_TYPE_MASK) | flags);
}

static bool prv_is_capture_window(time_t utc_sec) {
  const int minute_of_day = time_util_get_minute_of_day(utc_sec);
  return (minute_of_day >= SLEEP_CAPTURE_START_MINUTE) ||
         (minute_of_day < SLEEP_CAPTURE_END_MINUTE);
}

static uint16_t prv_clamp_u16(uint32_t value) {
  return (value > UINT16_MAX) ? UINT16_MAX : (uint16_t)value;
}

static uint32_t prv_new_session_id(time_t now_utc) {
  uint32_t session_id;
  if (!rng_rand(&session_id) || session_id == 0) {
    session_id = (uint32_t)now_utc;
  }
  return (session_id == 0) ? 1 : session_id;
}

static bool prv_log_record(SleepCaptureRecordType type, time_t timestamp_utc, uint16_t value,
                           int8_t quality, uint8_t flags) {
  if (!s_sleep_capture.active || !s_sleep_capture.dls_session) {
    return false;
  }
  if (s_sleep_capture.storage_full && type != SleepCaptureRecordType_Session) {
    ++s_sleep_capture.dropped_records;
    return false;
  }
  if (s_sleep_capture.next_sequence > UINT16_MAX) {
    ++s_sleep_capture.dropped_records;
    s_sleep_capture.storage_full = true;
    if (!s_sleep_capture.reported_log_error) {
      s_sleep_capture.reported_log_error = true;
      PBL_LOG_WRN("Sleep capture sequence capacity reached");
    }
    return false;
  }

  SleepCaptureDataLoggingRecord record = {
    .session_id = s_sleep_capture.session_id,
    // Leave sequence gaps visible when a write fails.
    .sequence = (uint16_t)s_sleep_capture.next_sequence++,
    .timestamp_utc = (uint32_t)timestamp_utc,
    .value = value,
    .quality = quality,
    .type_flags = prv_type_flags(type, flags),
  };
  const DataLoggingResult result = dls_log(s_sleep_capture.dls_session, &record, 1);
  if (result != DATA_LOGGING_SUCCESS) {
    ++s_sleep_capture.dropped_records;
    if (result == DATA_LOGGING_FULL) {
      s_sleep_capture.storage_full = true;
    }
    if (!s_sleep_capture.reported_log_error) {
      s_sleep_capture.reported_log_error = true;
      PBL_LOG_WRN("Sleep capture log failed: %"PRIi32, (int32_t)result);
    }
    return false;
  }
  s_sleep_capture.reported_log_error = false;
  return true;
}

static void prv_flush_motion_epoch(void) {
  if (!s_sleep_capture.active || s_sleep_capture.motion_epoch_start_utc == 0 ||
      s_sleep_capture.motion_sample_count == 0) {
    return;
  }

  // Completeness identifies partial 30-second epochs.
  const uint32_t expected_samples = SLEEP_CAPTURE_EPOCH_SECONDS * 25;
  const uint32_t completeness = MIN(100,
                                    (s_sleep_capture.motion_sample_count * 100) /
                                        expected_samples);
  prv_log_record(SleepCaptureRecordType_Motion, s_sleep_capture.motion_epoch_start_utc,
                 prv_clamp_u16(s_sleep_capture.motion_energy), (int8_t)completeness, 0);
}

static void prv_reset_motion_epoch(time_t epoch_start_utc) {
  s_sleep_capture.motion_epoch_start_utc = epoch_start_utc;
  s_sleep_capture.motion_energy = 0;
  s_sleep_capture.motion_sample_count = 0;
}

static void prv_finish_capture_dls_session(void *data) {
  SleepCaptureDlsFinishData *finish_data = data;
  if (!finish_data->terminal_record_queued) {
    SleepCaptureDataLoggingRecord record = {
      .session_id = finish_data->session_id,
      .sequence = finish_data->sequence,
      .timestamp_utc = (uint32_t)finish_data->end_utc,
      .value = finish_data->dropped_records,
      .quality = SLEEP_CAPTURE_MOTION_QUALITY_UNKNOWN,
      .type_flags = prv_type_flags(
          SleepCaptureRecordType_Session,
          SLEEP_CAPTURE_FLAG_COMPLETE |
              ((finish_data->dropped_records != 0) ? SLEEP_CAPTURE_FLAG_DROPPED : 0)),
    };
    const DataLoggingResult result = dls_log(finish_data->session, &record, 1);
    // The final record is the phone's proof that this is a complete capture. Retry a full
    // staging buffer instead of closing the session and silently leaving valid PPI unusable.
    if (result == DATA_LOGGING_BUSY &&
        system_task_add_callback(prv_finish_capture_dls_session, finish_data)) {
      PBL_LOG_WRN("Sleep capture completion buffer busy; retrying");
      return;
    }
    if (result != DATA_LOGGING_SUCCESS) {
      PBL_LOG_WRN("Sleep capture completion log failed: %"PRIi32, (int32_t)result);
      dls_finish(finish_data->session);
      kernel_free(finish_data);
      return;
    }

    // dls_log only queues the flash write. Yield one System Task turn before closing so the
    // terminal record cannot time out in the staging buffer and disappear from the stream.
    finish_data->terminal_record_queued = true;
    if (system_task_add_callback(prv_finish_capture_dls_session, finish_data)) {
      return;
    }
    PBL_LOG_WRN("Unable to defer sleep capture completion close");
  }
  dls_finish(finish_data->session);
  kernel_free(finish_data);
}

static void prv_finish_capture(time_t now_utc) {
  if (!s_sleep_capture.active) {
    return;
  }

  prv_flush_motion_epoch();
  SleepCaptureDlsFinishData *finish_data = kernel_zalloc_check(sizeof(*finish_data));
  *finish_data = (SleepCaptureDlsFinishData) {
    .session = s_sleep_capture.dls_session,
    .session_id = s_sleep_capture.session_id,
    .sequence = (uint16_t)s_sleep_capture.next_sequence,
    .end_utc = now_utc,
    .dropped_records = prv_clamp_u16(s_sleep_capture.dropped_records),
  };
  s_sleep_capture = (SleepCaptureState) {};

  if (!system_task_add_callback(prv_finish_capture_dls_session, finish_data)) {
    PBL_LOG_WRN("Unable to finish sleep capture logging session");
    dls_finish(finish_data->session);
    kernel_free(finish_data);
  }
}

static void prv_start_capture(time_t now_utc) {
  const Uuid system_uuid = UUID_SYSTEM;
  // Resume the flash stream after a reboot.
  DataLoggingSession *session = dls_create(DlsSystemTagSleepCapture, DATA_LOGGING_BYTE_ARRAY,
                                           sizeof(SleepCaptureDataLoggingRecord),
                                           true /* buffered */, true /* resume */, &system_uuid);
  if (!session) {
    PBL_LOG_ERR("Unable to create sleep capture logging session");
    return;
  }

  s_sleep_capture = (SleepCaptureState) {
    .active = true,
    .session_id = prv_new_session_id(now_utc),
    .dls_session = session,
  };
  prv_reset_motion_epoch(now_utc - (now_utc % SLEEP_CAPTURE_EPOCH_SECONDS));
  prv_log_record(SleepCaptureRecordType_Session, now_utc, 0,
                 SLEEP_CAPTURE_MOTION_QUALITY_UNKNOWN, 0);
  PBL_LOG_INFO("Sleep capture started");
}

void sleep_capture_minute_handler(uint32_t utc_sec, bool heart_rate_enabled, bool sleep_active,
                                  bool enhanced_logging_enabled) {
#ifndef CONFIG_HRM_HRV
  (void)utc_sec;
  (void)heart_rate_enabled;
  (void)sleep_active;
  (void)enhanced_logging_enabled;
#else
  const time_t now_utc = (time_t)utc_sec;
  const bool should_capture = enhanced_logging_enabled && heart_rate_enabled && sleep_active &&
                              prv_is_capture_window(now_utc);
  if (should_capture && !s_sleep_capture.active) {
    prv_start_capture(now_utc);
  } else if (!should_capture && s_sleep_capture.active) {
    prv_finish_capture(now_utc);
  }
#endif
}

bool sleep_capture_is_active(void) {
#ifdef CONFIG_HRM_HRV
  return s_sleep_capture.active;
#else
  return false;
#endif
}

void sleep_capture_handle_hrm_event(const PebbleHRMEvent *event) {
#ifdef CONFIG_HRM_HRV
  if (!s_sleep_capture.active) {
    return;
  }

  const time_t now_utc = rtc_get_time();
  switch (event->event_type) {
    case HRMEvent_HRV:
      if (event->hrv.ppi_ms != 0) {
        // The API exposes receipt UTC, not a millisecond event timestamp.
        prv_log_record(SleepCaptureRecordType_Ppi, now_utc, event->hrv.ppi_ms,
                       event->hrv.quality, 0);
      }
      break;
    case HRMEvent_BPM:
      if (now_utc - s_sleep_capture.last_logged_bpm_utc >= SLEEP_CAPTURE_BPM_PERIOD_SECONDS &&
          prv_log_record(SleepCaptureRecordType_Bpm, now_utc, event->bpm.bpm,
                         event->bpm.quality, 0)) {
        s_sleep_capture.last_logged_bpm_utc = now_utc;
      }
      break;
    default:
      break;
  }
#else
  (void)event;
#endif
}

void sleep_capture_handle_accel(const AccelRawData *data, uint32_t num_samples) {
#ifdef CONFIG_HRM_HRV
  if (!s_sleep_capture.active || !data || num_samples == 0) {
    return;
  }

  const time_t now_utc = rtc_get_time();
  const time_t epoch_start = now_utc - (now_utc % SLEEP_CAPTURE_EPOCH_SECONDS);
  if (epoch_start != s_sleep_capture.motion_epoch_start_utc) {
    prv_flush_motion_epoch();
    prv_reset_motion_epoch(epoch_start);
  }

  for (uint32_t i = 0; i < num_samples; ++i) {
    if (s_sleep_capture.has_previous_accel) {
      const int32_t dx = (int32_t)data[i].x - s_sleep_capture.previous_accel.x;
      const int32_t dy = (int32_t)data[i].y - s_sleep_capture.previous_accel.y;
      const int32_t dz = (int32_t)data[i].z - s_sleep_capture.previous_accel.z;
      // Store a compact, gravity-independent motion feature.
      const uint32_t energy = (abs(dx) + abs(dy) + abs(dz)) >> 3;
      s_sleep_capture.motion_energy = MIN(UINT16_MAX, s_sleep_capture.motion_energy + energy);
    }
    s_sleep_capture.previous_accel = data[i];
    s_sleep_capture.has_previous_accel = true;
    if (s_sleep_capture.motion_sample_count != UINT16_MAX) {
      ++s_sleep_capture.motion_sample_count;
    }
  }
#else
  (void)data;
  (void)num_samples;
#endif
}

void sleep_capture_deinit(void) {
#ifdef CONFIG_HRM_HRV
  prv_finish_capture(rtc_get_time());
#endif
}
