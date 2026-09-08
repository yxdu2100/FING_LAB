#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "tof.h"

LOG_MODULE_REGISTER(tof_module, LOG_LEVEL_INF);

#define TOF_NODE DT_NODELABEL(vl53l1cb)

static const struct device *const tof_sensor = DEVICE_DT_GET(TOF_NODE);
static const struct i2c_dt_spec tof_i2c = I2C_DT_SPEC_GET(TOF_NODE);

static int set_ranging(bool enabled)
{
    /* VL53L1X SYSTEM__MODE_START (16-bit register address 0x0087). */
    uint8_t command[] = {0x00U, 0x87U, enabled ? 0x40U : 0x00U};
    return i2c_write_dt(&tof_i2c, command, sizeof(command));
}

static int range_status_is_valid(bool *valid)
{
    static const uint8_t status_map[24] = {
        255, 255, 255,   5,   2,   4,   1,   7,
          3,   0, 255, 255,   9,  13, 255, 255,
        255, 255,  10,   6, 255, 255,  11,  12,
    };
    uint8_t reg[] = {0x00U, 0x89U};
    uint8_t raw;

    int err = i2c_write_read_dt(&tof_i2c, reg, sizeof(reg), &raw, sizeof(raw));
    if (err != 0) {
        return err;
    }

    raw &= 0x1FU;
    *valid = (raw < ARRAY_SIZE(status_map)) && (status_map[raw] == 0U);
    return 0;
}

int tof_init(void)
{
    if (!device_is_ready(tof_sensor) || !i2c_is_ready_dt(&tof_i2c)) {
        return -ENODEV;
    }

    // k_msleep(100);
    int err = set_ranging(false);
    if (err != 0) {
        LOG_ERR("ToF standby failed: %d", err);
        return err;
    }
    LOG_INF("ToF ready in software standby");
    /* 1 = short, 2 = medium, 3 = long. The 60-500 mm capture gate fits
     * entirely inside short mode, which holds ~135 cm of range in strong
     * ambient light where long mode collapses to ~73 cm. */
    struct sensor_value mode = { .val1 = 1, .val2 = 0 };
    err = sensor_attr_set(tof_sensor, SENSOR_CHAN_DISTANCE,
                            SENSOR_ATTR_CONFIGURATION, &mode);
    if (err != 0) {
        LOG_ERR("ToF distance mode set failed: %d", err);
        return err;
    }

    return 0;
}

int tof_measure_mm(uint16_t *distance_mm)
{
    struct sensor_value distance;
    int err;

    if (distance_mm == NULL) {
        return -EINVAL;
    }

    err = sensor_sample_fetch(tof_sensor);
    if (err == 0) {
        err = sensor_channel_get(tof_sensor, SENSOR_CHAN_DISTANCE, &distance);
    }

    int sleep_err = set_ranging(false);
    if (sleep_err != 0) {
        /* A failed power-down must not discard a good measurement. */
        LOG_WRN("ToF power-down failed: %d", sleep_err);
    }
    if (err != 0) {
        return err;
    }

    /* Ranging is stopped, so 0x0089 still holds the measurement read above. */
    bool measurement_valid = false;
    err = range_status_is_valid(&measurement_valid);
    if (err != 0) {
        return err;
    }
    if (!measurement_valid) {
        LOG_INF("ToF measurement rejected by range status");
        return -EBADMSG;
    }

    if (distance.val1 < 0 || distance.val1 > UINT16_MAX) {
        return -ERANGE;
    }

    *distance_mm = (uint16_t)distance.val1;
    LOG_INF("ToF distance: %u mm", *distance_mm);
    return 0;
}
