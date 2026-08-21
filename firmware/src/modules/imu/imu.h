#ifndef IMU_H
#define IMU_H

typedef void (*imu_double_flick_callback_t)(void);

/* Loads the exact MEMS Studio table in mlc.h and arms INT1. */
int imu_init(imu_double_flick_callback_t callback);

#endif
