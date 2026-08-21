#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "tof.h"

LOG_MODULE_REGISTER(tof_module, LOG_LEVEL_INF);

#define TOF_NODE DT_NODELABEL(vl53l1cb)

static const struct device *const tof_sensor = DEVICE_DT_GET_ONE(st_vl53l1x);
static const struct i2c_dt_spec tof_i2c = I2C_DT_SPEC_GET(TOF_NODE);

static int set_ranging(bool enabled)
{
    /* VL53L1X SYSTEM__MODE_START (16-bit register address 0x0087). */
    uint8_t command[] = {0x00U, 0x87U, enabled ? 0x40U : 0x00U};
    return i2c_write_dt(&tof_i2c, command, sizeof(command));
}

int tof_init(void)
{
    if (!device_is_ready(tof_sensor) || !i2c_is_ready_dt(&tof_i2c)) {
        return -ENODEV;
    }

    k_msleep(100);
    int err = set_ranging(false);
    if (err == 0) {
        LOG_INF("ToF ready in software standby");
    }
    return err;
}

int tof_measure_mm(uint16_t *distance_mm)
{
    struct sensor_value distance;
    int err;

    if (distance_mm == NULL) {
        return -EINVAL;
    }

    err = set_ranging(true);
    if (err != 0) {
        return err;
    }
    k_msleep(40);

    err = sensor_sample_fetch(tof_sensor);
    if (err == 0) {
        err = sensor_channel_get(tof_sensor, SENSOR_CHAN_DISTANCE, &distance);
    }

    int sleep_err = set_ranging(false);
    if (err != 0) {
        return err;
    }
    if (sleep_err != 0) {
        return sleep_err;
    }

    if (distance.val1 < 0 || distance.val1 > UINT16_MAX) {
        return -ERANGE;
    }

    *distance_mm = (uint16_t)distance.val1;
    LOG_INF("ToF distance: %u mm", *distance_mm);
    return 0;
}
