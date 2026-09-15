/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "clar.h"

#include "pbl/services/activity/activity_private.h"
#include "pbl/services/data_logging/data_logging_service.h"
#include "pbl/services/hrm/hrm_manager.h"
#include "pbl/services/system_task.h"
#include "pbl/util/size.h"
#include "services/activity/sleep_capture.h"

#include "fake_pbl_malloc.h"
#include "fake_rtc.h"
#include "stubs_logging.h"

#include <string.h>

#define TEST_DAY_START_UTC (10 * SECONDS_PER_DAY)
#define TEST_NIGHT_UTC (TEST_DAY_START_UTC + (22 * SECONDS_PER_HOUR))

typedef enum {
  TestRecordTypePpi = 1,
  TestRecordTypeBpm = 2,
  TestRecordTypeMotion = 3,
  TestRecordTypeSession = 4,
} TestRecordType;

typedef struct PACKED {
  uint32_t session_id;
  uint32_t sequence;
  uint32_t timestamp_utc;
  uint16_t value;
  int8_t quality;
  uint8_t type_flags;
} TestSleepRecord;

#define TEST_RECORD_VERSION 2
#define TEST_RECORD_TYPE_MASK 0x07
#define TEST_RECORD_FLAG_COMPLETE (1 << 3)
#define TEST_RECORD_FLAG_DROPPED (1 << 4)
#define TEST_RECORD_VERSION_SHIFT 6

static bool s_dls_active;
static bool s_fail_dls_create;
static int s_dls_create_count;
static int s_dls_finish_count;
static TestSleepRecord s_records[64];
static size_t s_num_records;
static DataLoggingResult s_log_results[8];
static size_t s_num_log_results;
static size_t s_next_log_result;

static uint8_t prv_record_type(const TestSleepRecord *record) {
  return record->type_flags & TEST_RECORD_TYPE_MASK;
}

static uint8_t prv_record_version(const TestSleepRecord *record) {
  return record->type_flags >> TEST_RECORD_VERSION_SHIFT;
}

static void prv_queue_log_result(DataLoggingResult result) {
  cl_assert(s_num_log_results < ARRAY_LENGTH(s_log_results));
  s_log_results[s_num_log_results++] = result;
}

DataLoggingSession *dls_create(uint32_t tag, DataLoggingItemType item_type, uint16_t item_size,
                               bool buffered, bool resume, const Uuid *uuid) {
  cl_assert_equal_i(tag, DlsSystemTagSleepCaptureV2);
  cl_assert_equal_i(item_type, DATA_LOGGING_BYTE_ARRAY);
  cl_assert_equal_i(item_size, sizeof(TestSleepRecord));
  cl_assert_equal_b(buffered, true);
  cl_assert_equal_b(resume, true);
  cl_assert(uuid != NULL);
  if (s_fail_dls_create) {
    ++s_dls_create_count;
    return NULL;
  }
  cl_assert_equal_b(s_dls_active, false);
  s_dls_active = true;
  ++s_dls_create_count;
  return (DataLoggingSession *)1;
}

DataLoggingResult dls_log(DataLoggingSession *session, const void *data, uint32_t num_items) {
  cl_assert_equal_p(session, (DataLoggingSession *)1);
  cl_assert_equal_b(s_dls_active, true);
  cl_assert_equal_i(num_items, 1);

  DataLoggingResult result = DATA_LOGGING_SUCCESS;
  if (s_next_log_result < s_num_log_results) {
    result = s_log_results[s_next_log_result++];
  }
  if (result == DATA_LOGGING_SUCCESS) {
    cl_assert(s_num_records < ARRAY_LENGTH(s_records));
    memcpy(&s_records[s_num_records++], data, sizeof(TestSleepRecord));
  }
  return result;
}

void dls_finish(DataLoggingSession *session) {
  cl_assert_equal_p(session, (DataLoggingSession *)1);
  cl_assert_equal_b(s_dls_active, true);
  s_dls_active = false;
  ++s_dls_finish_count;
}

static struct {
  SystemTaskEventCallback callback;
  void *data;
} s_callbacks[16];
static size_t s_num_callbacks;
static int s_rejected_callbacks_remaining;

bool system_task_add_callback(SystemTaskEventCallback callback, void *data) {
  if (s_rejected_callbacks_remaining > 0) {
    --s_rejected_callbacks_remaining;
    return false;
  }
  cl_assert(s_num_callbacks < ARRAY_LENGTH(s_callbacks));
  s_callbacks[s_num_callbacks].callback = callback;
  s_callbacks[s_num_callbacks].data = data;
  ++s_num_callbacks;
  return true;
}

static void prv_invoke_next_callback(void) {
  cl_assert(s_num_callbacks > 0);
  SystemTaskEventCallback callback = s_callbacks[0].callback;
  void *data = s_callbacks[0].data;
  memmove(&s_callbacks[0], &s_callbacks[1],
          (s_num_callbacks - 1) * sizeof(s_callbacks[0]));
  --s_num_callbacks;
  callback(data);
}

static void prv_invoke_all_callbacks(void) {
  while (s_num_callbacks > 0) {
    prv_invoke_next_callback();
  }
}

bool rng_rand(uint32_t *rand_out) {
  *rand_out = 0x12345678;
  return true;
}

static void prv_set_time(time_t utc) {
  rtc_set_time(utc);
  fake_rtc_set_ticks((RtcTicks)utc * PBL_TICK_HZ);
}

static void prv_start_capture(void) {
  prv_set_time(TEST_NIGHT_UTC);
  sleep_capture_minute_handler(TEST_NIGHT_UTC, true, true, true);
  cl_assert_equal_b(sleep_capture_is_active(), true);
  cl_assert_equal_b(s_dls_active, true);
  cl_assert_equal_i(s_dls_create_count, 1);
  cl_assert_equal_i(s_num_records, 1);
  cl_assert_equal_i(prv_record_type(&s_records[0]), TestRecordTypeSession);
  cl_assert_equal_i(prv_record_version(&s_records[0]), TEST_RECORD_VERSION);
  cl_assert_equal_i(s_records[0].session_id, 0x12345678);
  cl_assert_equal_i(s_records[0].sequence, 0);
  cl_assert_equal_i(s_records[0].timestamp_utc, TEST_NIGHT_UTC);
}

static void prv_stop_capture(time_t utc) {
  prv_set_time(utc);
  sleep_capture_minute_handler((uint32_t)utc, true, false, true);
  cl_assert_equal_b(sleep_capture_is_active(), false);
}

void test_sleep_capture__initialize(void) {
  fake_rtc_init(0, TEST_NIGHT_UTC);
  s_dls_active = false;
  s_fail_dls_create = false;
  s_dls_create_count = 0;
  s_dls_finish_count = 0;
  s_num_records = 0;
  s_num_log_results = 0;
  s_next_log_result = 0;
  s_num_callbacks = 0;
  s_rejected_callbacks_remaining = 0;
  sleep_capture_deinit();
}

void test_sleep_capture__cleanup(void) {
  sleep_capture_deinit();
  prv_invoke_all_callbacks();
}

void test_sleep_capture__requires_every_gate_and_obeys_window_boundaries(void) {
  const time_t daytime = TEST_DAY_START_UTC + (12 * SECONDS_PER_HOUR);
  sleep_capture_minute_handler(daytime, true, true, true);
  sleep_capture_minute_handler(TEST_NIGHT_UTC, false, true, true);
  sleep_capture_minute_handler(TEST_NIGHT_UTC, true, false, true);
  sleep_capture_minute_handler(TEST_NIGHT_UTC, true, true, false);
  cl_assert_equal_i(s_dls_create_count, 0);
  cl_assert_equal_b(sleep_capture_is_active(), false);

  const time_t start_boundary = TEST_DAY_START_UTC + (21 * SECONDS_PER_HOUR);
  sleep_capture_minute_handler(start_boundary, true, true, true);
  cl_assert_equal_b(sleep_capture_is_active(), true);
  prv_stop_capture(TEST_DAY_START_UTC + (12 * SECONDS_PER_HOUR));
  prv_invoke_all_callbacks();
  cl_assert_equal_i(s_dls_finish_count, 1);
}

void test_sleep_capture__retries_creation_after_transient_dls_failure(void) {
  s_fail_dls_create = true;
  sleep_capture_minute_handler(TEST_NIGHT_UTC, true, true, true);
  cl_assert_equal_b(sleep_capture_is_active(), false);
  cl_assert_equal_b(s_dls_active, false);
  cl_assert_equal_i(s_dls_create_count, 1);
  cl_assert_equal_i(s_num_records, 0);

  s_fail_dls_create = false;
  sleep_capture_minute_handler(TEST_NIGHT_UTC + SECONDS_PER_MINUTE, true, true, true);
  cl_assert_equal_b(sleep_capture_is_active(), true);
  cl_assert_equal_b(s_dls_active, true);
  cl_assert_equal_i(s_dls_create_count, 2);
  cl_assert_equal_i(s_num_records, 1);
}

void test_sleep_capture__records_ppi_and_throttles_bpm_without_sequence_gaps(void) {
  prv_start_capture();

  PebbleHRMEvent zero_ppi = {
    .event_type = HRMEvent_HRV,
    .hrv = { .ppi_ms = 0, .quality = HRMQuality_Good },
  };
  sleep_capture_handle_hrm_event(&zero_ppi);
  cl_assert_equal_i(s_num_records, 1);

  PebbleHRMEvent ppi = {
    .event_type = HRMEvent_HRV,
    .hrv = { .ppi_ms = 812, .quality = HRMQuality_Excellent },
  };
  sleep_capture_handle_hrm_event(&ppi);
  cl_assert_equal_i(s_num_records, 2);
  cl_assert_equal_i(prv_record_type(&s_records[1]), TestRecordTypePpi);
  cl_assert_equal_i(s_records[1].sequence, 1);
  cl_assert_equal_i(s_records[1].value, 812);

  PebbleHRMEvent bpm = {
    .event_type = HRMEvent_BPM,
    .bpm = { .bpm = 73, .quality = HRMQuality_Good },
  };
  sleep_capture_handle_hrm_event(&bpm);
  sleep_capture_handle_hrm_event(&bpm);
  prv_set_time(TEST_NIGHT_UTC + 29);
  sleep_capture_handle_hrm_event(&bpm);
  cl_assert_equal_i(s_num_records, 3);
  cl_assert_equal_i(prv_record_type(&s_records[2]), TestRecordTypeBpm);
  cl_assert_equal_i(s_records[2].sequence, 2);
  cl_assert_equal_i(s_records[2].value, 73);

  prv_set_time(TEST_NIGHT_UTC + 30);
  sleep_capture_handle_hrm_event(&bpm);
  cl_assert_equal_i(s_num_records, 4);
  cl_assert_equal_i(s_records[3].sequence, 3);
  cl_assert_equal_i(s_records[3].timestamp_utc, TEST_NIGHT_UTC + 30);
}

void test_sleep_capture__flushes_motion_epochs_with_energy_and_completeness(void) {
  prv_start_capture();
  const AccelRawData first_epoch[] = {
    { .x = 0, .y = 0, .z = 0 },
    { .x = 80, .y = -40, .z = 16 },
  };
  sleep_capture_handle_accel(first_epoch, ARRAY_LENGTH(first_epoch));

  prv_set_time(TEST_NIGHT_UTC + 30);
  const AccelRawData next_epoch[] = {
    { .x = 80, .y = -40, .z = 16 },
  };
  sleep_capture_handle_accel(next_epoch, ARRAY_LENGTH(next_epoch));

  cl_assert_equal_i(s_num_records, 2);
  cl_assert_equal_i(prv_record_type(&s_records[1]), TestRecordTypeMotion);
  cl_assert_equal_i(s_records[1].sequence, 1);
  cl_assert_equal_i(s_records[1].timestamp_utc, TEST_NIGHT_UTC);
  cl_assert_equal_i(s_records[1].value, (80 + 40 + 16) >> 3);
  cl_assert_equal_i(s_records[1].quality, 0);
}

void test_sleep_capture__busy_terminal_retries_then_closes_exactly_once(void) {
  prv_start_capture();
  prv_queue_log_result(DATA_LOGGING_BUSY);
  prv_queue_log_result(DATA_LOGGING_SUCCESS);

  prv_stop_capture(TEST_NIGHT_UTC + SECONDS_PER_HOUR);
  cl_assert_equal_i(s_num_callbacks, 1);
  prv_invoke_next_callback();
  cl_assert_equal_i(s_num_callbacks, 1);
  cl_assert_equal_i(s_dls_finish_count, 0);
  prv_invoke_next_callback();
  cl_assert_equal_i(s_num_callbacks, 1);
  cl_assert_equal_i(s_num_records, 2);

  const TestSleepRecord *completion = &s_records[1];
  cl_assert_equal_i(prv_record_type(completion), TestRecordTypeSession);
  cl_assert_equal_i(prv_record_version(completion), TEST_RECORD_VERSION);
  cl_assert((completion->type_flags & TEST_RECORD_FLAG_COMPLETE) != 0);
  cl_assert_equal_i(completion->sequence, 1);
  prv_invoke_next_callback();
  cl_assert_equal_i(s_dls_finish_count, 1);
  cl_assert_equal_b(s_dls_active, false);
  cl_assert_equal_i(s_num_callbacks, 0);
}

void test_sleep_capture__full_stream_marks_drops_and_stops_writing_payloads(void) {
  prv_start_capture();
  prv_queue_log_result(DATA_LOGGING_FULL);

  PebbleHRMEvent ppi = {
    .event_type = HRMEvent_HRV,
    .hrv = { .ppi_ms = 750, .quality = HRMQuality_Good },
  };
  sleep_capture_handle_hrm_event(&ppi);
  sleep_capture_handle_hrm_event(&ppi);
  cl_assert_equal_i(s_num_records, 1);

  prv_stop_capture(TEST_NIGHT_UTC + SECONDS_PER_HOUR);
  prv_invoke_all_callbacks();
  cl_assert_equal_i(s_num_records, 2);
  const TestSleepRecord *completion = &s_records[1];
  cl_assert_equal_i(completion->sequence, 2);
  cl_assert_equal_i(completion->value, 2);
  cl_assert((completion->type_flags & TEST_RECORD_FLAG_COMPLETE) != 0);
  cl_assert((completion->type_flags & TEST_RECORD_FLAG_DROPPED) != 0);
  cl_assert_equal_i(s_dls_finish_count, 1);
}

void test_sleep_capture__callback_rejection_closes_session_without_double_finish(void) {
  prv_start_capture();
  s_rejected_callbacks_remaining = 1;
  prv_stop_capture(TEST_NIGHT_UTC + SECONDS_PER_HOUR);

  cl_assert_equal_i(s_num_callbacks, 0);
  cl_assert_equal_i(s_dls_finish_count, 1);
  cl_assert_equal_b(s_dls_active, false);
  sleep_capture_deinit();
  cl_assert_equal_i(s_dls_finish_count, 1);
}

void test_sleep_capture__defer_rejection_after_terminal_write_closes_immediately(void) {
  prv_start_capture();
  prv_stop_capture(TEST_NIGHT_UTC + SECONDS_PER_HOUR);
  cl_assert_equal_i(s_num_callbacks, 1);

  s_rejected_callbacks_remaining = 1;
  prv_invoke_next_callback();
  cl_assert_equal_i(s_num_records, 2);
  cl_assert_equal_i(prv_record_type(&s_records[1]), TestRecordTypeSession);
  cl_assert((s_records[1].type_flags & TEST_RECORD_FLAG_COMPLETE) != 0);
  cl_assert_equal_i(s_num_callbacks, 0);
  cl_assert_equal_i(s_dls_finish_count, 1);
  cl_assert_equal_b(s_dls_active, false);
}
