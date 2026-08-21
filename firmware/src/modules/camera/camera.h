#ifndef CAMERA_H
#define CAMERA_H
#include <stdint.h>


#define IMG_W 324
#define IMG_H 244
#define FRAME_BYTES (IMG_W * IMG_H)

void camera_init(void);
void camera_standby(void);
int camera_capture_frame(void);
void camera_wake_up(void);
uint8_t *camera_get_frame_buffer(void);


#endif
