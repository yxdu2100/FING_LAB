#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
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

K_EVENT_DEFINE(mode_events);

static volatile enum fing_mode requested_mode = FING_MODE_IDLE;
static enum fing_mode active_mode = FING_MODE_IDLE;
static uint32_t next_frame_id;

static void on_double_flick(void)
{
    k_event_post(&mode_events, EVENT_CAPTURE_TRIGGER);
}

static bool distance_is_in_range(uint16_t distance_mm)
{
    return distance_mm >= TOF_MIN_DISTANCE_MM &&
           distance_mm <= TOF_MAX_DISTANCE_MM;
}

static int capture_full_frame(void)
{
    int result = -EAGAIN;

    camera_wake_up();
    for (int attempt = 1; attempt <= CAMERA_CAPTURE_MAX_ATTEMPTS; ++attempt) {
        result = camera_capture_frame();
        if (result == FRAME_BYTES) {
            break;
        }
        if (result != -EAGAIN && result != -ETIMEDOUT) {
            break;
        }
        LOG_WRN("Capture sync miss %d/%d (err=%d)",
                attempt, CAMERA_CAPTURE_MAX_ATTEMPTS, result);
        k_msleep(50);
    }
    camera_standby();
    return result;
}

static void enter_idle(void)
{
    /* The active profile is stopped before the BLE-visible mode changes.
     * That makes a mode acknowledgement mean its data path is fully quiet. */
    if (active_mode == FING_MODE_CAPTURE) {
        imu_stop_mlc();
        camera_standby();
    } else if (active_mode == FING_MODE_STREAM) {
        imu_stream_stop();
    }

    active_mode = FING_MODE_IDLE;
    ble_set_active_mode(active_mode);
    LOG_INF("Mode is IDLE");
}

static int enter_mode(enum fing_mode target)
{
    int err = 0;

    if (target == FING_MODE_IDLE) {
        return 0;
    }
    if (target == FING_MODE_CAPTURE) {
        err = imu_start_mlc();
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
    LOG_INF("Mode is %s", target == FING_MODE_CAPTURE ? "CAPTURE" : "STREAM");
    return 0;
}

static void transition_to(enum fing_mode target)
{
    if (target == active_mode) {
        ble_set_active_mode(active_mode);
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
        LOG_INF("Ignoring stale MLC trigger outside capture mode");
        return;
    }

    uint16_t distance_mm;
    int err = tof_measure_mm(&distance_mm);
    if (err != 0) {
        LOG_ERR("ToF measurement failed: %d", err);
        return;
    }
    if (!distance_is_in_range(distance_mm)) {
        LOG_INF("Double flick ignored: %u mm outside [%u, %u]",
                distance_mm, TOF_MIN_DISTANCE_MM, TOF_MAX_DISTANCE_MM);
        return;
    }

    int length = capture_full_frame();
    if (length != FRAME_BYTES) {
        LOG_ERR("Camera capture failed: %d", length);
        return;
    }
    if (!ble_can_send_frame()) {
        LOG_WRN("Photo captured but app is not subscribed; photo discarded");
        return;
    }

    uint8_t *frame = camera_get_frame_buffer();
    err = ble_send_frame(frame, FRAME_BYTES, next_frame_id++, k_uptime_get_32());
    if (err != 0) {
        LOG_ERR("BLE image transfer failed: %d", err);
    }
}

int mode_manager_init(void)
{
    camera_init();

    int err = tof_init();
    if (err != 0) {
        return err;
    }
    err = imu_init(on_double_flick);
    if (err != 0) {
        return err;
    }
    err = imu_stream_init();
    if (err != 0) {
        return err;
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

void mode_manager_run(void)
{
    while (true) {
        uint32_t events = k_event_wait(&mode_events,
                                       EVENT_MODE_REQUEST | EVENT_CAPTURE_TRIGGER,
                                       true, K_FOREVER);

        /* Mode changes take priority, preventing a stale gesture from starting
         * capture after the app has requested the streaming profile. */
        if ((events & EVENT_MODE_REQUEST) != 0U) {
            transition_to(requested_mode);
        }
        if ((events & EVENT_CAPTURE_TRIGGER) != 0U) {
            handle_capture_trigger();
        }
    }
}
