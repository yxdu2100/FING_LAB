#ifndef CAMERA_H
#define CAMERA_H
#include <stdint.h>


#define IMG_W 324
#define IMG_H 244
#define FRAME_BYTES (IMG_W * IMG_H)

enum camera_diagnostic_stage {
    CAMERA_DIAG_IDLE = 0,
    CAMERA_DIAG_FRAME_START = 1,
    CAMERA_DIAG_SEARCHING_VBLANK = 2,
    CAMERA_DIAG_WAITING_ROW_ZERO = 3,
    CAMERA_DIAG_CAPTURING_ROWS = 4,
    CAMERA_DIAG_DRAINING_TRANSFER = 5,
    CAMERA_DIAG_FRAME_COMPLETE = 6,
    CAMERA_DIAG_ERROR = 7,
};

struct camera_diagnostic_snapshot {
    enum camera_diagnostic_stage stage;
    uint8_t row;
    int16_t last_error;
};

int camera_init(void);
int camera_standby(void);
int camera_capture_frame(void);
int camera_wake_up(void);
int camera_recover_stream(void);
int camera_log_ae_state(unsigned int burst_frame);
void camera_get_diagnostic_snapshot(struct camera_diagnostic_snapshot *snapshot);
uint8_t *camera_get_frame_buffer(void);


#endif
