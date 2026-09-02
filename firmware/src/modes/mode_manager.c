#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include "app_config.h"
#include "ble/ble.h"
#include "mode_manager.h"
#include "modules/camera/camera.h"
#include "modules/imu/imu.h"
#include "modules/imu/imu_stream.h"
#include "modules/tof/tof.h"

LOG_MODULE_REGISTER(mode_manager, LOG_LEVEL_INF);

#define EVENT_MODE_REQUEST BIT(0)
#define EVENT_CAPTURE_TRIGGER BIT(1)
#define EVENT_RESEND_REQUEST BIT(2)
#define EVENT_FRAME_ACK BIT(3)

K_EVENT_DEFINE(mode_events);

static volatile enum fing_mode requested_mode = FING_MODE_IDLE;
static enum fing_mode active_mode = FING_MODE_IDLE;
static uint32_t next_frame_id;
static volatile uint32_t last_trigger_ms;
static uint32_t capture_attempt_id;
static uint32_t last_frame_id;
static uint32_t last_frame_timestamp_ms;
static uint16_t last_frame_distance_mm = CAPTURE_STATUS_DISTANCE_UNKNOWN;
static bool last_frame_valid;
static bool last_transfer_failed;
static volatile uint32_t acknowledged_frame_id;
static volatile bool camera_diagnostics_active;
static volatile uint32_t diagnostic_attempt_id;
static volatile uint16_t diagnostic_distance_mm;
static volatile uint8_t diagnostic_burst_frame;

#define CAMERA_DIAGNOSTIC_INITIAL_DELAY_MS 1500U
#define CAMERA_DIAGNOSTIC_INTERVAL_MS 1000U
#define CAMERA_DIAGNOSTIC_WORK_QUEUE_PRIORITY 5
#define CAMERA_DIAGNOSTIC_WORK_QUEUE_STACK_SIZE 1024

static void camera_diagnostic_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(camera_diagnostic_work,
                               camera_diagnostic_work_handler);
static struct k_work_q camera_diagnostic_work_queue;
K_THREAD_STACK_DEFINE(camera_diagnostic_work_queue_stack,
                      CAMERA_DIAGNOSTIC_WORK_QUEUE_STACK_SIZE);

static void camera_diagnostic_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    if (!camera_diagnostics_active) {
        return;
    }

    struct camera_diagnostic_snapshot snapshot;
    camera_get_diagnostic_snapshot(&snapshot);
    /* detail carries the signed camera error. progress is the sensor row.
     * The upper nibble of total carries the diagnostic stage; its lower
     * nibble carries the current frame in the AE burst. */
    uint8_t encoded_stage_frame =
        ((uint8_t)snapshot.stage << 4) | (diagnostic_burst_frame & 0x0FU);
    ble_publish_capture_status(CAPTURE_STATUS_CAMERA_DIAGNOSTIC,
                               diagnostic_attempt_id,
                               diagnostic_distance_mm,
                               snapshot.last_error,
                               snapshot.row,
                               encoded_stage_frame);
    (void)k_work_reschedule_for_queue(&camera_diagnostic_work_queue,
                                     &camera_diagnostic_work,
                                     K_MSEC(CAMERA_DIAGNOSTIC_INTERVAL_MS));
}

static void camera_diagnostics_start(uint32_t attempt_id,
                                     uint16_t distance_mm,
                                     uint8_t total_frames)
{
    diagnostic_attempt_id = attempt_id;
    diagnostic_distance_mm = distance_mm;
    diagnostic_burst_frame = 1U;
    ARG_UNUSED(total_frames);
    camera_diagnostics_active = true;
    (void)k_work_reschedule_for_queue(&camera_diagnostic_work_queue,
                                     &camera_diagnostic_work,
                                     K_MSEC(CAMERA_DIAGNOSTIC_INITIAL_DELAY_MS));
}

static void camera_diagnostics_stop(void)
{
    camera_diagnostics_active = false;
    (void)k_work_cancel_delayable(&camera_diagnostic_work);
}

static uint8_t encode_camera_diagnostic(const struct camera_diagnostic_snapshot *snapshot,
                                        uint8_t burst_frame)
{
    return ((uint8_t)snapshot->stage << 4) | (burst_frame & 0x0FU);
}

static void publish_capture_readiness(void)
{
    if (last_frame_valid && last_transfer_failed) {
        ble_publish_capture_status(CAPTURE_STATUS_RESEND_AVAILABLE,
                                   capture_attempt_id,
                                   last_frame_distance_mm,
                                   0, 0, 0);
    } else {
        ble_publish_capture_status(CAPTURE_STATUS_WAITING,
                                   capture_attempt_id,
                                   CAPTURE_STATUS_DISTANCE_UNKNOWN,
                                   0, 0, 0);
    }
}

static void on_capture_gesture(void)
{
    last_trigger_ms = k_uptime_get_32();
    k_event_post(&mode_events, EVENT_CAPTURE_TRIGGER);
}

static void rearm_capture_detector_if_active(void)
{
    if (active_mode == FING_MODE_CAPTURE &&
        requested_mode == FING_MODE_CAPTURE) {
        imu_rearm_capture_detector();
    }
}

static bool distance_is_in_range(uint16_t distance_mm)
{
    return distance_mm >= TOF_MIN_DISTANCE_MM &&
           distance_mm <= TOF_MAX_DISTANCE_MM;
}

static int capture_full_frame(void)
{
    int result = -EAGAIN;

    for (int attempt = 1; attempt <= CAMERA_CAPTURE_MAX_ATTEMPTS; ++attempt) {
        result = camera_capture_frame();
        if (result == FRAME_BYTES) {
            break;
        }
        /* -ETIMEDOUT means the sensor produced no line and left an async SPIS
         * transaction armed. Repeating immediately only spends several more
         * seconds in the same unrecoverable driver context. */
        if (result != -EAGAIN) {
            break;
        }
        LOG_WRN("Capture sync miss %d/%d (err=%d)",
                attempt, CAMERA_CAPTURE_MAX_ATTEMPTS, result);
        k_msleep(50);
    }
    return result;
}

static int capture_ae_stabilized_frame(uint32_t attempt_id,
                                       uint16_t distance_mm)
{
    const unsigned int total_frames = CAMERA_AE_DISCARD_FRAMES + 1U;

    for (unsigned int frame = 1U; frame <= total_frames; ++frame) {
        /* The BLE callback updates requested_mode immediately, even while the
         * mode-manager thread is inside this blocking burst.  Check it between
         * frames so IDLE does not have to wait for the complete burst. */
        if (requested_mode != FING_MODE_CAPTURE) {
            LOG_INF("AE burst cancelled after %u/%u frames",
                    frame - 1U, total_frames);
            return -ECANCELED;
        }

        diagnostic_burst_frame = (uint8_t)frame;
        int result = capture_full_frame();
        if (result != FRAME_BYTES) {
            return result;
        }

        (void)camera_log_ae_state(frame);
        ble_publish_capture_status(CAPTURE_STATUS_EXPOSURE_SETTLING,
                                   attempt_id, distance_mm, 0,
                                   (uint8_t)frame, (uint8_t)total_frames);
        if (frame <= CAMERA_AE_DISCARD_FRAMES) {
            LOG_INF("Discarded AE settling frame %u/%u",
                    frame, CAMERA_AE_DISCARD_FRAMES);
        }
    }

    return FRAME_BYTES;
}

static void enter_idle(void)
{
    /* The active profile is stopped before the BLE-visible mode changes.
     * That makes a mode acknowledgement mean its data path is fully quiet. */
    if (active_mode == FING_MODE_CAPTURE) {
        imu_stop_capture_detector();
        (void)camera_standby();
    } else if (active_mode == FING_MODE_STREAM) {
        imu_stream_stop();
    }

    active_mode = FING_MODE_IDLE;
    ble_set_active_mode(active_mode);
    ble_set_preferred_phy(active_mode);
    LOG_INF("Mode is IDLE");
}

static int enter_mode(enum fing_mode target)
{
    int err = 0;

    if (target == FING_MODE_IDLE) {
        return 0;
    }
    if (target == FING_MODE_CAPTURE) {
        /* Wake the camera once for the complete Phase 1/2 session.  Keeping
         * MCLK running between gestures avoids the unreliable repeated
         * stop/start cycle that caused the first photo to work and later
         * photos to lose the camera's I2C acknowledgement. */
        err = camera_wake_up();
        if (err == 0) {
            err = imu_start_capture_detector();
        }
        if (err != 0) {
            (void)camera_standby();
        }
    } else if (target == FING_MODE_STREAM) {
        err = imu_stream_start();
    } else {
        return -EINVAL;
    }

    if (err != 0) {
        LOG_ERR("Cannot enter mode %u: %d", target, err);
        return err;
    }

    active_mode = target;
    ble_set_active_mode(active_mode);
    ble_set_preferred_phy(active_mode);
    if (target == FING_MODE_CAPTURE) {
        publish_capture_readiness();
    }
    LOG_INF("Mode is %s", target == FING_MODE_CAPTURE ? "CAPTURE" : "STREAM");
    return 0;
}

static void transition_to(enum fing_mode target)
{
    if (target == active_mode) {
        ble_set_active_mode(active_mode);
        ble_set_preferred_phy(active_mode);
        if (active_mode == FING_MODE_CAPTURE) {
            publish_capture_readiness();
        }
        return;
    }

    enter_idle();
    if (enter_mode(target) != 0) {
        /* A failed enter always leaves the device in the safe idle state. */
        enter_idle();
    }
}

static void handle_capture_trigger(void)
{
    if (active_mode != FING_MODE_CAPTURE) {
        LOG_INF("Ignoring stale gesture trigger outside capture mode");
        return;
    }

    uint32_t trigger_ms = last_trigger_ms;
    uint32_t attempt_id = ++capture_attempt_id;
    ble_publish_capture_status(CAPTURE_STATUS_GESTURE_DETECTED,
                               attempt_id,
                               CAPTURE_STATUS_DISTANCE_UNKNOWN,
                               0, 0, 0);
    ble_publish_capture_status(CAPTURE_STATUS_TOF_CHECKING,
                               attempt_id,
                               CAPTURE_STATUS_DISTANCE_UNKNOWN,
                               0, 0, 0);
    uint16_t distance_mm;
    int err = tof_measure_mm(&distance_mm);
    if (err != 0) {
        LOG_ERR("ToF measurement failed: %d", err);
        ble_publish_capture_status(CAPTURE_STATUS_TOF_ERROR,
                                   attempt_id,
                                   CAPTURE_STATUS_DISTANCE_UNKNOWN,
                                   (int16_t)err, 0, 0);
        rearm_capture_detector_if_active();
        return;
    }
    if (!distance_is_in_range(distance_mm)) {
        LOG_INF("Gesture ignored: %u mm outside [%u, %u]",
                distance_mm, TOF_MIN_DISTANCE_MM, TOF_MAX_DISTANCE_MM);
        ble_publish_capture_status(CAPTURE_STATUS_TOF_REJECTED,
                                   attempt_id, distance_mm, 0, 0, 0);
        rearm_capture_detector_if_active();
        return;
    }
    ble_publish_capture_status(CAPTURE_STATUS_TOF_ACCEPTED,
                               attempt_id, distance_mm, 0, 0, 0);

    camera_diagnostics_start(attempt_id, distance_mm,
                             CAMERA_AE_DISCARD_FRAMES + 1U);

    int length = capture_ae_stabilized_frame(attempt_id, distance_mm);
    camera_diagnostics_stop();
    if (length == -ECANCELED) {
        LOG_INF("Capture cancelled by mode request");
        ble_publish_capture_status(CAPTURE_STATUS_CANCELLED,
                                   attempt_id, distance_mm,
                                   (int16_t)length, 0, 0);
        rearm_capture_detector_if_active();
        return;
    }
    if (length != FRAME_BYTES) {
        struct camera_diagnostic_snapshot snapshot;
        camera_get_diagnostic_snapshot(&snapshot);
        LOG_ERR("Camera capture failed: %d", length);
        int recovery_err = camera_recover_stream();
        if (recovery_err != 0) {
            LOG_ERR("Post-failure camera recovery failed: %d", recovery_err);
        }
        ble_publish_capture_status(CAPTURE_STATUS_CAMERA_ERROR,
                                   attempt_id, distance_mm,
                                   (int16_t)length, snapshot.row,
                                   encode_camera_diagnostic(&snapshot,
                                                            diagnostic_burst_frame));
        rearm_capture_detector_if_active();
        return;
    }
    LOG_INF("Final image ready %u ms after gesture trigger",
            k_uptime_get_32() - trigger_ms);
    last_frame_id = next_frame_id++;
    last_frame_timestamp_ms = k_uptime_get_32();
    last_frame_distance_mm = distance_mm;
    last_frame_valid = true;
    uint8_t *frame = camera_get_frame_buffer();
    uint32_t frame_crc = crc32_ieee(frame, FRAME_BYTES);
    LOG_INF("Captured frame %u timestamp=%u CRC32=0x%08x",
            last_frame_id, last_frame_timestamp_ms, frame_crc);
    /* Only an acknowledgement sent after iOS saves the complete frame clears
     * this flag. Enqueuing every BLE notification is not proof of delivery. */
    last_transfer_failed = true;
    ble_publish_capture_status(CAPTURE_STATUS_IMAGE_READY,
                               attempt_id, distance_mm, 0, 0, 0);
    if (!ble_can_send_frame()) {
        last_transfer_failed = true;
        LOG_WRN("Photo retained because app is not subscribed");
        ble_publish_capture_status(CAPTURE_STATUS_TRANSFER_FAILED,
                                   attempt_id, distance_mm,
                                   -ENOTCONN, 0, 0);
        rearm_capture_detector_if_active();
        return;
    }

    ble_publish_capture_status(CAPTURE_STATUS_TRANSFER_STARTED,
                               attempt_id, distance_mm, 0, 0, 0);
    err = ble_send_frame(frame, FRAME_BYTES, last_frame_id,
                         last_frame_timestamp_ms);
    if (err == -ECANCELED) {
        last_transfer_failed = true;
        LOG_INF("BLE image transfer cancelled by mode request");
        ble_publish_capture_status(CAPTURE_STATUS_CANCELLED,
                                   attempt_id, distance_mm,
                                   (int16_t)err, 0, 0);
    } else if (err != 0) {
        last_transfer_failed = true;
        LOG_ERR("BLE image transfer failed: %d", err);
        ble_publish_capture_status(CAPTURE_STATUS_TRANSFER_FAILED,
                                   attempt_id, distance_mm,
                                   (int16_t)err, 0, 0);
    } else {
        ble_publish_capture_status(CAPTURE_STATUS_AWAITING_APP_ACK,
                                   attempt_id, distance_mm, 0, 0, 0);
        LOG_INF("Image queued to BLE %u ms after gesture trigger; awaiting app ACK",
                k_uptime_get_32() - trigger_ms);
    }
    rearm_capture_detector_if_active();
}

static void handle_resend_request(void)
{
    if (active_mode != FING_MODE_CAPTURE || !last_frame_valid) {
        LOG_WRN("No retained capture is available to resend");
        return;
    }
    if (!ble_can_send_frame()) {
        last_transfer_failed = true;
        ble_publish_capture_status(CAPTURE_STATUS_TRANSFER_FAILED,
                                   capture_attempt_id,
                                   last_frame_distance_mm,
                                   -ENOTCONN, 0, 0);
        return;
    }

    ble_publish_capture_status(CAPTURE_STATUS_RESEND_STARTED,
                               capture_attempt_id,
                               last_frame_distance_mm,
                               0, 0, 0);
    int err = ble_send_frame(camera_get_frame_buffer(), FRAME_BYTES,
                             last_frame_id, last_frame_timestamp_ms);
    if (err != 0) {
        last_transfer_failed = true;
        ble_publish_capture_status(CAPTURE_STATUS_TRANSFER_FAILED,
                                   capture_attempt_id,
                                   last_frame_distance_mm,
                                   (int16_t)err, 0, 0);
        LOG_ERR("Retained image resend failed: %d", err);
        return;
    }

    ble_publish_capture_status(CAPTURE_STATUS_AWAITING_APP_ACK,
                               capture_attempt_id,
                               last_frame_distance_mm,
                               0, 0, 0);
    LOG_INF("Retained frame %u resent; awaiting app ACK", last_frame_id);
}

static void handle_frame_ack(void)
{
    uint32_t frame_id = acknowledged_frame_id;
    if (!last_frame_valid || frame_id != last_frame_id) {
        LOG_WRN("Ignoring ACK for frame %u (latest retained frame is %u)",
                frame_id, last_frame_id);
        return;
    }

    last_transfer_failed = false;
    ble_publish_capture_status(CAPTURE_STATUS_TRANSFER_COMPLETE,
                               capture_attempt_id,
                               last_frame_distance_mm,
                               0, 0, 0);
    LOG_INF("App confirmed frame %u was saved", frame_id);
}

int mode_manager_init(void)
{
    k_work_queue_start(&camera_diagnostic_work_queue,
                       camera_diagnostic_work_queue_stack,
                       K_THREAD_STACK_SIZEOF(camera_diagnostic_work_queue_stack),
                       CAMERA_DIAGNOSTIC_WORK_QUEUE_PRIORITY, NULL);
    k_thread_name_set(&camera_diagnostic_work_queue.thread, "camera_diag");

    /* Match the known-good FingCam boot ordering: initialize the IMU side
     * before touching the camera.  ToF is also placed first so its 100 ms
     * power-up interval has elapsed before camera MCLK/register setup. */
    int err = imu_init(on_capture_gesture);
    if (err != 0) {
        return err;
    }
    err = imu_stream_init();
    if (err != 0) {
        return err;
    }
    err = tof_init();
    if (err != 0) {
        return err;
    }
    err = camera_init();
    if (err != 0) {
        /* Camera failure must not disable the independent Phase 3/4 IMU
         * stream.  Entering CAPTURE will retry camera_wake_up(), while
         * entering STREAM remains fully usable. */
        LOG_ERR("Camera unavailable at boot: %d; CAPTURE will retry later", err);
    }

    active_mode = FING_MODE_IDLE;
    requested_mode = FING_MODE_IDLE;
    return 0;
}

void mode_manager_request(enum fing_mode mode)
{
    if (mode > FING_MODE_STREAM) {
        return;
    }
    requested_mode = mode;
    k_event_post(&mode_events, EVENT_MODE_REQUEST);
}

void mode_manager_request_resend(void)
{
    k_event_post(&mode_events, EVENT_RESEND_REQUEST);
}

void mode_manager_acknowledge_frame(uint32_t frame_id)
{
    acknowledged_frame_id = frame_id;
    k_event_post(&mode_events, EVENT_FRAME_ACK);
}

void mode_manager_run(void)
{
    while (true) {
        uint32_t events = k_event_wait(&mode_events,
                                       EVENT_MODE_REQUEST | EVENT_CAPTURE_TRIGGER |
                                           EVENT_RESEND_REQUEST | EVENT_FRAME_ACK,
                                       true, K_FOREVER);

        /* Mode changes take priority, preventing a stale gesture from starting
         * capture after the app has requested the streaming profile. */
        if ((events & EVENT_MODE_REQUEST) != 0U) {
            transition_to(requested_mode);
        }
        if ((events & EVENT_CAPTURE_TRIGGER) != 0U) {
            handle_capture_trigger();
        }
        if ((events & EVENT_RESEND_REQUEST) != 0U) {
            handle_resend_request();
        }
        if ((events & EVENT_FRAME_ACK) != 0U) {
            handle_frame_ack();
        }
    }
}
