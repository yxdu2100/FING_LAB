#ifndef IMU_H
#define IMU_H

typedef void (*imu_gesture_callback_t)(void);

/* Initializes the shared INT1 GPIO callback without selecting an IMU profile. */
int imu_init(imu_gesture_callback_t callback);

/* Loads the exact MEMS Studio table in mlc.h and arms INT1 for MLC1. */
int imu_start_mlc(void);

/* Disarms MLC/INT1 and powers down accelerometer and gyroscope. */
void imu_stop_mlc(void);

/* Start/stop the capture detector selected in app_config.h. The original MLC
 * entry points above remain available when comparing the two detectors. */
int imu_start_capture_detector(void);
void imu_rearm_capture_detector(void);
void imu_stop_capture_detector(void);

#endif
