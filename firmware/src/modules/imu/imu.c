#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "app_config.h"
#include "imu.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-const-variable"
#include "mlc.h"
#pragma GCC diagnostic pop

LOG_MODULE_REGISTER(imu_module, LOG_LEVEL_INF);

#define IMU_ADDR 0x6AU
#define IMU_I2C_NODE DT_NODELABEL(i2c21)
#define IMU_INT_PIN 5U

#define REG_FUNC_CFG_ACCESS 0x01U
#define REG_ALL_INT_SRC 0x1DU
/* LSM6DSV16X main-bank MLC status register. 0x38 is an OIS data register. */
#define REG_MLC_STATUS_MAINPAGE 0x4BU
#define REG_MLC1_SRC 0x70U

#define FUNC_CFG_EMBEDDED_BANK BIT(7)
#define ALL_INT_SRC_EMB_FUNC_IA BIT(7)
#define MLC_STATUS_MLC1 BIT(0)
#define MLC_POLL_INTERVAL_MS 1U
#define MLC_POLL_TIMEOUT_MS 100U

#define REG_CTRL1_XL 0x10U
#define REG_CTRL2_G 0x11U
#define REG_CTRL3_C 0x12U
#define REG_CTRL8_XL 0x17U
#define REG_OUTY_L_A 0x2AU
#define CTRL3_BDU_IF_INC (BIT(6) | BIT(2))
#define CTRL8_XL_FS_MASK 0x03U
#define CTRL8_XL_FS_8G 0x02U
#define CTRL1_XL_ODR_120HZ 0x06U

static const struct device *const i2c_bus = DEVICE_DT_GET(IMU_I2C_NODE);
static const struct device *const gpio1 = DEVICE_DT_GET(DT_NODELABEL(gpio1));
static struct gpio_callback int_callback;
static struct k_work int_work;
static struct k_work_delayable hover_work;
static imu_gesture_callback_t gesture_callback;
static volatile bool mlc_enabled;
static volatile bool hover_enabled;
static int16_t hover_y_window[CAPTURE_HOVER_WINDOW_SAMPLES];
static uint16_t hover_window_count;
static uint16_t hover_window_index;
static bool hover_armed;
static uint32_t hover_last_trigger_ms;
static uint32_t hover_read_error_count;
static uint32_t hover_window_eval_count;

static int reg_write(uint8_t address, uint8_t value)
{
    uint8_t tx[] = {address, value};
    return i2c_write(i2c_bus, tx, sizeof(tx), IMU_ADDR);
}

static int reg_read(uint8_t address, uint8_t *value)
{
    return i2c_write_read(i2c_bus, IMU_ADDR, &address, sizeof(address), value, 1);
}

static int reg_read_buf(uint8_t address, uint8_t *buffer, size_t length)
{
    return i2c_write_read(i2c_bus, IMU_ADDR, &address, sizeof(address),
                          buffer, length);
}

static int reg_update(uint8_t address, uint8_t mask, uint8_t value)
{
    uint8_t current;
    int err = reg_read(address, &current);

    if (err != 0) {
        return err;
    }
    return reg_write(address, (current & ~mask) | (value & mask));
}

static void hover_reset_window(void)
{
    hover_window_count = 0U;
    hover_window_index = 0U;
    hover_armed = false;
}

static bool hover_window_matches(int16_t *minimum_y, int16_t *maximum_y)
{
    int16_t minimum = INT16_MAX;
    int16_t maximum = INT16_MIN;

    if (hover_window_count < CAPTURE_HOVER_WINDOW_SAMPLES) {
        return false;
    }

    for (uint16_t i = 0U; i < CAPTURE_HOVER_WINDOW_SAMPLES; ++i) {
        minimum = MIN(minimum, hover_y_window[i]);
        maximum = MAX(maximum, hover_y_window[i]);
    }
    *minimum_y = minimum;
    *maximum_y = maximum;

    return maximum <= CAPTURE_HOVER_MAX_Y_RAW &&
           minimum > CAPTURE_HOVER_MIN_Y_RAW;
}

static void hover_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    if (!hover_enabled) {
        return;
    }

    uint8_t bytes[2];
    int err = reg_read_buf(REG_OUTY_L_A, bytes, sizeof(bytes));
    if (err != 0) {
        hover_read_error_count++;
        if ((hover_read_error_count % CAPTURE_HOVER_SAMPLE_RATE_HZ) == 1U) {
            LOG_WRN("Hover accel-Y read failed (count=%u err=%d)",
                    hover_read_error_count, err);
        }
        goto reschedule;
    }

    int16_t accel_y = (int16_t)(((uint16_t)bytes[1] << 8) | bytes[0]);
    hover_y_window[hover_window_index] = accel_y;
    hover_window_index =
        (hover_window_index + 1U) % CAPTURE_HOVER_WINDOW_SAMPLES;
    if (hover_window_count < CAPTURE_HOVER_WINDOW_SAMPLES) {
        hover_window_count++;
    }

    int16_t minimum_y;
    int16_t maximum_y;
    bool matches = hover_window_matches(&minimum_y, &maximum_y);
    if (hover_window_count == CAPTURE_HOVER_WINDOW_SAMPLES) {
        hover_window_eval_count++;
        if ((hover_window_eval_count % CAPTURE_HOVER_SAMPLE_RATE_HZ) == 1U) {
            LOG_INF("Hover window: minY=%d maxY=%d raw match=%u armed=%u",
                    minimum_y, maximum_y, matches, hover_armed);
        }
    }
    if (!matches && hover_window_count == CAPTURE_HOVER_WINDOW_SAMPLES) {
        /* Require a full non-hover window before accepting hover. This is the
         * software equivalent of edge triggering and prevents a stationary
         * orientation inside the threshold band from firing at mode entry. */
        hover_armed = true;
    } else if (matches && hover_armed) {
        uint32_t now = k_uptime_get_32();
        if ((uint32_t)(now - hover_last_trigger_ms) >=
            CAPTURE_HOVER_REARM_COOLDOWN_MS) {
            hover_last_trigger_ms = now;
            hover_armed = false;
            /* Pause sampling during ToF/camera/BLE activity. The mode manager
             * rearms the detector after the complete capture attempt. */
            hover_enabled = false;
            LOG_INF("Threshold hover detected: minY=%d maxY=%d raw",
                    minimum_y, maximum_y);
            if (gesture_callback != NULL) {
                gesture_callback();
            }
            return;
        }
    }

reschedule:
    if (hover_enabled) {
        (void)k_work_reschedule(&hover_work,
                                K_USEC(CAPTURE_HOVER_SAMPLE_PERIOD_US));
    }
}

static int select_embedded_bank(bool embedded)
{
    return reg_write(REG_FUNC_CFG_ACCESS, embedded ? FUNC_CFG_EMBEDDED_BANK : 0U);
}

static int wait_for_register(uint8_t address, uint8_t mask, bool set)
{
    for (uint32_t waited = 0U; waited < MLC_POLL_TIMEOUT_MS;
         waited += MLC_POLL_INTERVAL_MS) {
        uint8_t value;
        int err = reg_read(address, &value);

        if (err != 0) {
            return err;
        }

        bool matches = set ? ((value & mask) == mask) : ((value & mask) == 0U);
        if (matches) {
            return 0;
        }
        k_msleep(MLC_POLL_INTERVAL_MS);
    }

    return -ETIMEDOUT;
}

static int apply_mlc_operation(const struct mems_conf_op *operation)
{
    switch (operation->type) {
    case MEMS_CONF_OP_TYPE_READ: {
        uint8_t ignored;
        return reg_read(operation->address, &ignored);
    }
    case MEMS_CONF_OP_TYPE_WRITE:
        return reg_write(operation->address, operation->data);
    case MEMS_CONF_OP_TYPE_DELAY:
        k_msleep(operation->data);
        return 0;
    case MEMS_CONF_OP_TYPE_POLL_SET:
        return wait_for_register(operation->address, operation->data, true);
    case MEMS_CONF_OP_TYPE_POLL_RESET:
        return wait_for_register(operation->address, operation->data, false);
    default:
        return -ENOTSUP;
    }
}

static int load_mlc_configuration(void)
{
    const struct mems_conf_op_list *configuration = &mlc_confs[0];

    for (uint32_t i = 0; i < configuration->len; ++i) {
        int err = apply_mlc_operation(&configuration->list[i]);
        if (err != 0) {
            LOG_ERR("MLC op %u failed (type=%u reg=0x%02x err=%d)",
                    i, configuration->list[i].type,
                    configuration->list[i].address, err);
            (void)select_embedded_bank(false);
            return err;
        }
    }

    /* MEMS Studio operations can switch banks. Always leave the main bank selected. */
    return select_embedded_bank(false);
}

static int read_mlc1_result(uint8_t *result)
{
    int err = select_embedded_bank(true);
    if (err != 0) {
        return err;
    }

    err = reg_read(REG_MLC1_SRC, result);
    int restore_err = select_embedded_bank(false);
    return err != 0 ? err : restore_err;
}

static void interrupt_work_handler(struct k_work *work)
{
    uint8_t all_interrupts = 0U;
    uint8_t mlc_status = 0U;
    uint8_t mlc_result = 0xFFU;

    ARG_UNUSED(work);

    if (!mlc_enabled) {
        return;
    }

    int err = reg_read(REG_ALL_INT_SRC, &all_interrupts);
    if (err != 0) {
        LOG_ERR("Cannot read ALL_INT_SRC: %d", err);
        return;
    }

    err = reg_read(REG_MLC_STATUS_MAINPAGE, &mlc_status);
    if (err != 0) {
        LOG_ERR("Cannot read MLC_STATUS_MAINPAGE: %d", err);
        return;
    }

    /* Keep this at INFO while bringing up the gesture path. It proves that
     * the GPIO interrupt reached the MCU and shows which IMU source asserted. */
    LOG_INF("INT1 received: ALL_INT_SRC=0x%02x MLC_STATUS=0x%02x",
            all_interrupts, mlc_status);

    /* 0x00 is a valid class, so never interpret MLC1_SRC unless an MLC1
     * interrupt is pending. This avoids treating an idle zero as a flick. */
    if ((all_interrupts & ALL_INT_SRC_EMB_FUNC_IA) == 0U ||
        (mlc_status & MLC_STATUS_MLC1) == 0U) {
        LOG_INF("Ignoring non-MLC1 interrupt (all=0x%02x mlc=0x%02x)",
                all_interrupts, mlc_status);
        return;
    }

    err = read_mlc1_result(&mlc_result);
    if (err != 0) {
        LOG_ERR("Cannot read MLC1_SRC (0x70): %d", err);
        return;
    }

    if (mlc_enabled && mlc_result == CAPTURE_MLC_TARGET_CLASS) {
        LOG_INF("Configured gesture detected (MLC1_SRC=0x%02x)", mlc_result);
        if (gesture_callback != NULL) {
            gesture_callback();
        }
    } else {
        LOG_INF("MLC class 0x%02x ignored", mlc_result);
    }
}

static void interrupt_handler(const struct device *device,
                              struct gpio_callback *callback,
                              uint32_t pins)
{
    ARG_UNUSED(device);
    ARG_UNUSED(callback);
    ARG_UNUSED(pins);

    /* Keep I2C and all capture work outside interrupt context. */
    if (mlc_enabled) {
        (void)k_work_submit(&int_work);
    }
}

int imu_init(imu_gesture_callback_t callback)
{
    int err;

    if (!device_is_ready(i2c_bus) || !device_is_ready(gpio1)) {
        return -ENODEV;
    }

    gesture_callback = callback;
    k_work_init(&int_work, interrupt_work_handler);
    k_work_init_delayable(&hover_work, hover_work_handler);

    err = gpio_pin_configure(gpio1, IMU_INT_PIN, GPIO_INPUT);
    if (err != 0) {
        return err;
    }
    err = gpio_pin_interrupt_configure(gpio1, IMU_INT_PIN, GPIO_INT_DISABLE);
    if (err != 0) {
        return err;
    }
    gpio_init_callback(&int_callback, interrupt_handler, BIT(IMU_INT_PIN));
    err = gpio_add_callback(gpio1, &int_callback);
    if (err != 0) {
        return err;
    }

    mlc_enabled = false;
    hover_enabled = false;
    LOG_INF("MLC interrupt endpoint initialized on GPIO1.%u", IMU_INT_PIN);
    return 0;
}

int imu_start_mlc(void)
{
    int err = gpio_pin_interrupt_configure(gpio1, IMU_INT_PIN, GPIO_INT_DISABLE);

    if (err != 0) {
        return err;
    }
    mlc_enabled = false;

    err = load_mlc_configuration();
    if (err != 0) {
        return err;
    }

    mlc_enabled = true;
    err = gpio_pin_interrupt_configure(gpio1, IMU_INT_PIN, GPIO_INT_EDGE_TO_ACTIVE);
    if (err != 0) {
        mlc_enabled = false;
        return err;
    }

    LOG_INF("MLC ready: INT1 / MLC1_SRC 0x70 / target class 0x%02x",
            CAPTURE_MLC_TARGET_CLASS);
    return 0;
}

void imu_stop_mlc(void)
{
    struct k_work_sync sync;

    mlc_enabled = false;
    (void)gpio_pin_interrupt_configure(gpio1, IMU_INT_PIN, GPIO_INT_DISABLE);
    (void)k_work_cancel_sync(&int_work, &sync);

    /* Clear both embedded-function routing layers, then power the IMU down.
     * The streaming profile will perform a complete register rewrite later. */
    (void)select_embedded_bank(true);
    (void)reg_write(0x0DU, 0x00U); /* MLC_INT1 */
    (void)reg_write(0x05U, 0x00U); /* EMB_FUNC_EN_B / MLC_EN */
    (void)select_embedded_bank(false);
    (void)reg_write(0x5EU, 0x00U); /* MD1_CFG */
    (void)reg_write(0x10U, 0x00U); /* CTRL1: accel power-down */
    (void)reg_write(0x11U, 0x00U); /* CTRL2: gyro power-down */
    LOG_INF("MLC stopped");
}

static int __maybe_unused imu_start_threshold_hover(void)
{
    struct k_work_sync sync;
    int err;

    hover_enabled = false;
    (void)k_work_cancel_delayable_sync(&hover_work, &sync);
    mlc_enabled = false;
    (void)gpio_pin_interrupt_configure(gpio1, IMU_INT_PIN,
                                       GPIO_INT_DISABLE);

    /* Remove MLC routing without deleting or modifying its configuration
     * code. Selecting the MLC source later performs a complete table load. */
    err = select_embedded_bank(true);
    if (err == 0) {
        err = reg_write(0x0DU, 0x00U); /* MLC_INT1 */
    }
    if (err == 0) {
        err = reg_write(0x05U, 0x00U); /* EMB_FUNC_EN_B / MLC_EN */
    }
    int restore_err = select_embedded_bank(false);
    if (err == 0) {
        err = restore_err;
    }
    if (err != 0) {
        return err;
    }

    err = reg_update(REG_CTRL3_C, CTRL3_BDU_IF_INC, CTRL3_BDU_IF_INC);
    if (err == 0) {
        err = reg_update(REG_CTRL8_XL, CTRL8_XL_FS_MASK,
                         CTRL8_XL_FS_8G);
    }
    if (err == 0) {
        err = reg_write(REG_CTRL2_G, 0x00U);
    }
    if (err == 0) {
        err = reg_write(REG_CTRL1_XL, CTRL1_XL_ODR_120HZ);
    }
    if (err != 0) {
        (void)reg_write(REG_CTRL1_XL, 0x00U);
        return err;
    }

    k_msleep(20);
    hover_reset_window();
    hover_read_error_count = 0U;
    hover_window_eval_count = 0U;
    /* Permit the first real transition to trigger without integer wrap
     * accidentally imposing a cooldown after boot. */
    hover_last_trigger_ms = k_uptime_get_32() -
                            CAPTURE_HOVER_REARM_COOLDOWN_MS;
    hover_enabled = true;
    (void)k_work_reschedule(&hover_work, K_NO_WAIT);
    LOG_INF("Threshold hover ready: 120 Hz, %u samples, raw Y (%d, %d]",
            CAPTURE_HOVER_WINDOW_SAMPLES,
            CAPTURE_HOVER_MIN_Y_RAW, CAPTURE_HOVER_MAX_Y_RAW);
    return 0;
}

static void __maybe_unused imu_stop_threshold_hover(void)
{
    struct k_work_sync sync;

    hover_enabled = false;
    (void)k_work_cancel_delayable_sync(&hover_work, &sync);
    (void)reg_write(REG_CTRL1_XL, 0x00U);
    (void)reg_write(REG_CTRL2_G, 0x00U);
    hover_reset_window();
    LOG_INF("Threshold hover stopped");
}

int imu_start_capture_detector(void)
{
#if CAPTURE_GESTURE_SOURCE == CAPTURE_GESTURE_SOURCE_THRESHOLD
    return imu_start_threshold_hover();
#else
    return imu_start_mlc();
#endif
}

void imu_rearm_capture_detector(void)
{
#if CAPTURE_GESTURE_SOURCE == CAPTURE_GESTURE_SOURCE_THRESHOLD
    if (hover_enabled) {
        return;
    }
    hover_reset_window();
    hover_enabled = true;
    (void)k_work_reschedule(&hover_work, K_NO_WAIT);
    LOG_INF("Threshold hover rearmed");
#endif
}

void imu_stop_capture_detector(void)
{
#if CAPTURE_GESTURE_SOURCE == CAPTURE_GESTURE_SOURCE_THRESHOLD
    imu_stop_threshold_hover();
#else
    imu_stop_mlc();
#endif
}
