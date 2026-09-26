#include <errno.h>
#include <stdbool.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <hal/nrf_gpio.h>
#include <hal/nrf_gpiote.h>
#include <hal/nrf_timer.h>
#include <hal/nrf_clock.h>
#include <helpers/nrfx_gppi.h> 
#include "hm01b0_init.h"
#include "camera.h"
#include <zephyr/pm/device.h>
#include <zephyr/sys/atomic.h>
#include <string.h>

LOG_MODULE_REGISTER(camera_module, LOG_LEVEL_INF);

#define camera_ADDR 0x24
uint32_t frame_marker = 0x12345678;
static uint8_t frame_buf[FRAME_BYTES];

/* Frame-sync tuning.
 *
 * With the HM01B0 running at 1 MHz vt_pix_clk, line_length_pck = 800:
 *   line period  ~= 800 us       (active 324 us + H-blank ~476 us)
 *   V-blank      ~= 28.8 ms      (36 blank lines * 800 us)
 *   frame period ~= 224.0 ms     (~4.5 FPS)
 *
 * LINE_TIMEOUT_US   : must be  >  1 line-period + scheduler jitter
 *                              <  V-blank duration
 * VBLANK_WAIT_MS    : must be  >  V-blank duration
 * MAX_PROBE_LINES   : worst-case probes to walk through one active region
 */
#define LINE_TIMEOUT_US   8000u
#define VBLANK_WAIT_MS    700u
#define MAX_PROBE_LINES   (IMG_H + 20)
#define CAMERA_STREAM_REG_RETRIES 3
#define CAMERA_STREAM_REG_RETRY_DELAY_MS 20
#define CAMERA_MCLK_SETTLE_MS 20
#define CAMERA_BOOT_SETTLE_MS 100
#define CAMERA_INIT_RETRIES 10
#define CAMERA_INIT_RETRY_DELAY_MS 100
#define CAMERA_RECOVERY_SETTLE_MS 100
#define CAMERA_RECOVERY_VERIFY_MS 600

/* Fixed SPIS EasyDMA staging buffer.
 *
 * Do not point the peripheral directly at successive rows of frame_buf.
 * Doing so makes the DMA destination change on every line.  Keeping the
 * address fixed removes that variable from the repeatable timeout observed
 * around row 161/162.  Every hardware transfer now lands at this single
 * known-working address and is copied into its final frame row only after
 * completion.  The same buffer is also used to swallow probe lines while
 * finding vertical blanking. */
static uint8_t probe_buf[IMG_W];

#define MY_SPI_SLAVE  DT_NODELABEL(my_spi_slave)
#define I2C_NODE DT_NODELABEL(i2c21)

// LVLD/CS Config (Uses Channels 1, 2, 3)， If there is an external inverter, this can be removed.
#define CS_PIN              NRF_PIN_PORT_TO_PIN_NUMBER(10U, 1) 
#define LVLD_PIN            NRF_PIN_PORT_TO_PIN_NUMBER(13U, 1) 
#define CS_GPIOTE_CH        1  // Output (CS)
#define LVLD_RISE_CH        2  // Input (Rise Event)
#define LVLD_FALL_CH        3  // Input (Fall Event)

// MCLK Config
#define MCLK_PIN NRF_PIN_PORT_TO_PIN_NUMBER(8U, 1) //pin P1.08
#define MCLK_GPIOTE_CH 0

// Timer and GPIOTE instances for MCLK generation
#define NRF_TIMER_REG NRF_TIMER22
#define NRF_GPIOTE_REG NRF_GPIOTE20

const struct device *i2c_dev = DEVICE_DT_GET(I2C_NODE);
const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));

static struct k_poll_signal line_done_sig;
const struct device *spi_slave_dev;
static struct k_poll_signal spi_slave_done_sig = K_POLL_SIGNAL_INITIALIZER(spi_slave_done_sig);
/* Zephyr keeps the SPIS context lock until an asynchronous transaction
 * completes. Calling spi_transceive_signal() again while a timed-out
 * transaction is still armed therefore blocks forever inside the driver.
 * This flag is owned by the capture thread and prevents that second arm. */
static bool spis_transfer_pending;
/* Final destination for the transfer currently armed on probe_buf.  It stays
 * valid across a timeout because frame_buf and probe_buf are static. */
static uint8_t *spis_pending_destination;
/* Packed atomically because the BLE diagnostic heartbeat reads this state
 * from the system work queue while the mode-manager thread captures rows.
 * Bits 0..7 hold the stage and bits 8..15 hold the current row. */
static atomic_t camera_diag_state;
static atomic_t camera_diag_error;
static uint8_t mclk_gppi_channel;
static bool mclk_gppi_channel_allocated;
static struct onoff_manager *camera_hfclk_manager;
static struct onoff_client camera_hfclk_client;
static bool camera_hfclk_requested;

static void mclk_stop(void);
static int camera_recover_pending_transfer(void);
static int camera_ensure_streaming(void);

static void camera_diag_set(enum camera_diagnostic_stage stage,
                            uint8_t row, int error)
{
    atomic_set(&camera_diag_error, error);
    atomic_set(&camera_diag_state,
               ((atomic_val_t)row << 8) | (atomic_val_t)stage);
}

void camera_get_diagnostic_snapshot(struct camera_diagnostic_snapshot *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    atomic_val_t packed = atomic_get(&camera_diag_state);
    snapshot->stage = (enum camera_diagnostic_stage)(packed & 0xFF);
    snapshot->row = (uint8_t)((packed >> 8) & 0xFF);
    snapshot->last_error = (int16_t)atomic_get(&camera_diag_error);
}

static const struct spi_config spi_slave_cfg = {
	.operation = SPI_WORD_SET(8) | SPI_TRANSFER_MSB | 
                //  SPI_MODE_CPHA | // Clock idle low, data captured on pclk falling edge
				 SPI_OP_MODE_SLAVE,
	.frequency = 8000000,
	.slave = 0,
};

static void spi_slave_init(void)
{
	spi_slave_dev = DEVICE_DT_GET(MY_SPI_SLAVE);
	if(!device_is_ready(spi_slave_dev)) {
		printk("SPI slave device not ready!\n");
	}
}

int camera_write_reg(const struct device *i2c_dev, uint16_t reg, uint8_t value)
{
    uint8_t tx_buf[3];
    tx_buf[0] = (uint8_t)(reg >> 8);   // High byte
    tx_buf[1] = (uint8_t)(reg & 0xFF); // Low byte
    tx_buf[2] = value;

    int ret = i2c_write(i2c_dev, tx_buf, sizeof(tx_buf), camera_ADDR);
    if (ret) {
        LOG_ERR("Failed to write reg 0x%04X (err %d)", reg, ret);
        return ret;
    }

    return 0;
}

static int camera_read_reg(uint16_t reg, uint8_t *value)
{
    uint8_t address[2] = {
        (uint8_t)(reg >> 8),
        (uint8_t)(reg & 0xFF),
    };

    if (value == NULL) {
        return -EINVAL;
    }
    return i2c_write_read(i2c_dev, camera_ADDR,
                          address, sizeof(address), value, 1);
}

int camera_log_ae_state(unsigned int burst_frame)
{
    uint8_t frame_count;
    uint8_t integration_h;
    uint8_t integration_l;
    uint8_t analog_gain;
    uint8_t digital_gain_h;
    uint8_t digital_gain_l;

    int err = camera_read_reg(0x0005, &frame_count);
    if (err == 0) {
        err = camera_read_reg(0x0202, &integration_h);
    }
    if (err == 0) {
        err = camera_read_reg(0x0203, &integration_l);
    }
    if (err == 0) {
        err = camera_read_reg(0x0205, &analog_gain);
    }
    if (err == 0) {
        err = camera_read_reg(0x020E, &digital_gain_h);
    }
    if (err == 0) {
        err = camera_read_reg(0x020F, &digital_gain_l);
    }
    if (err != 0) {
        LOG_WRN("Could not read AE state after burst frame %u: %d",
                burst_frame, err);
        return err;
    }

    uint16_t integration = ((uint16_t)integration_h << 8) | integration_l;
    uint16_t digital_gain = ((uint16_t)(digital_gain_h & 0x03U) << 8) |
                            (digital_gain_l & 0xFCU);
    LOG_INF("AE burst %u: sensor_frame=%u integration=%u rows "
            "again=0x%02x dgain=0x%03x",
            burst_frame, frame_count, integration,
            analog_gain, digital_gain);
    return 0;
}

static int camera_load_regs(const struct device *i2c_dev,
                            const struct sensor_reg *reg_list)
{
    int ret;

    for (int i = 0; ; i++)
    {
        uint16_t reg = reg_list[i].reg;
        uint8_t val  = reg_list[i].val;

        // End of list marker
        if (reg == 0xFFFF && val == 0xFF) {
            LOG_INF("Camera register init finished");
            return 0;
        }

        ret = camera_write_reg(i2c_dev, reg, val);
        if (ret != 0) {
            LOG_ERR("Camera register init stopped at index %d "
                    "(reg 0x%04X, err %d)", i, reg, ret);
            return ret;
        }
        k_usleep(500); // small delay to avoid SCCB timing issues
    }
}

///* ---------- MCLK INIT ---------- */
static int configure_mclk_gppi(void)
{
    if (!mclk_gppi_channel_allocated) {
        nrfx_err_t err = nrfx_gppi_channel_alloc(&mclk_gppi_channel);
        if (err != NRFX_SUCCESS) {
            LOG_ERR("MCLK GPPI channel allocation failed: %08x", err);
            return -ENOMEM;
        }
        mclk_gppi_channel_allocated = true;
    }
    
    uint32_t timer_compare_evt = nrf_timer_event_address_get(NRF_TIMER_REG,
                                                             NRF_TIMER_EVENT_COMPARE0);
    

    uint32_t gpiote_task_addr = nrf_gpiote_task_address_get(NRF_GPIOTE_REG,
                                                            nrf_gpiote_out_task_get(MCLK_GPIOTE_CH));

    /*  - Connect the event to the task */
    nrfx_gppi_channel_endpoints_setup(mclk_gppi_channel,
                                      timer_compare_evt, 
                                      gpiote_task_addr);

    /*  Enable the channel*/
    nrfx_gppi_channels_enable(BIT(mclk_gppi_channel));
    LOG_INF("GPPI (DPPI) channel %d configured", mclk_gppi_channel);
    return 0;
}

static int camera_hfclk_acquire(void)
{
    if (camera_hfclk_requested) {
        return 0;
    }

    camera_hfclk_manager =
        z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF);
    if (camera_hfclk_manager == NULL) {
        LOG_ERR("Cannot get HF clock manager for camera MCLK");
        return -ENXIO;
    }

    sys_notify_init_spinwait(&camera_hfclk_client.notify);
    int err = onoff_request(camera_hfclk_manager, &camera_hfclk_client);
    if (err < 0) {
        LOG_ERR("Camera HF clock request failed: %d", err);
        return err;
    }

    int result = 0;
    do {
        err = sys_notify_fetch_result(&camera_hfclk_client.notify, &result);
        if (err == -EAGAIN) {
            k_yield();
        }
    } while (err == -EAGAIN);

    if (err != 0 || result != 0) {
        LOG_ERR("Camera HF clock did not start: notify=%d result=%d",
                err, result);
        (void)onoff_cancel_or_release(camera_hfclk_manager,
                                      &camera_hfclk_client);
        return err != 0 ? err : result;
    }

    camera_hfclk_requested = true;
    LOG_INF("HF clock held for camera MCLK");
    return 0;
}

static void camera_hfclk_release(void)
{
    if (!camera_hfclk_requested) {
        return;
    }

    int err = onoff_cancel_or_release(camera_hfclk_manager,
                                      &camera_hfclk_client);
    if (err < 0) {
        LOG_WRN("Camera HF clock release failed: %d", err);
    }
    camera_hfclk_requested = false;
}

static int mclk_start(void)
{
    int err = camera_hfclk_acquire();
    if (err != 0) {
        return err;
    }

    /* Reapply the complete path on every restart. These registers are
     * controlled directly, so Zephyr cannot restore them for us if another
     * peripheral or a power transition changes their state. */
    nrf_gpio_cfg_output(MCLK_PIN);

    NRF_GPIOTE_REG->CONFIG[MCLK_GPIOTE_CH] =
                        (GPIOTE_CONFIG_MODE_Task << GPIOTE_CONFIG_MODE_Pos) |
                        (GPIOTE_CONFIG_POLARITY_Toggle << GPIOTE_CONFIG_POLARITY_Pos) |
                        (MCLK_PIN << GPIOTE_CONFIG_PSEL_Pos) |
                        (GPIOTE_CONFIG_OUTINIT_Low << GPIOTE_CONFIG_OUTINIT_Pos);
    NRF_TIMER_REG->TASKS_STOP = 1;
    NRF_TIMER_REG->TASKS_CLEAR = 1;
    NRF_TIMER_REG->PRESCALER = 0;
    NRF_TIMER_REG->CC[0] = 1;  // 1 tick @ 16MHz = 16M events/sec -> 8MHz toggle
    NRF_TIMER_REG->SHORTS = (TIMER_SHORTS_COMPARE0_CLEAR_Enabled << TIMER_SHORTS_COMPARE0_CLEAR_Pos);

    err = configure_mclk_gppi();
    if (err != 0) {
        camera_hfclk_release();
        return err;
    }

    NRF_TIMER_REG->TASKS_START = 1;
    return 0;
}

static int mclk_init(void)
{
    LOG_INF("Starting 8MHz wave generator on P1.08 using register access + DPPI");
    int err = mclk_start();
    if (err == 0) {
        LOG_INF("Timer started. 8MHz wave should be generating.");
    }
    return err;
}

/* ==========================================
 * LVLD --> Invert --> CS LOGIC. If there is an external inverter, this can be removed.
 * ========================================== */
static void configure_lvld_cs_gppi(void)
{
    uint8_t dppi_ch_rise, dppi_ch_fall;

    // Allocate DPPI Channels
    if (nrfx_gppi_channel_alloc(&dppi_ch_rise) != NRFX_SUCCESS) return;
    if (nrfx_gppi_channel_alloc(&dppi_ch_fall) != NRFX_SUCCESS) return;

    // Get Event Addresses (From LVLD Input Channels 2 & 3)
    uint32_t evt_rise = nrf_gpiote_event_address_get(NRF_GPIOTE_REG, nrf_gpiote_in_event_get(LVLD_RISE_CH));
    uint32_t evt_fall = nrf_gpiote_event_address_get(NRF_GPIOTE_REG, nrf_gpiote_in_event_get(LVLD_FALL_CH));

    // Get Task Addresses (For CS Output Channel 1)
    uint32_t task_clr = nrf_gpiote_task_address_get(NRF_GPIOTE_REG, nrf_gpiote_clr_task_get(CS_GPIOTE_CH));
    uint32_t task_set = nrf_gpiote_task_address_get(NRF_GPIOTE_REG, nrf_gpiote_set_task_get(CS_GPIOTE_CH));

    // Link them (Rise -> Clear, Fall -> Set)
    nrfx_gppi_channel_endpoints_setup(dppi_ch_rise, evt_rise, task_clr);
    nrfx_gppi_channel_endpoints_setup(dppi_ch_fall, evt_fall, task_set);

    // Enable DPPI
    nrfx_gppi_channels_enable(BIT(dppi_ch_rise) | BIT(dppi_ch_fall));
}

void lvld_cs_init(void)
{
    // Configure Hardware Pins
    nrf_gpio_cfg_output(CS_PIN);
    nrf_gpio_cfg_input(LVLD_PIN, NRF_GPIO_PIN_NOPULL);

    // Configure GPIOTE for CS (Channel 1)
    // Use NRF_GPIOTE_REG to target NRF_GPIOTE20
    NRF_GPIOTE_REG->CONFIG[CS_GPIOTE_CH] =
            (GPIOTE_CONFIG_MODE_Task << GPIOTE_CONFIG_MODE_Pos) |
            (GPIOTE_CONFIG_POLARITY_Toggle << GPIOTE_CONFIG_POLARITY_Pos) | 
            (CS_PIN << GPIOTE_CONFIG_PSEL_Pos) |
            (GPIOTE_CONFIG_OUTINIT_High << GPIOTE_CONFIG_OUTINIT_Pos);

    // Configure GPIOTE for LVLD Rise (Channel 2)
    NRF_GPIOTE_REG->CONFIG[LVLD_RISE_CH] =
            (GPIOTE_CONFIG_MODE_Event << GPIOTE_CONFIG_MODE_Pos) |
            (LVLD_PIN << GPIOTE_CONFIG_PSEL_Pos) |
            (GPIOTE_CONFIG_POLARITY_LoToHi << GPIOTE_CONFIG_POLARITY_Pos);

    // Configure GPIOTE for LVLD Fall (Channel 3)
    NRF_GPIOTE_REG->CONFIG[LVLD_FALL_CH] =
            (GPIOTE_CONFIG_MODE_Event << GPIOTE_CONFIG_MODE_Pos) |
            (LVLD_PIN << GPIOTE_CONFIG_PSEL_Pos) |
            (GPIOTE_CONFIG_POLARITY_HiToLo << GPIOTE_CONFIG_POLARITY_Pos);

    // Connect via DPPI
    configure_lvld_cs_gppi();
}

int camera_init(void)
{
    int ret;

    // Initialize MCLK
    ret = mclk_init();
    if (ret != 0) {
        return ret;
    }
    // Initialize I2C
    if (!device_is_ready(i2c_dev)) {
        LOG_ERR("I2C device not ready");
        return -ENODEV;
    }

    /* Set up the MCU-side capture endpoint even if the sensor is temporarily
     * absent.  A later CAPTURE-mode request can then retry the cold wake
     * without dereferencing an uninitialized SPI device. */
    spi_slave_init();
    if (!device_is_ready(spi_slave_dev)) {
        LOG_ERR("SPI slave device not ready");
        mclk_stop();
        return -ENODEV;
    }
    k_poll_signal_init(&line_done_sig);
    camera_diag_set(CAMERA_DIAG_IDLE, 0U, 0);

    /* The integrated firmware reaches main() much sooner than the original
     * camera-only firmware.  Give both the camera rails and the newly-started
     * MCLK time to settle before the first SCCB/I2C transaction. */
    k_msleep(CAMERA_BOOT_SETTLE_MS);

    // Load Camera Registers
    for (int attempt = 1; attempt <= CAMERA_INIT_RETRIES; ++attempt) {
        ret = camera_load_regs(i2c_dev, hm01b0_324x244);
        if (ret == 0) {
            break;
        }

        LOG_WRN("Camera register init attempt %d/%d failed: %d",
                attempt, CAMERA_INIT_RETRIES, ret);
        if (attempt < CAMERA_INIT_RETRIES) {
            k_msleep(CAMERA_INIT_RETRY_DELAY_MS);
        }
    }
    if (ret != 0) {
        LOG_ERR("Camera at I2C address 0x%02x never acknowledged; "
                "check 1.8 V/DVDD, MCLK P1.08, and the flex connector",
                camera_ADDR);
        mclk_stop();
        (void)pm_device_action_run(spi_slave_dev, PM_DEVICE_ACTION_SUSPEND);
        return ret;
    }

    LOG_INF("Camera initialization complete");
    ret = camera_standby();
    if (ret != 0) {
        LOG_ERR("Initial camera standby failed: %d", ret);
        return ret;
    }

    return 0;
}


/* Arm a one-line SPIS transfer into `dst` and wait up to `timeout` for it.
 *
 * Returns:
 *   0         - line received successfully
 *   -EAGAIN   - timeout: no CS edge arrived (most likely V-blank)
 *   <0 other  - hard error from spi_transceive_signal
 *
 * On -EAGAIN the transfer is NOT aborted; the SPIS hardware is still
 * armed waiting for the next CS edge. That is intentional: the caller
 * can follow up with spis_wait_only() to harvest that pending transfer
 * as the first line of the next frame.
 */
static void spis_complete_pending_transfer(void)
{
    if (spis_pending_destination != NULL &&
        spis_pending_destination != probe_buf) {
        memcpy(spis_pending_destination, probe_buf, IMG_W);
    }
    spis_pending_destination = NULL;
    spis_transfer_pending = false;
}

static int spis_arm_and_wait(uint8_t *dst, k_timeout_t timeout)
{
    /* Keep EasyDMA on one fixed, aligned address.  Copying 324 bytes after a
     * completed line easily fits inside the HM01B0 horizontal blanking time. */
    struct spi_buf rx_buf = { .buf = probe_buf, .len = IMG_W };
    struct spi_buf_set rx_set = { .buffers = &rx_buf, .count = 1 };
    struct spi_buf tx_buf = { .buf = probe_buf, .len = IMG_W };
    struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1 };

    if (spis_transfer_pending) {
        LOG_ERR("Refusing to arm SPIS while a previous transfer is pending");
        return -EBUSY;
    }

    /* Reset before arming. Resetting after the asynchronous call can erase a
     * completion that arrives between spi_transceive_signal() and reset(),
     * permanently shifting the line pipeline. */
    k_poll_signal_reset(&line_done_sig);

    int ret = spi_transceive_signal(spi_slave_dev, &spi_slave_cfg,
                                    &tx_set, &rx_set, &line_done_sig);
    if (ret != 0) {
        LOG_ERR("spi_transceive_signal failed: %d", ret);
        return ret;
    }
    spis_pending_destination = dst;
    spis_transfer_pending = true;

    struct k_poll_event evt;
    k_poll_event_init(&evt, K_POLL_TYPE_SIGNAL,
                      K_POLL_MODE_NOTIFY_ONLY, &line_done_sig);

    ret = k_poll(&evt, 1, timeout);
    if (ret == 0) {
        spis_complete_pending_transfer();
    }
    return ret;
}

/* Wait for an already-armed SPIS transfer to complete, without re-arming.
 * Used after spis_arm_and_wait() times out in V-blank, to pick up the
 * row-0 capture when V-blank ends. Returns 0 on completion, -EAGAIN on
 * timeout.
 */
static int spis_wait_only(k_timeout_t timeout)
{
    if (!spis_transfer_pending) {
        LOG_ERR("SPIS wait requested without an armed transfer");
        return -EINVAL;
    }

    struct k_poll_event evt;
    k_poll_event_init(&evt, K_POLL_TYPE_SIGNAL,
                      K_POLL_MODE_NOTIFY_ONLY, &line_done_sig);
    int ret = k_poll(&evt, 1, timeout);
    if (ret == 0) {
        spis_complete_pending_transfer();
    }
    return ret;
}

/* Finish an asynchronous line transaction that timed out while armed.  The
 * next LVLD/CS edge completes it, normally at row 0 after vertical blanking.
 * Starting another transfer before this drain completes leaves Zephyr's SPIS
 * context and the camera row pipeline permanently out of phase. */
static int spis_drain_timed_out_transfer(void)
{
    camera_diag_set(CAMERA_DIAG_DRAINING_TRANSFER, 0U, -EAGAIN);
    int rc = spis_wait_only(K_MSEC(VBLANK_WAIT_MS));
    if (rc != 0) {
        LOG_ERR("Timed-out SPIS transfer did not drain: %d", rc);
        camera_diag_set(CAMERA_DIAG_ERROR, 0U, rc);
        return rc;
    }

    LOG_INF("Timed-out SPIS transfer drained; next attempt will re-sync");
    return 0;
}

/* Capture a single IMG_WxIMG_H frame into frame_buf, frame-aligned.
 *
 * Strategy:
 *   A. Probe: arm throwaway SPIS transfers with a short per-line timeout.
 *      A completion means we are still inside a frame's active region.
 *      A timeout means no CS arrived within ~4 line-periods, which can
 *      only happen in V-blank. At that point, SPIS is still armed, so
 *      the NEXT CS edge is guaranteed to be row 0 of the next frame.
 *   B. Harvest: wait (long timeout) for the pending transfer to finish.
 *      Copy its data into frame_buf[row 0].
 *   C. Capture rows 1..IMG_H-1 normally, each with a per-line timeout.
 *      Any per-line timeout in this phase means we lost mid-frame sync
 *      (a line was missed and the loop ran off into V-blank). The frame
 *      is discarded (-EAGAIN) and the caller should simply retry.
 *
 * Returns:
 *   FRAME_BYTES   - a clean, aligned frame is in frame_buf
 *   -EAGAIN       - frame misaligned / line missed, caller should drop
 *   -ETIMEDOUT    - sensor appears stuck; caller may wait and retry
 *   -EIO          - unrecoverable SPI error
 */
int camera_capture_frame(void)
{
    int rc;

    /* A previous attempt may have timed out while the asynchronous SPIS
     * transaction was still waiting for LVLD/CS. Restart the signal source and
     * finish that transaction before entering spi_transceive_signal(); otherwise
     * Zephyr waits forever on its internal context lock. */
    if (spis_transfer_pending) {
        LOG_WRN("Recovering SPIS transaction left by the previous attempt");
        rc = camera_recover_pending_transfer();
        if (rc != 0) {
            return -ETIMEDOUT;
        }
    }

    /* The RTT trace showed MODE_SELECT returning to standby while capture
     * mode was still active. Verify/restart the sensor before arming an async
     * SPIS request, because a request armed against a stopped sensor cannot
     * be cancelled safely through Zephyr's public SPI API. */
    rc = camera_ensure_streaming();
    if (rc != 0) {
        camera_diag_set(CAMERA_DIAG_ERROR, 0U, rc);
        return -ETIMEDOUT;
    }

    camera_diag_set(CAMERA_DIAG_FRAME_START, 0U, 0);

    /* ---------- Probe until V-blank ------------------------------- */
    bool in_vblank = false;
    int probes;
    for (probes = 0; probes < MAX_PROBE_LINES; probes++) {
        camera_diag_set(CAMERA_DIAG_SEARCHING_VBLANK,
                        (uint8_t)MIN(probes, UINT8_MAX), 0);
        rc = spis_arm_and_wait(probe_buf, K_USEC(LINE_TIMEOUT_US));
        if (rc == -EAGAIN) {
            /* No CS for > LINE_TIMEOUT_US -> we are in V-blank.
             * The SPIS is still armed on probe_buf for the next CS. */
            in_vblank = true;
            break;
        }
        if (rc != 0) {
            return -EIO;
        }
        /* rc == 0: a line completed, keep probing */
    }

    if (!in_vblank) {
        LOG_WRN("V-blank not found in %d probes; caller should retry", probes);
        camera_diag_set(CAMERA_DIAG_ERROR, 0U, -ETIMEDOUT);
        return -ETIMEDOUT;
    }

    /* ---------- Harvest row 0 of the new frame -------------------- */
    camera_diag_set(CAMERA_DIAG_WAITING_ROW_ZERO, 0U, 0);
    rc = spis_wait_only(K_MSEC(VBLANK_WAIT_MS));
    if (rc != 0) {
        LOG_WRN("Row 0 wait timed out (rc=%d); caller should retry", rc);
        int recover_rc = camera_recover_pending_transfer();
        camera_diag_set(CAMERA_DIAG_ERROR, 0U,
                        recover_rc == 0 ? -EAGAIN : recover_rc);
        return recover_rc == 0 ? -EAGAIN : -ETIMEDOUT;
    }
    memcpy(&frame_buf[0], probe_buf, IMG_W);

    /* ---------- Capture rows 1..IMG_H-1 --------------------------- */
    for (int line = 1; line < IMG_H; line++) {
        camera_diag_set(CAMERA_DIAG_CAPTURING_ROWS, (uint8_t)line, 0);
        uint8_t *line_ptr = &frame_buf[line * IMG_W];
        rc = spis_arm_and_wait(line_ptr, K_USEC(LINE_TIMEOUT_US));
        if (rc != 0) {
            LOG_WRN("Line %d stalled (rc=%d), dropping frame", line, rc);
            if (rc == -EAGAIN) {
                /* spis_arm_and_wait() deliberately leaves a timed-out
                 * transaction armed. Drain it before returning so the retry
                 * is independent instead of inheriting a stale row. */
                int drain_rc = spis_drain_timed_out_transfer();
                if (drain_rc != 0) {
                    drain_rc = camera_recover_pending_transfer();
                }
                if (drain_rc == 0) {
                    camera_diag_set(CAMERA_DIAG_ERROR, (uint8_t)line,
                                    -EAGAIN);
                    return -EAGAIN;
                }
                return -ETIMEDOUT;
            }
            camera_diag_set(CAMERA_DIAG_ERROR, (uint8_t)line, -EIO);
            return -EIO;
        }
    }

    camera_diag_set(CAMERA_DIAG_FRAME_COMPLETE, IMG_H, 0);
    return FRAME_BYTES;
}


static void mclk_stop(void)
{
    NRF_TIMER_REG->TASKS_STOP = 1;
    NRF_TIMER_REG->TASKS_CLEAR = 1;
    camera_hfclk_release();
    LOG_INF("MCLK Stopped");
}

int camera_standby(void)
{
    int first_err = 0;

    /* Give a pending line its final opportunity to complete while the camera
     * is still producing LVLD/CS. Never clear its completion signal while the
     * Zephyr SPIS driver still owns the transaction. */
    if (spis_transfer_pending) {
        LOG_WRN("Recovering pending SPIS transfer before camera standby");
        int recover_err = camera_recover_pending_transfer();
        if (recover_err != 0) {
            first_err = recover_err;
        }
    }

    int err = camera_write_reg(i2c_dev, 0x0100, 0x00);
    if (err) {
        LOG_ERR("Camera standby stream-off failed: %d", err);
        if (first_err == 0) {
            first_err = err;
        }
    }
    k_msleep(10);
    mclk_stop();
    if (!spis_transfer_pending) {
        k_poll_signal_reset(&line_done_sig);
    }
    if (!spis_transfer_pending) {
        err = pm_device_action_run(spi_slave_dev, PM_DEVICE_ACTION_SUSPEND);
        if (err) {
            LOG_ERR("Failed to suspend SPI Slave: %d", err);
            if (first_err == 0) {
                first_err = err;
            }
        }
    } else {
        /* Suspending SPIS cannot cancel an asynchronous Zephyr transaction;
         * it only removes the pins and makes recovery impossible. Keep SPIS
         * alive so the next camera wake can finish the pending line. */
        LOG_ERR("Leaving SPI Slave active because a transfer is still pending");
    }
    LOG_INF("Camera Standby; SPI Slave %s",
            spis_transfer_pending ? "kept active" : "suspended");
    camera_diag_set(CAMERA_DIAG_IDLE, 0U, first_err);
    return first_err;
}

uint8_t* camera_get_frame_buffer(void)
{
    return frame_buf;
}


static int camera_start_streaming_with_retry(void)
{
    int err = 0;

    for (int attempt = 1; attempt <= CAMERA_STREAM_REG_RETRIES; attempt++) {
        err = camera_write_reg(i2c_dev, 0x0100, 0x01);
        if (err == 0) {
            if (attempt > 1) {
                LOG_INF("Camera stream-on succeeded after %d attempts", attempt);
            }
            return 0;
        }

        LOG_WRN("Camera stream-on attempt %d/%d failed: %d",
                attempt, CAMERA_STREAM_REG_RETRIES, err);
        k_msleep(CAMERA_STREAM_REG_RETRY_DELAY_MS);
    }

    return err;
}

static int camera_ensure_streaming(void)
{
    uint8_t mode = 0xFFU;
    int err = camera_read_reg(0x0100, &mode);

    if (err == 0 && mode == 0x01U) {
        return 0;
    }

    LOG_WRN("Camera was not streaming before capture: mode=0x%02x err=%d",
            mode, err);
    err = mclk_start();
    if (err == 0) {
        k_msleep(CAMERA_MCLK_SETTLE_MS);
        err = camera_start_streaming_with_retry();
    }
    if (err != 0) {
        return err;
    }

    k_msleep(CAMERA_RECOVERY_SETTLE_MS);
    err = camera_read_reg(0x0100, &mode);
    if (err != 0) {
        return err;
    }
    if (mode != 0x01U) {
        LOG_ERR("Camera rejected stream-on before capture: mode=0x%02x",
                mode);
        return -EIO;
    }
    return 0;
}

/* Zephyr's asynchronous SPIS API does not expose a safe cancellation call.
 * A line transaction that times out therefore still owns the driver context.
 * Recover by restoring the clock and restarting the sensor so that a genuine
 * camera line completes that transaction and releases the driver normally.
 */
static int camera_recover_pending_transfer(void)
{
    if (!spis_transfer_pending) {
        return 0;
    }

    camera_diag_set(CAMERA_DIAG_DRAINING_TRANSFER, 0U, -EAGAIN);
    LOG_WRN("Camera recovery: restoring MCLK and restarting sensor stream");

    int err = mclk_start();
    if (err != 0) {
        LOG_ERR("Camera recovery could not restore MCLK: %d", err);
        return err;
    }
    k_msleep(CAMERA_MCLK_SETTLE_MS);

    uint8_t mode_before = 0xFFU;
    uint8_t frame_before = 0xFFU;
    (void)camera_read_reg(0x0100, &mode_before);
    (void)camera_read_reg(0x0005, &frame_before);

    /* A clean standby-to-stream transition is enough for transient sensor
     * stalls and preserves the current AE state. */
    err = camera_write_reg(i2c_dev, 0x0100, 0x00);
    if (err == 0) {
        k_msleep(10);
        err = camera_start_streaming_with_retry();
    }
    if (err == 0) {
        k_msleep(CAMERA_RECOVERY_SETTLE_MS);
        int drain_err = spis_wait_only(K_MSEC(VBLANK_WAIT_MS));
        if (drain_err == 0) {
            uint8_t frame_after = 0xFFU;
            (void)camera_read_reg(0x0005, &frame_after);
            LOG_INF("Camera recovery succeeded: mode=0x%02x frame=%u->%u",
                    mode_before, frame_before, frame_after);
            return 0;
        }
        LOG_WRN("Stream restart produced no SPIS line: %d", drain_err);
    } else {
        LOG_WRN("Camera stream restart failed during recovery: %d", err);
    }

    /* If the sensor lost configuration or browned out, reload the complete
     * known-good table. It ends with MODE_SELECT=1, so the still-pending SPIS
     * request can complete as soon as the first new line is emitted. */
    LOG_WRN("Camera recovery: reloading complete register table");
    err = camera_load_regs(i2c_dev, hm01b0_324x244);
    if (err != 0) {
        LOG_ERR("Camera register reload failed during recovery: %d", err);
        return err;
    }
    k_msleep(CAMERA_RECOVERY_SETTLE_MS);

    err = spis_wait_only(K_MSEC(VBLANK_WAIT_MS));
    if (err != 0) {
        uint8_t mode_after = 0xFFU;
        uint8_t frame_after = 0xFFU;
        int mode_err = camera_read_reg(0x0100, &mode_after);
        int frame_err = camera_read_reg(0x0005, &frame_after);
        LOG_ERR("Camera recovery failed: SPIS=%d mode=0x%02x(%d) "
                "frame=%u(%d)", err, mode_after, mode_err,
                frame_after, frame_err);
        camera_diag_set(CAMERA_DIAG_ERROR, 0U, err);
        return -ETIMEDOUT;
    }

    uint8_t frame_after = 0xFFU;
    (void)camera_read_reg(0x0005, &frame_after);
    LOG_INF("Camera recovery succeeded after register reload: frame=%u->%u",
            frame_before, frame_after);
    return 0;
}

/* Put the sensor back at a clean frame boundary after a complete capture
 * burst has exhausted its sync retries.  This is intentionally separate from
 * camera_recover_pending_transfer(): a drained transaction can release the
 * Zephyr driver correctly while the HM01B0 stream itself is still out of the
 * state expected by the next trigger. */
int camera_recover_stream(void)
{
    if (spis_transfer_pending) {
        return camera_recover_pending_transfer();
    }

    LOG_WRN("Camera recovery: restarting stream after capture failure");
    int err = mclk_start();
    if (err != 0) {
        return err;
    }
    k_msleep(CAMERA_MCLK_SETTLE_MS);

    uint8_t frame_before = 0xFFU;
    (void)camera_read_reg(0x0005, &frame_before);

    err = camera_write_reg(i2c_dev, 0x0100, 0x00);
    if (err == 0) {
        k_msleep(10);
        err = camera_start_streaming_with_retry();
    }
    if (err != 0) {
        LOG_WRN("Camera stream restart failed; reloading register table: %d",
                err);
        err = camera_load_regs(i2c_dev, hm01b0_324x244);
    }
    if (err != 0) {
        LOG_ERR("Camera recovery after capture failure failed: %d", err);
        return err;
    }

    k_msleep(CAMERA_RECOVERY_SETTLE_MS);
    uint8_t frame_after = 0xFFU;
    (void)camera_read_reg(0x0005, &frame_after);
    LOG_INF("Camera stream recovered after capture failure: frame=%u->%u",
            frame_before, frame_after);
    return 0;
}

int camera_recover_pipeline(void)
{
    LOG_WRN("Manual camera recovery: refreshing capture pipeline");
    camera_diag_set(CAMERA_DIAG_DRAINING_TRANSFER, 0U, 0);

    int err = mclk_start();
    if (err != 0) {
        camera_diag_set(CAMERA_DIAG_ERROR, 0U, err);
        return err;
    }

    err = pm_device_action_run(spi_slave_dev, PM_DEVICE_ACTION_RESUME);
    if (err != 0 && err != -EALREADY) {
        LOG_ERR("Manual recovery could not resume SPI Slave: %d", err);
        camera_diag_set(CAMERA_DIAG_ERROR, 0U, err);
        return err;
    }
    k_msleep(CAMERA_MCLK_SETTLE_MS);

    /* Zephyr's SPIS context must be released by a real sensor transaction.
     * Generate clocked frames first; only reset the peripheral after the
     * outstanding callback has completed and released its buffers. */
    if (spis_transfer_pending) {
        err = camera_recover_pending_transfer();
        if (err != 0) {
            LOG_ERR("Manual recovery could not drain pending SPIS: %d", err);
            camera_diag_set(CAMERA_DIAG_ERROR, 0U, err);
            return err;
        }
    }

    err = camera_write_reg(i2c_dev, 0x0100, 0x00);
    if (err != 0) {
        camera_diag_set(CAMERA_DIAG_ERROR, 0U, err);
        return err;
    }
    k_msleep(10);

    /* With no asynchronous transfer outstanding, suspend/resume restores the
     * SPIS pins and hardware to the same state as a fresh CAPTURE entry. */
    err = pm_device_action_run(spi_slave_dev, PM_DEVICE_ACTION_SUSPEND);
    if (err != 0 && err != -EALREADY) {
        LOG_ERR("Manual recovery could not suspend SPI Slave: %d", err);
        camera_diag_set(CAMERA_DIAG_ERROR, 0U, err);
        return err;
    }
    k_msleep(10);
    err = pm_device_action_run(spi_slave_dev, PM_DEVICE_ACTION_RESUME);
    if (err != 0 && err != -EALREADY) {
        LOG_ERR("Manual recovery could not restore SPI Slave: %d", err);
        camera_diag_set(CAMERA_DIAG_ERROR, 0U, err);
        return err;
    }
    k_poll_signal_reset(&line_done_sig);
    spis_pending_destination = NULL;

    /* The table starts with the HM01B0 software reset and ends in streaming
     * mode. A complete rewrite prevents stale AE/timing state from surviving
     * an operator-requested refresh. */
    err = camera_load_regs(i2c_dev, hm01b0_324x244);
    if (err != 0) {
        LOG_ERR("Manual recovery register reload failed: %d", err);
        camera_diag_set(CAMERA_DIAG_ERROR, 0U, err);
        return err;
    }
    k_msleep(CAMERA_RECOVERY_SETTLE_MS);

    err = camera_ensure_streaming();
    if (err != 0) {
        LOG_ERR("Manual recovery could not start sensor stream: %d", err);
        camera_diag_set(CAMERA_DIAG_ERROR, 0U, err);
        return err;
    }

    uint8_t frame_before = 0xFFU;
    uint8_t frame_after = 0xFFU;
    err = camera_read_reg(0x0005, &frame_before);
    if (err == 0) {
        k_msleep(CAMERA_RECOVERY_VERIFY_MS);
        err = camera_read_reg(0x0005, &frame_after);
    }
    if (err != 0 || frame_after == frame_before) {
        int result = err != 0 ? err : -ETIMEDOUT;
        LOG_ERR("Manual recovery frame verification failed: %u->%u err=%d",
                frame_before, frame_after, err);
        camera_diag_set(CAMERA_DIAG_ERROR, 0U, result);
        return result;
    }

    camera_diag_set(CAMERA_DIAG_IDLE, 0U, 0);
    LOG_INF("Manual camera recovery complete: frame=%u->%u",
            frame_before, frame_after);
    return 0;
}

int camera_wake_up(void)
{
    LOG_INF("Waking up Camera, MCLK & SPI Slave...");
    int err = pm_device_action_run(spi_slave_dev, PM_DEVICE_ACTION_RESUME);
    if (err != 0 && err != -EALREADY) {
        LOG_ERR("Failed to resume SPI Slave: %d", err);
        return err;
    }

    err = mclk_start();
    if (err != 0) {
        LOG_ERR("Failed to restore camera MCLK: %d", err);
        return err;
    }
    k_msleep(CAMERA_MCLK_SETTLE_MS);

    err = camera_start_streaming_with_retry();
    if (err != 0) {
        LOG_WRN("Camera stream-on failed after retries, reloading registers");
        err = camera_load_regs(i2c_dev, hm01b0_324x244);
        if (err != 0) {
            LOG_ERR("Camera register reload failed during wake: %d", err);
            return err;
        }

        err = camera_start_streaming_with_retry();
        if (err != 0) {
            LOG_ERR("Camera stream-on still failed after register reload: %d", err);
            return err;
        }
    }

    k_msleep(100);
    err = camera_ensure_streaming();
    if (err != 0) {
        LOG_ERR("Camera did not remain streaming after wake: %d", err);
        return err;
    }
    LOG_INF("Camera wake complete");
    return 0;
}
