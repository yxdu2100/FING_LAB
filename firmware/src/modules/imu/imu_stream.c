#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/util.h>
#include <string.h>
#include "imu_stream.h"
#include "../../ble/ble.h"

LOG_MODULE_REGISTER(imu_stream_module, LOG_LEVEL_INF);

#define IMU_ADDR 0x6A
#define I2C_NODE DT_NODELABEL(i2c21)
#define IMU_INT_PIN 5

/* IMU sample rate. Set to 60 or 120 at build time only; this is not
 * switchable at runtime. ODR/BDR register codes are derived below. */
#define IMU_STREAM_HZ 120
#define IMU_THREAD_STACK_SIZE 3072
#define IMU_THREAD_PRIORITY 7
#define IMU_DRDY_QUEUE_LEN 8
#define IMU_FIFO_WATERMARK_ROWS 20
#define IMU_FIFO_ROW_SIZE 7
#define IMU_SAMPLE_PERIOD_US (1000000U / IMU_STREAM_HZ)
#define IMU_FIFO_TAG_GYRO 0x01
#define IMU_FIFO_TAG_ACCEL 0x02
#define IMU_FIFO_TAG_TIMESTAMP 0x04
#define IMU_FIFO_TAG_SLOT_COUNT 4
#define IMU_POLL_FALLBACK_MS 200
#define IMU_HEARTBEAT_INTERVAL_MS 2000
#define IMU_FIFO_MAX_ROWS 0x01FF
#define IMU_FIFO_MAX_PAIRS (IMU_FIFO_MAX_ROWS / 2)

#define LSM6DSV16X_REG_FIFO_CTRL1 0x07
#define LSM6DSV16X_REG_FIFO_CTRL2 0x08
#define LSM6DSV16X_REG_FIFO_CTRL3 0x09
#define LSM6DSV16X_REG_FIFO_CTRL4 0x0A
#define LSM6DSV16X_REG_INT1_CTRL 0x0D
#define LSM6DSV16X_REG_CTRL1     0x10
#define LSM6DSV16X_REG_CTRL2     0x11
#define LSM6DSV16X_REG_CTRL3     0x12
#define LSM6DSV16X_REG_CTRL4     0x13
#define LSM6DSV16X_REG_CTRL6     0x15
#define LSM6DSV16X_REG_CTRL8     0x17
#define LSM6DSV16X_REG_FIFO_STATUS1 0x1B
#define LSM6DSV16X_REG_FIFO_STATUS2 0x1C
#define LSM6DSV16X_REG_TIMESTAMP0 0x40
#define LSM6DSV16X_REG_FUNCTIONS_ENABLE 0x50
#define LSM6DSV16X_REG_FIFO_DATA_OUT_TAG 0x78

#if IMU_STREAM_HZ == 60
#define LSM6DSV16X_ODR_CODE      0x05
#define LSM6DSV16X_FIFO_BDR_CODE 0x05
#elif IMU_STREAM_HZ == 120
#define LSM6DSV16X_ODR_CODE      0x06
#define LSM6DSV16X_FIFO_BDR_CODE 0x06
#else
#error "IMU_STREAM_HZ must be 60 or 120"
#endif
#define LSM6DSV16X_GYRO_2000_DPS 0x04
#define LSM6DSV16X_ACCEL_8G      0x02
#define LSM6DSV16X_INT1_FIFO_TH  BIT(3)
#define LSM6DSV16X_FUNCTIONS_ENABLE_TIMESTAMP_EN BIT(6)
#define LSM6DSV16X_CTRL3_IF_INC  BIT(2)
#define LSM6DSV16X_CTRL3_BDU     BIT(6)
#define LSM6DSV16X_CTRL4_DRDY_PULSED BIT(1)
#define LSM6DSV16X_FIFO_CTRL2_WTM_8 BIT(0)
#define LSM6DSV16X_FIFO_CTRL4_DEC_TS_BATCH_MASK GENMASK(7, 6)
#define LSM6DSV16X_FIFO_CTRL4_DEC_TS_BATCH_1    BIT(6)
#define LSM6DSV16X_FIFO_MODE_MASK GENMASK(2, 0)
#define LSM6DSV16X_FIFO_MODE_BYPASS 0x00
#define LSM6DSV16X_FIFO_MODE_CONTINUOUS 0x06
#define LSM6DSV16X_FIFO_DIFF_FIFO_8 BIT(0)
#define LSM6DSV16X_FIFO_STATUS2_OVR_IA BIT(6)

struct imu_fifo_pair {
    int16_t accel_raw[3];
    int16_t gyro_raw[3];
    uint32_t timestamp_us;
    uint32_t timestamp_ticks;
    uint8_t timestamp_flags;
};

struct imu_fifo_slot {
    int16_t accel_raw[3];
    int16_t gyro_raw[3];
    uint32_t timestamp_ticks;
    bool accel_valid;
    bool gyro_valid;
    bool timestamp_valid;
};

static const struct device *gpio1_dev = DEVICE_DT_GET(DT_NODELABEL(gpio1));
static struct gpio_callback imu_cb_data;
static struct k_thread imu_stream_thread;
static K_THREAD_STACK_DEFINE(imu_stream_stack, IMU_THREAD_STACK_SIZE);
K_MSGQ_DEFINE(imu_drdy_msgq, sizeof(uint32_t), IMU_DRDY_QUEUE_LEN, 4);
static struct imu_fifo_pair imu_pair_buffer[IMU_FIFO_MAX_PAIRS];

static volatile bool imu_stream_enabled;
static uint16_t imu_sample_id;
static struct imu_fifo_slot imu_fifo_slots[IMU_FIFO_TAG_SLOT_COUNT];
static bool imu_timestamp_base_valid;
static uint32_t imu_timestamp_base_ticks;
static bool imu_last_timestamp_valid;
static uint32_t imu_last_timestamp_us;
static uint32_t imu_timestamp_fallback_count;
static uint8_t imu_current_batch_flags;
static uint32_t imu_samples_sent_total;
static int64_t imu_heartbeat_last_ms;

const struct device *imu_sensor = DEVICE_DT_GET_ONE(st_lsm6dsv16x);
const struct device *i2c_bus = DEVICE_DT_GET(I2C_NODE);

int imu_reg_write(uint8_t reg, uint8_t val)
{
    uint8_t tx[2] = { reg, val };

    return i2c_write(i2c_bus, tx, sizeof(tx), IMU_ADDR);
}

int imu_reg_read(uint8_t reg, uint8_t *val)
{
    return i2c_write_read(i2c_bus, IMU_ADDR, &reg, 1, val, 1);
}

int imu_reg_read_buf(uint8_t start_reg, uint8_t *buf, size_t len)
{
    return i2c_write_read(i2c_bus, IMU_ADDR, &start_reg, 1, buf, len);
}

static uint32_t imu_timestamp_now_us(void)
{
    return k_cyc_to_us_floor32(k_cycle_get_32());
}

int imu_reg_update(uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t reg_val = 0;
    int ret = imu_reg_read(reg, &reg_val);

    if (ret != 0) {
        return ret;
    }

    reg_val = (reg_val & ~mask) | (value & mask);
    return imu_reg_write(reg, reg_val);
}

static int imu_set_fifo_mode(uint8_t mode)
{
    return imu_reg_update(LSM6DSV16X_REG_FIFO_CTRL4,
                          LSM6DSV16X_FIFO_MODE_MASK,
                          mode);
}

static int imu_read_fifo_status(uint16_t *fifo_rows, bool *fifo_overrun)
{
    uint8_t status[2];
    int ret = imu_reg_read_buf(LSM6DSV16X_REG_FIFO_STATUS1, status, sizeof(status));

    if (ret != 0) {
        LOG_ERR("Failed to read FIFO status: %d", ret);
        return ret;
    }

    *fifo_rows = (uint16_t)status[0] |
                 (((uint16_t)(status[1] & LSM6DSV16X_FIFO_DIFF_FIFO_8)) << 8);
    *fifo_overrun = (status[1] & LSM6DSV16X_FIFO_STATUS2_OVR_IA) != 0U;

    return 0;
}

static int imu_flush_fifo(void)
{
    int ret = imu_set_fifo_mode(LSM6DSV16X_FIFO_MODE_BYPASS);

    if (ret != 0) {
        LOG_ERR("Failed to enter FIFO bypass mode: %d", ret);
        return ret;
    }

    ret = imu_set_fifo_mode(LSM6DSV16X_FIFO_MODE_CONTINUOUS);
    if (ret != 0) {
        LOG_ERR("Failed to restore FIFO continuous mode: %d", ret);
        return ret;
    }

    return 0;
}

static int imu_read_fifo_row(uint8_t fifo_row[IMU_FIFO_ROW_SIZE])
{
    return imu_reg_read_buf(LSM6DSV16X_REG_FIFO_DATA_OUT_TAG, fifo_row, IMU_FIFO_ROW_SIZE);
}

static void imu_parse_fifo_axes(const uint8_t fifo_row[IMU_FIFO_ROW_SIZE], int16_t axes[3])
{
    for (int i = 0; i < 3; i++) {
        axes[i] = (int16_t)((fifo_row[(i * 2) + 2] << 8) | fifo_row[(i * 2) + 1]);
    }
}

static uint32_t imu_parse_fifo_timestamp_ticks(const uint8_t fifo_row[IMU_FIFO_ROW_SIZE])
{
    return (uint32_t)fifo_row[1] |
           ((uint32_t)fifo_row[2] << 8) |
           ((uint32_t)fifo_row[3] << 16) |
           ((uint32_t)fifo_row[4] << 24);
}

static int imu_read_timestamp_ticks(uint32_t *timestamp_ticks)
{
    uint8_t timestamp[4];
    int ret = imu_reg_read_buf(LSM6DSV16X_REG_TIMESTAMP0, timestamp, sizeof(timestamp));

    if (ret != 0) {
        return ret;
    }

    *timestamp_ticks = (uint32_t)timestamp[0] |
                       ((uint32_t)timestamp[1] << 8) |
                       ((uint32_t)timestamp[2] << 16) |
                       ((uint32_t)timestamp[3] << 24);

    return 0;
}

static uint32_t imu_timestamp_ticks_to_elapsed_us(uint32_t timestamp_ticks)
{
    if (!imu_timestamp_base_valid) {
        imu_timestamp_base_ticks = timestamp_ticks;
        imu_timestamp_base_valid = true;
    }

    uint32_t elapsed_ticks = timestamp_ticks - imu_timestamp_base_ticks;

    /* LSM6DSV16X timestamp resolution is 21.75 us/LSB typical. */
    return (uint32_t)(((uint64_t)elapsed_ticks * 87U) / 4U);
}

static uint32_t imu_next_interpolated_timestamp_us(void)
{
    uint32_t timestamp_us = imu_last_timestamp_valid
        ? imu_last_timestamp_us + IMU_SAMPLE_PERIOD_US
        : 0U;

    imu_last_timestamp_us = timestamp_us;
    imu_last_timestamp_valid = true;

    return timestamp_us;
}

static void imu_reset_fifo_parser_state(void)
{
    memset(imu_fifo_slots, 0, sizeof(imu_fifo_slots));
    imu_timestamp_base_valid = false;
    imu_last_timestamp_valid = false;
    imu_last_timestamp_us = 0;
    imu_timestamp_fallback_count = 0;
    imu_current_batch_flags = 0;
}

static void imu_note_fallback_timestamp(void)
{
    imu_timestamp_fallback_count++;
    if ((imu_timestamp_fallback_count % 120U) == 1U) {
        LOG_WRN("IMU timestamp fallback/interpolation used (count %u)",
                imu_timestamp_fallback_count);
    }
}

static void imu_fill_pair_from_slot(struct imu_fifo_slot *slot,
                                    struct imu_fifo_pair *pair,
                                    uint8_t extra_flags)
{
    for (int axis = 0; axis < 3; axis++) {
        pair->accel_raw[axis] = slot->accel_raw[axis];
        pair->gyro_raw[axis] = slot->gyro_raw[axis];
    }

    pair->timestamp_flags = extra_flags;
    if (slot->timestamp_valid) {
        pair->timestamp_ticks = slot->timestamp_ticks;
        pair->timestamp_us = imu_timestamp_ticks_to_elapsed_us(slot->timestamp_ticks);
        pair->timestamp_flags |= BT_APP_IMU_TS_FLAG_HARDWARE;

        if (imu_last_timestamp_valid && pair->timestamp_us <= imu_last_timestamp_us) {
            pair->timestamp_flags |= BT_APP_IMU_TS_FLAG_NONMONOTONIC;
        }

        imu_last_timestamp_us = pair->timestamp_us;
        imu_last_timestamp_valid = true;
    } else {
        pair->timestamp_ticks = 0;
        pair->timestamp_us = imu_next_interpolated_timestamp_us();
        pair->timestamp_flags |= BT_APP_IMU_TS_FLAG_INTERPOLATED |
                                 BT_APP_IMU_TS_FLAG_FALLBACK;
        imu_note_fallback_timestamp();
    }
}

static void imu_emit_slot(uint8_t slot_index, size_t *pair_count, bool force_interpolated)
{
    struct imu_fifo_slot *slot = &imu_fifo_slots[slot_index];

    if (!slot->accel_valid || !slot->gyro_valid) {
        return;
    }

    if (*pair_count >= IMU_FIFO_MAX_PAIRS) {
        LOG_WRN("FIFO pair buffer full, dropping oldest batch data");
        memset(slot, 0, sizeof(*slot));
        return;
    }

    if (!slot->timestamp_valid && !force_interpolated) {
        return;
    }

    imu_fill_pair_from_slot(slot,
                            &imu_pair_buffer[*pair_count],
                            imu_current_batch_flags);
    (*pair_count)++;

    memset(slot, 0, sizeof(*slot));
}

static void imu_emit_sample_batch(const struct bt_app_imu_sample *batch, size_t batch_count)
{
    ble_send_imu_samples(batch, batch_count);
}

static void imu_send_fifo_pairs(size_t pair_count)
{
    struct bt_app_imu_sample batch[BT_APP_IMU_SAMPLES_PER_PACKET];
    size_t batch_count = 0;

    for (size_t k = 0; k < pair_count; k++) {
        batch[batch_count++] = (struct bt_app_imu_sample) {
            .sample_id = imu_sample_id++,
            .timestamp_us = imu_pair_buffer[k].timestamp_us,
            .timestamp_ticks = imu_pair_buffer[k].timestamp_ticks,
            .timestamp_flags = imu_pair_buffer[k].timestamp_flags,
            .accel_raw = {
                imu_pair_buffer[k].accel_raw[0],
                imu_pair_buffer[k].accel_raw[1],
                imu_pair_buffer[k].accel_raw[2],
            },
            .gyro_raw = {
                imu_pair_buffer[k].gyro_raw[0],
                imu_pair_buffer[k].gyro_raw[1],
                imu_pair_buffer[k].gyro_raw[2],
            },
        };

        if (batch_count == BT_APP_IMU_SAMPLES_PER_PACKET) {
            imu_emit_sample_batch(batch, batch_count);
            batch_count = 0;
        }
    }

    if (batch_count > 0U) {
        imu_emit_sample_batch(batch, batch_count);
    }

    k_msleep(1);
}

static int imu_configure_streaming_profile(void)
{
    int ret;

    /* Fully remove the capture/MLC profile before writing FIFO settings. */
    ret = imu_reg_write(0x01, 0x80); /* embedded bank */
    if (ret != 0) { return ret; }
    ret = imu_reg_write(0x0D, 0x00); /* MLC_INT1 */
    if (ret != 0) { return ret; }
    ret = imu_reg_write(0x05, 0x00); /* EMB_FUNC_EN_B */
    if (ret != 0) { return ret; }
    ret = imu_reg_write(0x01, 0x00); /* main bank */
    if (ret != 0) { return ret; }
    ret = imu_reg_write(0x5E, 0x00); /* MD1_CFG */
    if (ret != 0) { return ret; }
    ret = imu_reg_write(LSM6DSV16X_REG_INT1_CTRL, 0x00);
    if (ret != 0) { return ret; }

    ret = imu_reg_write(LSM6DSV16X_REG_CTRL1, LSM6DSV16X_ODR_CODE);
    if (ret != 0) {
        LOG_ERR("Failed to set accel ODR: %d", ret);
        return ret;
    }

    ret = imu_reg_write(LSM6DSV16X_REG_CTRL2, LSM6DSV16X_ODR_CODE);
    if (ret != 0) {
        LOG_ERR("Failed to set gyro ODR: %d", ret);
        return ret;
    }

    ret = imu_reg_update(LSM6DSV16X_REG_CTRL3,
                         LSM6DSV16X_CTRL3_IF_INC | LSM6DSV16X_CTRL3_BDU,
                         LSM6DSV16X_CTRL3_IF_INC | LSM6DSV16X_CTRL3_BDU);
    if (ret != 0) {
        LOG_ERR("Failed to configure CTRL3: %d", ret);
        return ret;
    }

    ret = imu_reg_update(LSM6DSV16X_REG_CTRL4,
                         LSM6DSV16X_CTRL4_DRDY_PULSED,
                         0);
    if (ret != 0) {
        LOG_ERR("Failed to configure CTRL4: %d", ret);
        return ret;
    }

    /* Gyro full scale is +/-2000 dps; gesture impacts exceed +/-500 dps. */
    ret = imu_reg_update(LSM6DSV16X_REG_CTRL6, 0x0F, LSM6DSV16X_GYRO_2000_DPS);
    if (ret != 0) {
        LOG_ERR("Failed to set gyro full scale: %d", ret);
        return ret;
    }

    /* Accel full scale is +/-8g; gesture impacts exceed +/-4g. */
    ret = imu_reg_update(LSM6DSV16X_REG_CTRL8, 0x03, LSM6DSV16X_ACCEL_8G);
    if (ret != 0) {
        LOG_ERR("Failed to set accel full scale: %d", ret);
        return ret;
    }

    ret = imu_reg_update(LSM6DSV16X_REG_FUNCTIONS_ENABLE,
                         LSM6DSV16X_FUNCTIONS_ENABLE_TIMESTAMP_EN,
                         LSM6DSV16X_FUNCTIONS_ENABLE_TIMESTAMP_EN);
    if (ret != 0) {
        LOG_ERR("Failed to enable IMU timestamp counter: %d", ret);
        return ret;
    }
    uint8_t functions_enable = 0;
    ret = imu_reg_read(LSM6DSV16X_REG_FUNCTIONS_ENABLE, &functions_enable);
    if (ret != 0) {
        LOG_ERR("Failed to read FUNCTIONS_ENABLE: %d", ret);
        return ret;
    }
    LOG_INF("FUNCTIONS_ENABLE=0x%02x (TIMESTAMP_EN=%u)",
            functions_enable,
            (functions_enable & LSM6DSV16X_FUNCTIONS_ENABLE_TIMESTAMP_EN) ? 1U : 0U);
    uint32_t timestamp_ticks = 0;
    ret = imu_read_timestamp_ticks(&timestamp_ticks);
    if (ret != 0) {
        LOG_ERR("Failed to read IMU timestamp counter: %d", ret);
        return ret;
    }
    LOG_INF("Initial IMU timestamp ticks=%u", timestamp_ticks);

    ret = imu_reg_write(LSM6DSV16X_REG_FIFO_CTRL1, IMU_FIFO_WATERMARK_ROWS & 0xFF);
    if (ret != 0) {
        LOG_ERR("Failed to set FIFO watermark LSB: %d", ret);
        return ret;
    }

    ret = imu_reg_update(LSM6DSV16X_REG_FIFO_CTRL2,
                         LSM6DSV16X_FIFO_CTRL2_WTM_8,
                         (IMU_FIFO_WATERMARK_ROWS >> 8) & LSM6DSV16X_FIFO_CTRL2_WTM_8);
    if (ret != 0) {
        LOG_ERR("Failed to set FIFO watermark MSB: %d", ret);
        return ret;
    }

    ret = imu_reg_write(LSM6DSV16X_REG_FIFO_CTRL3,
                    (LSM6DSV16X_FIFO_BDR_CODE << 4) | LSM6DSV16X_FIFO_BDR_CODE);
    if (ret != 0) {
        LOG_ERR("Failed to set FIFO batch rates: %d", ret);
        return ret;
    }

    ret = imu_reg_update(LSM6DSV16X_REG_FIFO_CTRL4,
                         LSM6DSV16X_FIFO_CTRL4_DEC_TS_BATCH_MASK,
                         LSM6DSV16X_FIFO_CTRL4_DEC_TS_BATCH_1);
    if (ret != 0) {
        LOG_ERR("Failed to enable FIFO timestamp batching: %d", ret);
        return ret;
    }

    ret = imu_set_fifo_mode(LSM6DSV16X_FIFO_MODE_CONTINUOUS);
    if (ret != 0) {
        LOG_ERR("Failed to set FIFO continuous mode: %d", ret);
        return ret;
    }

    ret = imu_reg_write(LSM6DSV16X_REG_INT1_CTRL, LSM6DSV16X_INT1_FIFO_TH);
    if (ret != 0) {
        LOG_ERR("Failed to route FIFO watermark to INT1: %d", ret);
        return ret;
    }

    LOG_INF("IMU FIFO profile set: %d Hz stream, timestamp-tagged FIFO, 20-row watermark",
            IMU_STREAM_HZ);

    return 0;
}

static void imu_int_handler(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    uint32_t timestamp_us = imu_timestamp_now_us();

    ARG_UNUSED(dev);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);

    if (!imu_stream_enabled) {
        return;
    }

    (void)k_msgq_put(&imu_drdy_msgq, &timestamp_us, K_NO_WAIT);
}

static void imu_stream_thread_fn(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    while (1) {
        uint32_t interrupt_timestamp_us = 0;

        /* Wait for a watermark interrupt, but with a timeout so the thread
         * also polls the FIFO periodically. This is the key robustness fix:
         * INT1 (FIFO threshold) is a level signal, so if a single drain ever
         * fails to bring the FIFO below the watermark (I2C glitch, overrun,
         * scheduling delay) the line stays high and no new rising edge is
         * produced. The timeout guarantees we wake and re-check regardless,
         * so the stream can no longer wedge permanently. */
        (void)k_msgq_get(&imu_drdy_msgq, &interrupt_timestamp_us,
                         K_MSEC(IMU_POLL_FALLBACK_MS));
        ARG_UNUSED(interrupt_timestamp_us);

        if (!imu_stream_enabled) {
            memset(imu_fifo_slots, 0, sizeof(imu_fifo_slots));
            continue;
        }

        /* Drain the FIFO fully. Re-reading the status and looping until the
         * FIFO is empty guarantees we leave it below the watermark, so the
         * INT1 line is released and the next watermark crossing produces a
         * fresh edge. */
        while (imu_stream_enabled) {
            uint16_t fifo_rows = 0;
            bool fifo_overrun = false;
            size_t pair_count = 0;
            bool read_error = false;

            if (imu_read_fifo_status(&fifo_rows, &fifo_overrun) != 0) {
                break; /* transient I2C error: retry on next wake/poll */
            }

            if (fifo_overrun) {
                /* We fell behind. Flush to resync immediately instead of
                 * replaying a full 511-row backlog (which would itself keep
                 * the FIFO above the watermark for seconds). The dropped span
                 * is flagged so the app / post-processing can mark it. */
                LOG_WRN("IMU FIFO overrun; flushing and resyncing");
                imu_reset_fifo_parser_state();
                imu_current_batch_flags = BT_APP_IMU_TS_FLAG_FIFO_OVERRUN;
                (void)imu_flush_fifo();
                break;
            }

            if (fifo_rows == 0U) {
                break; /* fully drained */
            }

            imu_current_batch_flags = 0;

            while (fifo_rows > 0U) {
                uint8_t fifo_row[IMU_FIFO_ROW_SIZE];
                uint8_t tag;
                uint8_t slot_index;
                struct imu_fifo_slot *slot;

                if (imu_read_fifo_row(fifo_row) != 0) {
                    LOG_ERR("Failed to read FIFO row");
                    read_error = true;
                    break;
                }

                tag = (fifo_row[0] >> 3) & 0x1F;
                slot_index = (fifo_row[0] >> 1) & 0x03;
                slot = &imu_fifo_slots[slot_index];

                if (tag == IMU_FIFO_TAG_ACCEL) {
                    if (slot->accel_valid && slot->gyro_valid) {
                        imu_emit_slot(slot_index, &pair_count, true);
                    }
                    imu_parse_fifo_axes(fifo_row, slot->accel_raw);
                    slot->accel_valid = true;
                } else if (tag == IMU_FIFO_TAG_GYRO) {
                    if (slot->accel_valid && slot->gyro_valid) {
                        imu_emit_slot(slot_index, &pair_count, true);
                    }
                    imu_parse_fifo_axes(fifo_row, slot->gyro_raw);
                    slot->gyro_valid = true;
                } else if (tag == IMU_FIFO_TAG_TIMESTAMP) {
                    slot->timestamp_ticks = imu_parse_fifo_timestamp_ticks(fifo_row);
                    slot->timestamp_valid = true;
                }

                imu_emit_slot(slot_index, &pair_count, false);
                fifo_rows--;
            }

            if (pair_count > 0U) {
                imu_samples_sent_total += pair_count;
                imu_send_fifo_pairs(pair_count);
            }

            if (read_error) {
                break; /* bail out; poll fallback will retry */
            }
        }

        /* Flush trailing partial pairs only after the FIFO is fully drained,
         * so a sample whose tag rows straddle two status reads still gets its
         * real hardware timestamp instead of an interpolated one. */
        size_t flush_count = 0;
        for (uint8_t slot_index = 0; slot_index < IMU_FIFO_TAG_SLOT_COUNT; slot_index++) {
            imu_emit_slot(slot_index, &flush_count, true);
        }
        if (flush_count > 0U) {
            imu_samples_sent_total += flush_count;
            imu_send_fifo_pairs(flush_count);
        }

        /* Liveness heartbeat: if this log stops while streaming, the IMU
         * pipeline has stalled (as opposed to simply being quiet). */
        if (imu_stream_enabled) {
            int64_t now_ms = k_uptime_get();

            if ((now_ms - imu_heartbeat_last_ms) >= IMU_HEARTBEAT_INTERVAL_MS) {
                imu_heartbeat_last_ms = now_ms;
                LOG_INF("IMU alive: %u samples sent", imu_samples_sent_total);
            }
        }
    }
}

static int setup_imu_interrupt(void)
{
    int ret;

    if (!device_is_ready(gpio1_dev)) {
        LOG_ERR("IMU interrupt GPIO not ready");
        return -ENODEV;
    }

    ret = gpio_pin_configure(gpio1_dev, IMU_INT_PIN, GPIO_INPUT);
    if (ret < 0) {
        LOG_ERR("Error configuring IMU pin: %d", ret);
        return ret;
    }

    ret = gpio_pin_interrupt_configure(gpio1_dev, IMU_INT_PIN, GPIO_INT_DISABLE);
    if (ret < 0) {
        LOG_ERR("Error disabling IMU interrupt: %d", ret);
        return ret;
    }

    gpio_init_callback(&imu_cb_data, imu_int_handler, BIT(IMU_INT_PIN));
    gpio_add_callback(gpio1_dev, &imu_cb_data);

    LOG_INF("IMU watermark interrupt configured on GPIO1.%02u", IMU_INT_PIN);

    return 0;
}

int imu_stream_init(void)
{
    int ret;

    if (!device_is_ready(imu_sensor)) {
        LOG_ERR("IMU sensor driver not ready");
        return -ENODEV;
    }

    if (!device_is_ready(i2c_bus)) {
        LOG_ERR("I2C BUS device not ready");
        return -ENODEV;
    }

    ret = setup_imu_interrupt();
    if (ret != 0) {
        return ret;
    }

    k_thread_create(&imu_stream_thread,
                    imu_stream_stack,
                    K_THREAD_STACK_SIZEOF(imu_stream_stack),
                    imu_stream_thread_fn,
                    NULL, NULL, NULL,
                    IMU_THREAD_PRIORITY, 0, K_NO_WAIT);

    LOG_INF("IMU FIFO streaming endpoint initialized");
    return 0;
}

int imu_stream_start(void)
{
    int ret;
    uint16_t fifo_rows = 0;
    bool fifo_overrun = false;

    LOG_INF("imu_start_streaming: enter");

    imu_stream_enabled = false;

    ret = gpio_pin_interrupt_configure(gpio1_dev, IMU_INT_PIN, GPIO_INT_DISABLE);
    if (ret != 0) {
        LOG_ERR("imu_start_streaming: failed to disable IRQ (%d)", ret);
        return ret;
    }

    k_msgq_purge(&imu_drdy_msgq);

    ret = imu_configure_streaming_profile();
    if (ret != 0) {
        LOG_ERR("imu_start_streaming: configure_streaming_profile failed (%d)", ret);
        return ret;
    }

    ret = imu_flush_fifo();
    if (ret != 0) {
        LOG_ERR("imu_start_streaming: imu_flush_fifo failed (%d)", ret);
        return ret;
    }
    LOG_INF("imu_start_streaming: flush_fifo ok");

    ret = imu_read_fifo_status(&fifo_rows, &fifo_overrun);
    if (ret != 0) {
        LOG_ERR("imu_start_streaming: read_fifo_status failed (%d)", ret);
        return ret;
    }
    LOG_INF("imu_start_streaming: fifo_rows=%u overrun=%d", fifo_rows, (int)fifo_overrun);

    if (fifo_rows != 0U) {
        LOG_WRN("FIFO not empty after flush: %u rows", fifo_rows);
    }

    imu_reset_fifo_parser_state();
    imu_samples_sent_total = 0;
    imu_sample_id = 0;
    imu_heartbeat_last_ms = k_uptime_get();
    imu_stream_enabled = true;

    ret = gpio_pin_interrupt_configure(gpio1_dev, IMU_INT_PIN, GPIO_INT_EDGE_TO_ACTIVE);
    if (ret != 0) {
        LOG_ERR("imu_start_streaming: failed to re-enable IRQ (%d)", ret);
        imu_stream_enabled = false;
    }

    LOG_INF("imu_start_streaming: exit, stream_enabled=%d ret=%d",
            (int)imu_stream_enabled, ret);

    return ret;
}

void imu_stream_stop(void)
{
    imu_stream_enabled = false;
    imu_reset_fifo_parser_state();
    (void)gpio_pin_interrupt_configure(gpio1_dev, IMU_INT_PIN, GPIO_INT_DISABLE);
    k_msgq_purge(&imu_drdy_msgq);
    (void)imu_flush_fifo();
    (void)imu_reg_write(LSM6DSV16X_REG_INT1_CTRL, 0x00);
    (void)imu_reg_write(LSM6DSV16X_REG_CTRL1, 0x00);
    (void)imu_reg_write(LSM6DSV16X_REG_CTRL2, 0x00);
    LOG_INF("IMU stream stopped and sensors powered down");
}
