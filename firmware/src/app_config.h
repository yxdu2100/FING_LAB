#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#define TOF_MIN_DISTANCE_MM 60U
#define TOF_MAX_DISTANCE_MM 500U
#define CAMERA_CAPTURE_MAX_ATTEMPTS 5
#define CAMERA_AE_DISCARD_FRAMES 3U

/* Capture gesture configuration.
 *
 * Replace modules/imu/mlc.h when exporting a new MEMS Studio classifier,
 * then set this value to the MLC1_SRC class that should start capture.  No
 * camera, ToF, BLE, or app code needs to know the gesture's name.
 */
#define CAPTURE_MLC_TARGET_CLASS 0x00U

#define EVENT_IMU_CAPTURE_GESTURE (1U << 0)

#endif
