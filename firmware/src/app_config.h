#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#define TOF_MIN_DISTANCE_MM 60U
#define TOF_MAX_DISTANCE_MM 500U
#define CAMERA_CAPTURE_MAX_ATTEMPTS 5
#define CAMERA_AE_DISCARD_FRAMES 3U

/* HM01B0 image-quality tuning.
 *
 * The current sensor timing is 800 pixel clocks per row at a 1 MHz video
 * timing clock, or approximately 0.8 ms per integration row.  Therefore
 * 30 rows limits motion-producing exposure to approximately 24 ms.  When
 * changing the line timing in hm01b0_init.h, recalculate these row values.
 *
 * Analog gain codes are power-of-two limits: 0=1x, 1=2x, 2=4x, 3=8x.
 * Digital gain uses 2.6 fixed point: 0x40=1x, 0x80=2x, 0xC0=3x.
 */
#define CAMERA_AE_TARGET_MEAN              0x40U
#define CAMERA_LINE_LENGTH_PCK             800U
#define CAMERA_FRAME_LENGTH_LINES          313U
#define CAMERA_AE_MAX_INTEGRATION_ROWS     30U
#define CAMERA_AE_MIN_INTEGRATION_ROWS     2U
#define CAMERA_AE_MAX_ANALOG_GAIN_CODE     0x03U
#define CAMERA_AE_MAX_DIGITAL_GAIN_CODE    0x60U
#define CAMERA_AE_DAMPING_FACTOR           0x20U

/* Indoor-light flicker avoidance for the present 800 us row time. */
#define CAMERA_AE_FLICKER_AVOIDANCE        1
#define CAMERA_MAINS_FREQUENCY_HZ          60U
#define CAMERA_AE_60HZ_STEP_ROWS           10U
#define CAMERA_AE_50HZ_STEP_ROWS           13U

#if CAMERA_AE_MAX_INTEGRATION_ROWS > (CAMERA_FRAME_LENGTH_LINES - 2U)
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
 * Both implementations remain compiled into the firmware. Change only
 * CAPTURE_GESTURE_SOURCE to switch between the original MEMS Studio MLC
 * interrupt and the host-side rolling-window hover detector.
 */
#define CAPTURE_GESTURE_SOURCE_MLC        0U
#define CAPTURE_GESTURE_SOURCE_THRESHOLD  1U
#define CAPTURE_GESTURE_SOURCE CAPTURE_GESTURE_SOURCE_THRESHOLD

/* Original MLC path. Replace modules/imu/mlc.h when exporting a classifier,
 * then select the MLC source above and set its target class here. */
#define CAPTURE_MLC_TARGET_CLASS 0x00U

/* Threshold hover path. A 50-sample window (about 417 ms at 120 Hz) counts as
 * hover only when every sample satisfies all of the following:
 *   Y BAND  max(Y) <= 0.188720 g and min(Y) > -0.444580 g, from the original
 *           MEMS Studio decision tree (converted at 4096 LSB/g)
 *   FLAT    Z inside [-1.30 g, -0.70 g]: PCB face-down, gravity on -Z
 *   LEVEL   sqrt(X^2 + Y^2) <= 0.60 g, about 37 degrees of tilt
 *   STILL   max(Z) - min(Z) <= 0.12 g: the ring is held, not moving
 * Capture mode runs the accelerometer at +/-8 g. The FLAT, LEVEL and STILL
 * limits use the datasheet sensitivity of 0.244 mg/LSB, the same factor the
 * recording app used when it wrote mems_hover_dataset.
 *
 * Tuned on mems_hover_dataset (144 hover windows from 3 sessions, 288 null
 * windows), testing every 50-sample window: the Y band alone detected 140/144
 * hover windows and fired on 56/288 null windows; the combined rule detects
 * 137/144 and fires on 39/288. Detection was unchanged for tilt 600-700 mg,
 * stillness 120-140 mg and Z max -750..-650 mg, so no limit sits on an edge.
 *
 * A complete non-hover window is required before the first trigger, and
 * after every trigger, so a stationary ring cannot repeatedly take photos. */
#define CAPTURE_HOVER_SAMPLE_RATE_HZ       120U
#define CAPTURE_HOVER_SAMPLE_PERIOD_US     \
    (1000000U / CAPTURE_HOVER_SAMPLE_RATE_HZ)
#define CAPTURE_HOVER_WINDOW_SAMPLES       50U
#define CAPTURE_HOVER_MAX_Y_RAW            773
#define CAPTURE_HOVER_MIN_Y_RAW            (-1821)
#define CAPTURE_HOVER_Z_MIN_RAW            (-5328)  /* -1.30 g */
#define CAPTURE_HOVER_Z_MAX_RAW            (-2869)  /* -0.70 g */
#define CAPTURE_HOVER_LATERAL_MAX_RAW      2459     /*  0.60 g */
#define CAPTURE_HOVER_Z_STILL_RANGE_RAW    492      /*  0.12 g peak-to-peak */
#define CAPTURE_HOVER_REARM_COOLDOWN_MS    750U

#if CAPTURE_GESTURE_SOURCE != CAPTURE_GESTURE_SOURCE_MLC && \
    CAPTURE_GESTURE_SOURCE != CAPTURE_GESTURE_SOURCE_THRESHOLD
#error "CAPTURE_GESTURE_SOURCE must select MLC or threshold"
#endif

#define EVENT_IMU_CAPTURE_GESTURE (1U << 0)

#endif
