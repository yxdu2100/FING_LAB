#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "ble.h"

LOG_MODULE_REGISTER(ble_module, LOG_LEVEL_INF);

#define ATTR_IMAGE_VALUE_INDEX 2
#define ATTR_STATUS_VALUE_INDEX 7
#define ATTR_IMU_VALUE_INDEX 10
#define NOTIFY_RETRY_TIMEOUT_MS 5000
#define NOTIFY_RETRY_SLEEP_US 1000
#define MAX_PAYLOAD_CHUNK 244U
#define ADV_RESTART_RETRY_DELAY_MS 50
#define ADV_RESTART_MAX_ATTEMPTS 20
#define STATUS_NOTIFY_RETRY_DELAY_MS 20
#define STATUS_NOTIFY_MAX_ATTEMPTS 50
#define CONTROL_KEEPALIVE 3U

#define BT_UUID_FING_SERVICE_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x9abc, 0x11ee, 0xbe56, 0x0242ac120002)
#define BT_UUID_FING_SERVICE BT_UUID_DECLARE_128(BT_UUID_FING_SERVICE_VAL)
#define BT_UUID_IMAGE_DATA_VAL \
    BT_UUID_128_ENCODE(0x1234567a, 0x9abc, 0x11ee, 0xbe56, 0x0242ac120002)
#define BT_UUID_IMAGE_DATA BT_UUID_DECLARE_128(BT_UUID_IMAGE_DATA_VAL)
#define BT_UUID_CONTROL_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcde01)
#define BT_UUID_CONTROL BT_UUID_DECLARE_128(BT_UUID_CONTROL_VAL)
#define BT_UUID_STATUS_VAL \
    BT_UUID_128_ENCODE(0x1234567c, 0x9abc, 0x11ee, 0xbe56, 0x0242ac120002)
#define BT_UUID_STATUS BT_UUID_DECLARE_128(BT_UUID_STATUS_VAL)
#define BT_UUID_IMU_DATA_VAL \
    BT_UUID_128_ENCODE(0x1234567d, 0x9abc, 0x11ee, 0xbe56, 0x0242ac120002)
#define BT_UUID_IMU_DATA BT_UUID_DECLARE_128(BT_UUID_IMU_DATA_VAL)

static struct bt_conn *current_connection;
static bool image_notifications_enabled;
static bool status_notifications_enabled;
static bool imu_notifications_enabled;
static uint8_t image_value;
static uint8_t status_value = FING_MODE_IDLE;
static uint8_t imu_value;
static uint8_t advertising_restart_attempts;
static uint8_t status_notify_attempts;
static volatile uint8_t control_requested_mode = FING_MODE_IDLE;
static ble_mode_request_callback_t mode_request_callback;

static void image_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    ARG_UNUSED(attr);
    image_notifications_enabled = value == BT_GATT_CCC_NOTIFY;
    LOG_INF("Image notifications %s", image_notifications_enabled ? "enabled" : "disabled");
}

static void status_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    ARG_UNUSED(attr);
    status_notifications_enabled = value == BT_GATT_CCC_NOTIFY;
    LOG_INF("Mode status notifications %s", status_notifications_enabled ? "enabled" : "disabled");
}

static void imu_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    ARG_UNUSED(attr);
    imu_notifications_enabled = value == BT_GATT_CCC_NOTIFY;
    LOG_INF("IMU notifications %s", imu_notifications_enabled ? "enabled" : "disabled");
}

static ssize_t read_status(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           void *buf, uint16_t len, uint16_t offset)
{
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                             &status_value, sizeof(status_value));
}

static ssize_t write_control(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                             const void *buf, uint16_t len, uint16_t offset,
                             uint8_t flags)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);

    if (offset != 0U || len != 1U) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    uint8_t command = ((const uint8_t *)buf)[0];
    if (command == CONTROL_KEEPALIVE) {
        return len;
    }
    if (command > FING_MODE_STREAM) {
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }

    /* Publish the request before waking the mode manager.  Image notification
     * loops use this value to stop promptly when the app requests IDLE. */
    control_requested_mode = command;
    LOG_INF("App requested mode %u", command);
    if (mode_request_callback != NULL) {
        mode_request_callback((enum fing_mode)command);
    }
    return len;
}

BT_GATT_SERVICE_DEFINE(fing_service,
    BT_GATT_PRIMARY_SERVICE(BT_UUID_FING_SERVICE),
    BT_GATT_CHARACTERISTIC(BT_UUID_IMAGE_DATA,
        BT_GATT_CHRC_NOTIFY, 0, NULL, NULL, &image_value),
    BT_GATT_CCC(image_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    BT_GATT_CHARACTERISTIC(BT_UUID_CONTROL,
        BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
        BT_GATT_PERM_WRITE, NULL, write_control, NULL),
    BT_GATT_CHARACTERISTIC(BT_UUID_STATUS,
        BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
        BT_GATT_PERM_READ, read_status, NULL, &status_value),
    BT_GATT_CCC(status_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    BT_GATT_CHARACTERISTIC(BT_UUID_IMU_DATA,
        BT_GATT_CHRC_NOTIFY, 0, NULL, NULL, &imu_value),
    BT_GATT_CCC(imu_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE)
);

static void status_notify_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(status_notify_work, status_notify_work_handler);

static void status_notify_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (current_connection == NULL || !status_notifications_enabled) {
        status_notify_attempts = 0U;
        return;
    }

    int err = bt_gatt_notify(current_connection,
                             &fing_service.attrs[ATTR_STATUS_VALUE_INDEX],
                             &status_value, sizeof(status_value));
    if (err == 0) {
        status_notify_attempts = 0U;
        return;
    }

    if ((err == -ENOMEM || err == -EAGAIN) &&
        status_notify_attempts < STATUS_NOTIFY_MAX_ATTEMPTS) {
        status_notify_attempts++;
        (void)k_work_reschedule(&status_notify_work,
                                K_MSEC(STATUS_NOTIFY_RETRY_DELAY_MS));
        return;
    }

    LOG_WRN("Could not notify mode status %u: %d", status_value, err);
    status_notify_attempts = 0U;
}

static const struct bt_data advertising_data[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
            sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static const struct bt_data scan_response[] = {
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_FING_SERVICE_VAL),
};

static int start_advertising(void)
{
    return bt_le_adv_start(BT_LE_ADV_CONN_FAST_1,
                           advertising_data, ARRAY_SIZE(advertising_data),
                           scan_response, ARRAY_SIZE(scan_response));
}

static void restart_advertising_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(restart_advertising_work,
                               restart_advertising_work_handler);

static void restart_advertising_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    if (current_connection != NULL) {
        advertising_restart_attempts = 0U;
        return;
    }

    int err = start_advertising();
    if (err == 0 || err == -EALREADY) {
        advertising_restart_attempts = 0U;
        LOG_INF("Advertising restarted");
        return;
    }

    advertising_restart_attempts++;
    LOG_WRN("Advertising restart failed: %d (attempt %u/%u)",
            err, advertising_restart_attempts, ADV_RESTART_MAX_ATTEMPTS);
    if (advertising_restart_attempts < ADV_RESTART_MAX_ATTEMPTS) {
        (void)k_work_reschedule(&restart_advertising_work,
                                K_MSEC(ADV_RESTART_RETRY_DELAY_MS));
    }
}

static void schedule_advertising_restart(void)
{
    advertising_restart_attempts = 0U;
    (void)k_work_reschedule(&restart_advertising_work,
                            K_MSEC(ADV_RESTART_RETRY_DELAY_MS));
}

static void connected(struct bt_conn *connection, uint8_t error)
{
    if (error != 0U) {
        LOG_ERR("BLE connection failed (0x%02x)", error);
        schedule_advertising_restart();
        return;
    }

    (void)k_work_cancel_delayable(&restart_advertising_work);
    current_connection = bt_conn_ref(connection);
    struct bt_le_conn_param *parameters = BT_LE_CONN_PARAM(12, 12, 0, 400);
    (void)bt_conn_le_param_update(connection, parameters);
    const struct bt_conn_le_phy_param phy = {
        .options = BT_CONN_LE_PHY_OPT_NONE,
        .pref_tx_phy = BT_GAP_LE_PHY_2M,
        .pref_rx_phy = BT_GAP_LE_PHY_2M,
    };
    (void)bt_conn_le_phy_update(connection, &phy);
    LOG_INF("Phone connected");
}

static void disconnected(struct bt_conn *connection, uint8_t reason)
{
    ARG_UNUSED(connection);
    LOG_INF("Phone disconnected (0x%02x)", reason);
    image_notifications_enabled = false;
    status_notifications_enabled = false;
    imu_notifications_enabled = false;
    control_requested_mode = FING_MODE_IDLE;
    (void)k_work_cancel_delayable(&status_notify_work);
    if (current_connection != NULL) {
        bt_conn_unref(current_connection);
        current_connection = NULL;
    }
    if (mode_request_callback != NULL) {
        mode_request_callback(FING_MODE_IDLE);
    }
    schedule_advertising_restart();
}

BT_CONN_CB_DEFINE(connection_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
};

int ble_init(ble_mode_request_callback_t callback)
{
    mode_request_callback = callback;
    int err = bt_enable(NULL);
    if (err != 0) {
        return err;
    }
    err = start_advertising();
    if (err == 0) {
        LOG_INF("Advertising as %s", CONFIG_BT_DEVICE_NAME);
    }
    return err;
}

void ble_set_active_mode(enum fing_mode mode)
{
    status_value = (uint8_t)mode;
    status_notify_attempts = 0U;
    (void)k_work_reschedule(&status_notify_work, K_NO_WAIT);
}

enum fing_mode ble_get_active_mode(void)
{
    return (enum fing_mode)status_value;
}

bool ble_is_connected(void)
{
    return current_connection != NULL;
}

bool ble_can_send_frame(void)
{
    return status_value == FING_MODE_CAPTURE && current_connection != NULL &&
           image_notifications_enabled &&
           control_requested_mode == FING_MODE_CAPTURE;
}

static int notify_image_with_retry(const uint8_t *data, uint16_t length)
{
    int64_t start = k_uptime_get();
    while (ble_can_send_frame()) {
        int err = bt_gatt_notify(current_connection,
                                 &fing_service.attrs[ATTR_IMAGE_VALUE_INDEX],
                                 data, length);
        if (err == 0) {
            return 0;
        }
        if (err != -ENOMEM && err != -EAGAIN) {
            return err;
        }
        if (k_uptime_get() - start >= NOTIFY_RETRY_TIMEOUT_MS) {
            return -ETIMEDOUT;
        }
        k_usleep(NOTIFY_RETRY_SLEEP_US);
    }
    if (control_requested_mode != FING_MODE_CAPTURE) {
        return -ECANCELED;
    }
    return -ENOTCONN;
}

int ble_send_frame(const uint8_t *frame, size_t length,
                   uint32_t frame_id, uint32_t timestamp_ms)
{
    uint8_t header[16];
    if (frame == NULL || length == 0U || length > UINT32_MAX) {
        return -EINVAL;
    }
    if (!ble_can_send_frame()) {
        return -ENOTCONN;
    }

    sys_put_le32(0x12345678U, &header[0]);
    sys_put_le32(frame_id, &header[4]);
    sys_put_le32(timestamp_ms, &header[8]);
    sys_put_le32((uint32_t)length, &header[12]);
    int err = notify_image_with_retry(header, sizeof(header));
    if (err != 0) {
        return err;
    }

    size_t mtu_payload = bt_gatt_get_mtu(current_connection) - 3U;
    size_t chunk_size = MIN(mtu_payload, MAX_PAYLOAD_CHUNK);
    for (size_t offset = 0U; offset < length; offset += chunk_size) {
        uint16_t count = (uint16_t)MIN(chunk_size, length - offset);
        err = notify_image_with_retry(&frame[offset], count);
        if (err != 0) {
            LOG_ERR("Frame %u stopped at byte %u: %d",
                    frame_id, (uint32_t)offset, err);
            return err;
        }
    }
    LOG_INF("Frame %u sent (%u bytes)", frame_id, (uint32_t)length);
    return 0;
}

void ble_send_imu_samples(const struct bt_app_imu_sample *samples,
                          size_t sample_count)
{
    uint8_t payload[BT_APP_IMU_SAMPLE_PAYLOAD_LEN * BT_APP_IMU_SAMPLES_PER_PACKET];
    static uint32_t dropped_batches;

    if (samples == NULL || sample_count == 0U ||
        status_value != FING_MODE_STREAM || current_connection == NULL ||
        !imu_notifications_enabled) {
        return;
    }

    while (sample_count > 0U) {
        size_t packet_count = MIN(sample_count, (size_t)BT_APP_IMU_SAMPLES_PER_PACKET);
        for (size_t i = 0U; i < packet_count; i++) {
            uint8_t *out = &payload[i * BT_APP_IMU_SAMPLE_PAYLOAD_LEN];
            const struct bt_app_imu_sample *sample = &samples[i];
            sys_put_le16(sample->sample_id, &out[0]);
            sys_put_le32(sample->timestamp_us, &out[2]);
            sys_put_le32(sample->timestamp_ticks, &out[6]);
            out[10] = sample->timestamp_flags;
            sys_put_le16((uint16_t)sample->accel_raw[0], &out[11]);
            sys_put_le16((uint16_t)sample->accel_raw[1], &out[13]);
            sys_put_le16((uint16_t)sample->accel_raw[2], &out[15]);
            sys_put_le16((uint16_t)sample->gyro_raw[0], &out[17]);
            sys_put_le16((uint16_t)sample->gyro_raw[1], &out[19]);
            sys_put_le16((uint16_t)sample->gyro_raw[2], &out[21]);
        }

        int err = bt_gatt_notify(current_connection,
                                 &fing_service.attrs[ATTR_IMU_VALUE_INDEX],
                                 payload,
                                 packet_count * BT_APP_IMU_SAMPLE_PAYLOAD_LEN);
        if (err != 0) {
            dropped_batches++;
            if ((dropped_batches % 32U) == 1U) {
                LOG_WRN("Dropped IMU BLE batch (err %d, total %u)",
                        err, dropped_batches);
            }
        }
        samples += packet_count;
        sample_count -= packet_count;
    }
}
