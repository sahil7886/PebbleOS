# Sleep tracking delivery regression

## Deployment boundary

The last confirmed deployment before the first missing night was reconstructed from the Codex task
`Fix Workout HR Underreporting` and the device logs produced during that task:

| Time (UTC) | Component | Deployed revision | Evidence |
| --- | --- | --- | --- |
| 2026-08-31 03:00 | iOS app | `cd408c07` | Xcode device install completed from a clean `master` checkout |
| 2026-08-31 03:13 | Obelix firmware, slot 0 | `c70fc5b3a` | Runtime log reported `v4.36.0-5-gc70fc5b3a` |

The preceding confirmed app revision was `1abbcad2` on August 26. The preceding firmware package
was built from `6554aac59` on August 24. There was one commit in each deployment interval:

- Firmware `c70fc5b3a`: removed HRV/PPI subscriptions from manual workouts and retained BPM-only
  capture. It did not change the sleep classifier or activity-session export.
- Mobile `cd408c07`: corrected workout end-time export and changed iOS project/export UI files. It
  did not change ordinary health parsing or persistence.

The deployment made an existing delivery weakness visible; the sensor classifier itself was not the
failure point.

## Reproduction evidence

The phone's minute-level database rows from the missing August 31 night were converted into
`tests/fixtures/activity/sleep_samples/aug31_regression.c`. This fixture contains the recorded
orientation, vibration, light, and step fields and is evaluated by the production
Kraepelin algorithm.

The exact oracle is:

- total sleep: 420 minutes
- restful/deep sleep: 176 minutes
- first sleep start: minute 36 (00:36 local)
- final sleep end: minute 526 (08:46 local)
- awake at the end of the capture

This proves the captured input was sufficient, the classifier entered sleep, and the wake-up path
finalized the session. The phone database contained automatic walk overlays after the same date, so
global activity tracking was enabled. There is no manual sleep on/off preference in either product.

## Failure modes and fixes

Ordinary health DataLogging originally acknowledged a payload before its asynchronous database work
completed. Mobile revision `e6974b65` changed this to await persistence and NACK missing sessions or
database failures. This fix is present in current `master`.

Firmware activity export had a complementary at-most-once error. `prv_log_activities()` advanced and
persisted the per-class export watermark even if `dls_create()` or `dls_log()` failed. A sleep session
is normally emitted only once after waking, so a transient PFS/DataLogging failure permanently hid
that night from the phone. Activity export now returns an acceptance result, advances a watermark
only on success, and blocks later records of the same class during that pass so retries remain in
order. Other activity classes may continue independently.

## Regression tests

Firmware tests use production classifier and activity-session code. Diagnostics are emitted only by
the host test binary.

```sh
PATH="/path/to/pebble-sdk/arm-none-eabi/bin:$PWD/.venv/bin:$PATH" \
  .venv/bin/python ./waf test --out=/tmp/pebble-sleep-tests \
  -M '.*test_kraepelin_algorithm[.]c$' -T sleep_tests --show_output --no_images

PATH="/path/to/pebble-sdk/arm-none-eabi/bin:$PWD/.venv/bin:$PATH" \
  .venv/bin/python ./waf test --out=/tmp/pebble-sleep-tests \
  -M '.*test_activity[.]c$' --show_output --no_images
```

The activity export test injects failures at both session creation and record append. It records the
accepted record count plus sleep, restful-sleep, and step watermarks after each pass. It verifies:

1. failed sleep records do not advance their watermark;
2. later sleep records cannot skip a failed earlier record;
3. independent activity classes still make progress;
4. retry delivers both sleep records in order; and
5. another minute tick does not duplicate accepted records.

Mobile tests construct the packed 26-byte firmware record directly. They cover version 3 field
decoding, the August 31 sleep/deep-sleep aggregate through the real UI grouper, unknown-record
alignment, partial trailing records, deterministic replay, missing/closed session NACK behavior,
database failure before ACK, real Room persistence before ACK, and replay deduplication. The JVM ACK
tests mock only the DAO boundary; the iOS integration tests use generated production DAOs and a real
temporary SQLite database. Assertion messages contain the wire records, parsed records, session
aggregates, persistence call order, ACK state, and temporary database path; none of these diagnostics
are in application runtime logging.

```sh
JAVA_HOME=/path/to/jdk-17 ./gradlew :libpebble3:jvmTest \
  --tests 'io.rebble.libpebblecommon.health.OverlayPipelineTest' \
  --tests 'io.rebble.libpebblecommon.datalogging.HealthDataProcessorAckTest'

JAVA_HOME=/path/to/jdk-17 ./gradlew :libpebble3:iosSimulatorArm64Test \
  --tests 'io.rebble.libpebblecommon.datalogging.HealthOverlayPersistenceIntegrationTest'
```
