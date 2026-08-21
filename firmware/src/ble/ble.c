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

#include "ble.h"

LOG_MODULE_REGISTER(ble_module, LOG_LEVEL_INF);

#define ATTR_IMAGE_VALUE_INDEX 2
#define NOTIFY_RETRY_TIMEOUT_MS 5000
#define NOTIFY_RETRY_SLEEP_US 1000
#define MAX_PAYLOAD_CHUNK 244U
#define ADV_RESTART_RETRY_DELAY_MS 50
#define ADV_RESTART_MAX_ATTEMPTS 20

#define BT_UUID_IMAGE_SERVICE_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x9abc, 0x11ee, 0xbe56, 0x0242ac120002)
#define BT_UUID_IMAGE_SERVICE BT_UUID_DECLARE_128(BT_UUID_IMAGE_SERVICE_VAL)
#define BT_UUID_IMAGE_DATA_VAL \
    BT_UUID_128_ENCODE(0x1234567a, 0x9abc, 0x11ee, 0xbe56, 0x0242ac120002)
#define BT_UUID_IMAGE_DATA BT_UUID_DECLARE_128(BT_UUID_IMAGE_DATA_VAL)

static struct bt_conn *current_connection;
static bool image_notifications_enabled;
static uint8_t image_value;
static uint8_t advertising_restart_attempts;

static void image_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    ARG_UNUSED(attr);
    image_notifications_enabled = value == BT_GATT_CCC_NOTIFY;
    LOG_INF("Image notifications %s", image_notifications_enabled ? "enabled" : "disabled");
}

BT_GATT_SERVICE_DEFINE(image_service,
    BT_GATT_PRIMARY_SERVICE(BT_UUID_IMAGE_SERVICE),
    BT_GATT_CHARACTERISTIC(BT_UUID_IMAGE_DATA,
        BT_GATT_CHRC_NOTIFY, 0, NULL, NULL, &image_value),
    BT_GATT_CCC(image_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE)
);

static const struct bt_data advertising_data[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
            sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static const struct bt_data scan_response[] = {
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_IMAGE_SERVICE_VAL),
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

    /* A new connection can arrive before delayed work runs. */
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
    } else {
        LOG_ERR("Advertising restart abandoned after %u attempts",
                ADV_RESTART_MAX_ATTEMPTS);
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
    advertising_restart_attempts = 0U;
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
    if (current_connection != NULL) {
        bt_conn_unref(current_connection);
        current_connection = NULL;
    }
    /* Restart outside the Bluetooth callback. The controller may still be
     * releasing the previous connection and can transiently return -ENOMEM. */
    schedule_advertising_restart();
}

BT_CONN_CB_DEFINE(connection_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
};

static int notify_with_retry(const uint8_t *data, uint16_t length)
{
    int64_t start = k_uptime_get();

    while (ble_can_send_frame()) {
        int err = bt_gatt_notify(current_connection,
                                 &image_service.attrs[ATTR_IMAGE_VALUE_INDEX],
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
    return -ENOTCONN;
}

int ble_init(void)
{
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

bool ble_can_send_frame(void)
{
    return current_connection != NULL && image_notifications_enabled;
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

    int err = notify_with_retry(header, sizeof(header));
    if (err != 0) {
        return err;
    }

    size_t mtu_payload = bt_gatt_get_mtu(current_connection) - 3U;
    size_t chunk_size = MIN(mtu_payload, MAX_PAYLOAD_CHUNK);
    for (size_t offset = 0U; offset < length; offset += chunk_size) {
        uint16_t count = (uint16_t)MIN(chunk_size, length - offset);
        err = notify_with_retry(&frame[offset], count);
        if (err != 0) {
            LOG_ERR("Frame %u stopped at byte %u: %d",
                    frame_id, (uint32_t)offset, err);
            return err;
        }
    }

    LOG_INF("Frame %u sent (%u bytes)", frame_id, (uint32_t)length);
    return 0;
}
