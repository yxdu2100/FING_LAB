#ifndef BLE_H
#define BLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int ble_init(void);
bool ble_can_send_frame(void);
int ble_send_frame(const uint8_t *frame, size_t length,
                   uint32_t frame_id, uint32_t timestamp_ms);

#endif
