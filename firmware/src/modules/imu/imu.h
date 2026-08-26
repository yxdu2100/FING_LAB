#ifndef IMU_H
#define IMU_H

typedef void (*imu_gesture_callback_t)(void);

/* Initializes the shared INT1 GPIO callback without selecting an IMU profile. */
int imu_init(imu_gesture_callback_t callback);

/* Loads the exact MEMS Studio table in mlc.h and arms INT1 for MLC1. */
int imu_start_mlc(void);

/* Disarms MLC/INT1 and powers down accelerometer and gyroscope. */
void imu_stop_mlc(void);

#endif
