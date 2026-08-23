#ifndef IMU_STREAM_H
#define IMU_STREAM_H

/* Initializes the FIFO worker and its shared INT1 callback. */
int imu_stream_init(void);

/* Applies the complete 120 Hz FIFO profile and begins sample delivery. */
int imu_stream_start(void);

/* Stops FIFO delivery, clears INT1 routing, and powers the IMU down. */
void imu_stream_stop(void);

#endif
