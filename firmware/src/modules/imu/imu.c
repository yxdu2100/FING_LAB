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
#define MLC_CODE_DOUBLE_FLICK 0x00U

#define MLC_POLL_INTERVAL_MS 1U
#define MLC_POLL_TIMEOUT_MS 100U

static const struct device *const i2c_bus = DEVICE_DT_GET(IMU_I2C_NODE);
static const struct device *const gpio1 = DEVICE_DT_GET(DT_NODELABEL(gpio1));
static struct gpio_callback int_callback;
static struct k_work int_work;
static imu_double_flick_callback_t double_flick_callback;

static int reg_write(uint8_t address, uint8_t value)
{
    uint8_t tx[] = {address, value};
    return i2c_write(i2c_bus, tx, sizeof(tx), IMU_ADDR);
}

static int reg_read(uint8_t address, uint8_t *value)
{
    return i2c_write_read(i2c_bus, IMU_ADDR, &address, sizeof(address), value, 1);
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

    if (mlc_result == MLC_CODE_DOUBLE_FLICK) {
        LOG_INF("Double flick detected (MLC1_SRC=0x00)");
        if (double_flick_callback != NULL) {
            double_flick_callback();
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
    (void)k_work_submit(&int_work);
}

int imu_init(imu_double_flick_callback_t callback)
{
    int err;

    if (!device_is_ready(i2c_bus) || !device_is_ready(gpio1)) {
        return -ENODEV;
    }

    double_flick_callback = callback;
    k_work_init(&int_work, interrupt_work_handler);

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

    err = load_mlc_configuration();
    if (err != 0) {
        return err;
    }

    err = gpio_pin_interrupt_configure(gpio1, IMU_INT_PIN, GPIO_INT_EDGE_TO_ACTIVE);
    if (err != 0) {
        return err;
    }

    LOG_INF("MLC ready: INT1 / MLC1_SRC 0x70 / double_flick 0x00");
    return 0;
}
