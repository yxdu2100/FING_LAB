#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app_config.h"
#include "ble/ble.h"
#include "modules/camera/camera.h"
#include "modules/imu/imu.h"
#include "modules/tof/tof.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

K_EVENT_DEFINE(application_events);

static void on_double_flick(void)
{
    k_event_post(&application_events, EVENT_IMU_DOUBLE_FLICK);
}

static bool distance_is_in_range(uint16_t distance_mm)
{
    return distance_mm >= TOF_MIN_DISTANCE_MM && distance_mm <= TOF_MAX_DISTANCE_MM;
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

int main(void)
{
    uint32_t next_frame_id = 0U;

    LOG_INF("Starting Fing Lab gesture capture firmware");

    camera_init();
    int err = tof_init();
    if (err != 0) {
        LOG_ERR("ToF initialization failed: %d", err);
        return err;
    }
    err = ble_init();
    if (err != 0) {
        LOG_ERR("BLE initialization failed: %d", err);
        return err;
    }
    err = imu_init(on_double_flick);
    if (err != 0) {
        LOG_ERR("IMU/MLC initialization failed: %d", err);
        return err;
    }

    uint8_t *frame = camera_get_frame_buffer();
    if (frame == NULL) {
        return -ENOMEM;
    }

    LOG_INF("Ready: double flick -> ToF [%u, %u] mm -> 324x244 capture -> BLE",
            TOF_MIN_DISTANCE_MM, TOF_MAX_DISTANCE_MM);

    while (true) {
        uint32_t events = k_event_wait(&application_events,
                                       EVENT_IMU_DOUBLE_FLICK,
                                       true, K_FOREVER);
        if ((events & EVENT_IMU_DOUBLE_FLICK) == 0U) {
            continue;
        }

        uint16_t distance_mm;
        err = tof_measure_mm(&distance_mm);
        if (err != 0) {
            LOG_ERR("ToF measurement failed: %d", err);
            continue;
        }
        if (!distance_is_in_range(distance_mm)) {
            LOG_INF("Double flick ignored: %u mm outside [%u, %u]",
                    distance_mm, TOF_MIN_DISTANCE_MM, TOF_MAX_DISTANCE_MM);
            continue;
        }

        int length = capture_full_frame();
        if (length != FRAME_BYTES) {
            LOG_ERR("Camera capture failed: %d", length);
            continue;
        }

        if (!ble_can_send_frame()) {
            LOG_WRN("Photo captured but app is not connected/subscribed; photo discarded");
            continue;
        }

        err = ble_send_frame(frame, FRAME_BYTES, next_frame_id++, k_uptime_get_32());
        if (err != 0) {
            LOG_ERR("BLE image transfer failed: %d", err);
        }
    }
}
