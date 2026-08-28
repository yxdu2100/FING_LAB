#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#define TOF_MIN_DISTANCE_MM 60U
#define TOF_MAX_DISTANCE_MM 500U
#define CAMERA_CAPTURE_MAX_ATTEMPTS 5
#define CAMERA_AE_DISCARD_FRAMES 3U

/* HM01B0 image-quality tuning.
 *
 * The current sensor timing is 625 pixel clocks per row at a 1 MHz video
 * timing clock, or approximately 0.625 ms per integration row.  Therefore
 * 80 rows limits motion-producing exposure to approximately 50 ms.  When
 * changing the line timing in hm01b0_init.h, recalculate these row values.
 *
 * Analog gain codes are power-of-two limits: 0=1x, 1=2x, 2=4x, 3=8x.
 * Digital gain uses 2.6 fixed point: 0x40=1x, 0x80=2x, 0xC0=3x.
 */
#define CAMERA_AE_TARGET_MEAN              0x50U
#define CAMERA_AE_MAX_INTEGRATION_ROWS     80U
#define CAMERA_AE_MIN_INTEGRATION_ROWS     2U
#define CAMERA_AE_MAX_ANALOG_GAIN_CODE     0x03U
#define CAMERA_AE_MAX_DIGITAL_GAIN_CODE    0x80U
#define CAMERA_AE_DAMPING_FACTOR           0x20U

/* Indoor-light flicker avoidance for the present 625 us row time. */
#define CAMERA_AE_FLICKER_AVOIDANCE        1
#define CAMERA_MAINS_FREQUENCY_HZ          60U
#define CAMERA_AE_60HZ_STEP_ROWS           13U
#define CAMERA_AE_50HZ_STEP_ROWS           16U

#if CAMERA_AE_MAX_INTEGRATION_ROWS > 398U
#error "CAMERA_AE_MAX_INTEGRATION_ROWS must not exceed frame_length_lines - 2"
#endif

#if CAMERA_AE_MIN_INTEGRATION_ROWS > 255U
#error "CAMERA_AE_MIN_INTEGRATION_ROWS must fit in the HM01B0 8-bit register"
#endif

#if CAMERA_AE_MIN_INTEGRATION_ROWS > CAMERA_AE_MAX_INTEGRATION_ROWS
#error "CAMERA_AE_MIN_INTEGRATION_ROWS must not exceed the maximum"
#endif

#if CAMERA_AE_FLICKER_AVOIDANCE
#if CAMERA_MAINS_FREQUENCY_HZ == 60U
#define CAMERA_AE_FLICKER_CONTROL          0x03U
#elif CAMERA_MAINS_FREQUENCY_HZ == 50U
#define CAMERA_AE_FLICKER_CONTROL          0x01U
#else
#error "CAMERA_MAINS_FREQUENCY_HZ must be 50 or 60"
#endif
#else
#define CAMERA_AE_FLICKER_CONTROL          0x00U
#endif

/* Capture gesture configuration.
 *
 * Replace modules/imu/mlc.h when exporting a new MEMS Studio classifier,
 * then set this value to the MLC1_SRC class that should start capture.  No
 * camera, ToF, BLE, or app code needs to know the gesture's name.
 */
#define CAPTURE_MLC_TARGET_CLASS 0x00U

#define EVENT_IMU_CAPTURE_GESTURE (1U << 0)

#endif
