#ifndef MODE_MANAGER_H
#define MODE_MANAGER_H

#include "ble/ble.h"

int mode_manager_init(void);
void mode_manager_request(enum fing_mode mode);
void mode_manager_request_resend(void);
void mode_manager_acknowledge_frame(uint32_t frame_id);
void mode_manager_run(void);

#endif
