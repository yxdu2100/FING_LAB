#ifndef BLE_H
#define BLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/sys/util.h>

enum fing_mode {
    FING_MODE_IDLE = 0,
    FING_MODE_CAPTURE = 1,
    FING_MODE_STREAM = 2,
};

typedef void (*ble_mode_request_callback_t)(enum fing_mode requested_mode);

#define BT_APP_IMU_SAMPLE_PAYLOAD_LEN 23U
#define BT_APP_IMU_SAMPLES_PER_PACKET 7U

#define BT_APP_IMU_TS_FLAG_HARDWARE     BIT(0)
#define BT_APP_IMU_TS_FLAG_INTERPOLATED BIT(1)
#define BT_APP_IMU_TS_FLAG_FALLBACK     BIT(2)
#define BT_APP_IMU_TS_FLAG_FIFO_OVERRUN BIT(3)
#define BT_APP_IMU_TS_FLAG_NONMONOTONIC BIT(4)

struct bt_app_imu_sample {
    uint16_t sample_id;
    uint32_t timestamp_us;
    uint32_t timestamp_ticks;
    uint8_t timestamp_flags;
    int16_t accel_raw[3];
    int16_t gyro_raw[3];
};

int ble_init(ble_mode_request_callback_t mode_callback);
void ble_set_active_mode(enum fing_mode mode);
enum fing_mode ble_get_active_mode(void);
bool ble_is_connected(void);

bool ble_can_send_frame(void);
int ble_send_frame(const uint8_t *frame, size_t length,
                   uint32_t frame_id, uint32_t timestamp_ms);
void ble_send_imu_samples(const struct bt_app_imu_sample *samples,
                          size_t sample_count);

#endif
