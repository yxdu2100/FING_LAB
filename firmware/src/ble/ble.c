#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include "ble.h"

LOG_MODULE_REGISTER(ble_module, LOG_LEVEL_INF);

#define ATTR_IMAGE_VALUE_INDEX 2
#define ATTR_STATUS_VALUE_INDEX 7
#define ATTR_IMU_VALUE_INDEX 10
#define ATTR_CAPTURE_STATUS_VALUE_INDEX 13
#define ATTR_DEVICE_INFO_VALUE_INDEX 16
#define NOTIFY_RETRY_TIMEOUT_MS 5000
#define NOTIFY_RETRY_SLEEP_US 1000
#define MAX_PAYLOAD_CHUNK 244U
#define ADV_RESTART_RETRY_DELAY_MS 50
#define ADV_RESTART_MAX_ATTEMPTS 20
#define ADV_RESTART_SLOW_RETRY_MS 1000
#define ADV_FAST_DURATION_SECONDS 30
#define STATUS_NOTIFY_RETRY_DELAY_MS 20
#define STATUS_NOTIFY_MAX_ATTEMPTS 50
#define CONTROL_KEEPALIVE 3U
#define CONTROL_RESEND_LAST_IMAGE 4U
#define CONTROL_ACK_FRAME 5U
#define CONTROL_RECOVER_CAMERA 6U
/* Command 7 is intentionally left available for a future full-MCU restart.
 * The manual camera recovery implemented here never reboots the board. */
#define CONTROL_RESTART_MCU_RESERVED 7U
#define CAPTURE_STATUS_PACKET_LENGTH 16U
#define CAPTURE_STATUS_VERSION 1U
/* Image header v2 keeps the legacy 16-byte prefix and appends:
 * version (u8), header length (u8), and ToF distance in millimetres (u16 LE).
 * At 20 bytes it still fits the notification payload at the default ATT MTU. */
#define IMAGE_HEADER_LENGTH 20U
#define IMAGE_HEADER_VERSION 2U

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
#define BT_UUID_CAPTURE_STATUS_VAL \
    BT_UUID_128_ENCODE(0x1234567e, 0x9abc, 0x11ee, 0xbe56, 0x0242ac120002)
#define BT_UUID_CAPTURE_STATUS BT_UUID_DECLARE_128(BT_UUID_CAPTURE_STATUS_VAL)
#define BT_UUID_DEVICE_INFO_VAL \
    BT_UUID_128_ENCODE(0x1234567f, 0x9abc, 0x11ee, 0xbe56, 0x0242ac120002)
#define BT_UUID_DEVICE_INFO BT_UUID_DECLARE_128(BT_UUID_DEVICE_INFO_VAL)

static struct bt_conn *current_connection;
static bool image_notifications_enabled;
static bool status_notifications_enabled;
static bool imu_notifications_enabled;
static bool capture_status_notifications_enabled;
static uint8_t image_value;
static uint8_t status_value = FING_MODE_IDLE;
static uint8_t imu_value;
static uint8_t capture_status_value[CAPTURE_STATUS_PACKET_LENGTH] = {
    [0] = CAPTURE_STATUS_VERSION,
    [1] = CAPTURE_STATUS_WAITING,
    [10] = 0xFFU,
    [11] = 0xFFU,
};
static char device_info_value[64];
static uint8_t advertising_restart_attempts;
static uint8_t status_notify_attempts;
static volatile uint8_t control_requested_mode = FING_MODE_IDLE;
static volatile bool camera_recovery_requested;
static ble_mode_request_callback_t mode_request_callback;
static ble_resend_request_callback_t resend_request_callback;
static ble_frame_ack_callback_t frame_ack_callback;
static ble_camera_recovery_callback_t camera_recovery_request_callback;

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

static void capture_status_notify_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(capture_status_notify_work,
                               capture_status_notify_work_handler);

static void capture_status_ccc_changed(const struct bt_gatt_attr *attr,
                                       uint16_t value)
{
    ARG_UNUSED(attr);
    capture_status_notifications_enabled = value == BT_GATT_CCC_NOTIFY;
    LOG_INF("Capture status notifications %s",
            capture_status_notifications_enabled ? "enabled" : "disabled");
    if (capture_status_notifications_enabled) {
        (void)k_work_reschedule(&capture_status_notify_work, K_NO_WAIT);
    }
}

static ssize_t read_status(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           void *buf, uint16_t len, uint16_t offset)
{
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                             &status_value, sizeof(status_value));
}

static ssize_t read_capture_status(struct bt_conn *conn,
                                   const struct bt_gatt_attr *attr,
                                   void *buf, uint16_t len, uint16_t offset)
{
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                             capture_status_value,
                             sizeof(capture_status_value));
}

static ssize_t read_device_info(struct bt_conn *conn,
                                const struct bt_gatt_attr *attr,
                                void *buf, uint16_t len, uint16_t offset)
{
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                             device_info_value,
                             strlen(device_info_value));
}

static ssize_t write_control(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                             const void *buf, uint16_t len, uint16_t offset,
                             uint8_t flags)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);

    if (offset != 0U || (len != 1U && len != 5U)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    uint8_t command = ((const uint8_t *)buf)[0];
    if (command == CONTROL_ACK_FRAME) {
        if (len != 5U) {
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
        }
        if (frame_ack_callback != NULL) {
            frame_ack_callback(sys_get_le32(&((const uint8_t *)buf)[1]));
        }
        return len;
    }
    if (len != 1U) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    if (command == CONTROL_KEEPALIVE) {
        return len;
    }
    if (command == CONTROL_RESEND_LAST_IMAGE) {
        if (resend_request_callback != NULL) {
            resend_request_callback();
        }
        return len;
    }
    if (command == CONTROL_RECOVER_CAMERA) {
        /* This flag also stops a BLE image notification loop promptly. The
         * mode-manager owns the actual recovery and clears it when done. */
        camera_recovery_requested = true;
        LOG_INF("App requested camera recovery");
        if (camera_recovery_request_callback != NULL) {
            camera_recovery_request_callback();
        } else {
            camera_recovery_requested = false;
        }
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
    BT_GATT_CCC(imu_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    BT_GATT_CHARACTERISTIC(BT_UUID_CAPTURE_STATUS,
        BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
        BT_GATT_PERM_READ, read_capture_status, NULL, capture_status_value),
    BT_GATT_CCC(capture_status_ccc_changed,
                BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    BT_GATT_CHARACTERISTIC(BT_UUID_DEVICE_INFO,
        BT_GATT_CHRC_READ, BT_GATT_PERM_READ,
        read_device_info, NULL, device_info_value)
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

static void capture_status_notify_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (current_connection == NULL || !capture_status_notifications_enabled) {
        return;
    }

    int err = bt_gatt_notify(current_connection,
                             &fing_service.attrs[ATTR_CAPTURE_STATUS_VALUE_INDEX],
                             capture_status_value,
                             sizeof(capture_status_value));
    if (err == -ENOMEM || err == -EAGAIN) {
        (void)k_work_reschedule(&capture_status_notify_work,
                                K_MSEC(STATUS_NOTIFY_RETRY_DELAY_MS));
    } else if (err != 0) {
        LOG_WRN("Could not notify capture status %u: %d",
                capture_status_value[1], err);
    }
}

static const struct bt_data advertising_data[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
            sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static const struct bt_data scan_response[] = {
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_FING_SERVICE_VAL),
};

static const struct bt_le_adv_param slow_advertising_parameters =
    BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONN,
                         BT_GAP_ADV_SLOW_INT_MIN,
                         BT_GAP_ADV_SLOW_INT_MAX,
                         NULL);

static int start_advertising(bool slow)
{
    const struct bt_le_adv_param *parameters = slow
        ? &slow_advertising_parameters
        : BT_LE_ADV_CONN_FAST_1;
    return bt_le_adv_start(parameters,
                           advertising_data, ARRAY_SIZE(advertising_data),
                           scan_response, ARRAY_SIZE(scan_response));
}

static void restart_advertising_work_handler(struct k_work *work);
static void schedule_advertising_restart(void);
static K_WORK_DELAYABLE_DEFINE(restart_advertising_work,
                               restart_advertising_work_handler);
static void slow_advertising_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(slow_advertising_work,
                               slow_advertising_work_handler);

static void schedule_slow_advertising(void)
{
    (void)k_work_reschedule(&slow_advertising_work,
                            K_SECONDS(ADV_FAST_DURATION_SECONDS));
}

static void slow_advertising_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    if (current_connection != NULL) {
        return;
    }

    int err = bt_le_adv_stop();
    if (err != 0 && err != -EALREADY) {
        LOG_WRN("Could not stop fast advertising: %d", err);
        return;
    }

    err = start_advertising(true);
    if (err == 0 || err == -EALREADY) {
        LOG_INF("Using low-duty advertising while idle");
    } else {
        LOG_WRN("Could not start low-duty advertising: %d", err);
        schedule_advertising_restart();
    }
}

static void restart_advertising_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    if (current_connection != NULL) {
        advertising_restart_attempts = 0U;
        return;
    }

    int err = start_advertising(false);
    if (err == 0 || err == -EALREADY) {
        advertising_restart_attempts = 0U;
        schedule_slow_advertising();
        LOG_INF("Fast advertising restarted");
        return;
    }

    /* Just after a disconnect this can fail with -ENOMEM until the stack frees
     * the connection object, so retry quickly at first. Never give up: a ring
     * that stops advertising is unreachable until it is power-cycled. */
    if (advertising_restart_attempts < ADV_RESTART_MAX_ATTEMPTS) {
        advertising_restart_attempts++;
        LOG_WRN("Advertising restart failed: %d (attempt %u/%u)",
                err, advertising_restart_attempts, ADV_RESTART_MAX_ATTEMPTS);
        (void)k_work_reschedule(&restart_advertising_work,
                                K_MSEC(ADV_RESTART_RETRY_DELAY_MS));
    } else {
        LOG_WRN("Advertising restart still failing: %d; retrying in %u ms",
                err, ADV_RESTART_SLOW_RETRY_MS);
        (void)k_work_reschedule(&restart_advertising_work,
                                K_MSEC(ADV_RESTART_SLOW_RETRY_MS));
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
    (void)k_work_cancel_delayable(&slow_advertising_work);
    current_connection = bt_conn_ref(connection);
    struct bt_le_conn_param *parameters = BT_LE_CONN_PARAM(12, 12, 0, 400);
    (void)bt_conn_le_param_update(connection, parameters);
    /* Start on the longer-range 1M PHY. The mode manager requests 2M only
     * for the low-volume IMU stream; image capture intentionally stays 1M. */
    ble_set_preferred_phy(FING_MODE_IDLE);
    LOG_INF("Phone connected");
}

static void disconnected(struct bt_conn *connection, uint8_t reason)
{
    ARG_UNUSED(connection);
    LOG_INF("Phone disconnected (0x%02x)", reason);
    image_notifications_enabled = false;
    status_notifications_enabled = false;
    imu_notifications_enabled = false;
    capture_status_notifications_enabled = false;
    control_requested_mode = FING_MODE_IDLE;
    camera_recovery_requested = false;
    (void)k_work_cancel_delayable(&status_notify_work);
    (void)k_work_cancel_delayable(&capture_status_notify_work);
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

int ble_init(ble_mode_request_callback_t callback,
             ble_resend_request_callback_t resend_callback,
             ble_frame_ack_callback_t ack_callback,
             ble_camera_recovery_callback_t recovery_callback)
{
    mode_request_callback = callback;
    resend_request_callback = resend_callback;
    frame_ack_callback = ack_callback;
    camera_recovery_request_callback = recovery_callback;
    (void)snprintf(device_info_value, sizeof(device_info_value),
                   "1|%s|%d|%s", CONFIG_FING_BOARD_ID,
                   CONFIG_FING_RING_SIZE_MM, CONFIG_FING_FIRMWARE_VERSION);
    int err = bt_enable(NULL);
    if (err != 0) {
        return err;
    }
    err = start_advertising(false);
    if (err == 0) {
        schedule_slow_advertising();
        LOG_INF("Advertising as %s (%s)", CONFIG_BT_DEVICE_NAME,
                device_info_value);
    } else {
        LOG_ERR("Initial advertising start failed: %d; retrying", err);
        schedule_advertising_restart();
    }
    return 0;
}

void ble_set_active_mode(enum fing_mode mode)
{
    status_value = (uint8_t)mode;
    status_notify_attempts = 0U;
    (void)k_work_reschedule(&status_notify_work, K_NO_WAIT);
}

void ble_set_preferred_phy(enum fing_mode mode)
{
    if (current_connection == NULL) {
        return;
    }

    uint8_t phy = mode == FING_MODE_STREAM ? BT_GAP_LE_PHY_2M
                                           : BT_GAP_LE_PHY_1M;
    const struct bt_conn_le_phy_param parameters = {
        .options = BT_CONN_LE_PHY_OPT_NONE,
        .pref_tx_phy = phy,
        .pref_rx_phy = phy,
    };
    int err = bt_conn_le_phy_update(current_connection, &parameters);
    if (err != 0) {
        LOG_WRN("Could not request %s PHY: %d",
                phy == BT_GAP_LE_PHY_2M ? "2M" : "1M", err);
    } else {
        LOG_INF("Requested %s PHY for mode %u",
                phy == BT_GAP_LE_PHY_2M ? "2M" : "1M", mode);
    }
}

void ble_publish_capture_status(enum capture_status_event event,
                                uint32_t attempt_id,
                                uint16_t distance_mm,
                                int16_t detail,
                                uint8_t progress,
                                uint8_t total)
{
    capture_status_value[0] = CAPTURE_STATUS_VERSION;
    capture_status_value[1] = (uint8_t)event;
    sys_put_le32(attempt_id, &capture_status_value[2]);
    sys_put_le32(k_uptime_get_32(), &capture_status_value[6]);
    sys_put_le16(distance_mm, &capture_status_value[10]);
    sys_put_le16((uint16_t)detail, &capture_status_value[12]);
    capture_status_value[14] = progress;
    capture_status_value[15] = total;

    if (current_connection == NULL || !capture_status_notifications_enabled) {
        return;
    }

    int err = bt_gatt_notify(current_connection,
                             &fing_service.attrs[ATTR_CAPTURE_STATUS_VALUE_INDEX],
                             capture_status_value,
                             sizeof(capture_status_value));
    if (err == -ENOMEM || err == -EAGAIN) {
        (void)k_work_reschedule(&capture_status_notify_work,
                                K_MSEC(STATUS_NOTIFY_RETRY_DELAY_MS));
    } else if (err != 0) {
        LOG_WRN("Could not publish capture status %u: %d", event, err);
    }
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
           control_requested_mode == FING_MODE_CAPTURE &&
           !camera_recovery_requested;
}

bool ble_is_camera_recovery_requested(void)
{
    return camera_recovery_requested;
}

void ble_camera_recovery_request_handled(void)
{
    camera_recovery_requested = false;
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
    if (camera_recovery_requested) {
        return -ECANCELED;
    }
    return -ENOTCONN;
}

int ble_send_frame(const uint8_t *frame, size_t length,
                   uint32_t frame_id, uint32_t timestamp_ms,
                   uint16_t distance_mm)
{
    uint8_t header[IMAGE_HEADER_LENGTH];
    if (frame == NULL || length == 0U || length > UINT32_MAX) {
        return -EINVAL;
    }
    if (!ble_can_send_frame()) {
        return -ENOTCONN;
    }

    LOG_INF("Frame %u BLE source timestamp=%u distance=%u mm CRC32=0x%08x",
            frame_id, timestamp_ms, distance_mm, crc32_ieee(frame, length));

    sys_put_le32(0x12345678U, &header[0]);
    sys_put_le32(frame_id, &header[4]);
    sys_put_le32(timestamp_ms, &header[8]);
    sys_put_le32((uint32_t)length, &header[12]);
    header[16] = IMAGE_HEADER_VERSION;
    header[17] = IMAGE_HEADER_LENGTH;
    sys_put_le16(distance_mm, &header[18]);
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
