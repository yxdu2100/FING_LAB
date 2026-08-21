#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <hal/nrf_gpio.h>
#include <hal/nrf_gpiote.h>
#include <hal/nrf_timer.h>
#include <hal/nrf_clock.h>
#include <helpers/nrfx_gppi.h> 
#include "hm01b0_init.h"
#include "camera.h"
#include <zephyr/pm/device.h>
#include <string.h>

LOG_MODULE_REGISTER(camera_module, LOG_LEVEL_INF);

#define camera_ADDR 0x24
uint32_t frame_marker = 0x12345678;
static uint8_t frame_buf[FRAME_BYTES];

/* Frame-sync tuning.
 *
 * With the HM01B0 running at 1 MHz vt_pix_clk, line_length_pck = 625:
 *   line period  ~= 625 us       (active 324 us + H-blank ~301 us)
 *   V-blank      ~= 515 ms       (823 blank lines * 625 us)
 *   frame period ~= 667 ms       (1.5 FPS)
 *
 * LINE_TIMEOUT_US   : must be  >  1 line-period + scheduler jitter
 *                              <  V-blank duration
 * VBLANK_WAIT_MS    : must be  >  V-blank duration
 * MAX_PROBE_LINES   : worst-case probes to walk through one active region
 */
#define LINE_TIMEOUT_US   2500u
#define VBLANK_WAIT_MS    700u
#define MAX_PROBE_LINES   (IMG_H + 20)

/* Throwaway buffer used to swallow lines until we detect V-blank, and
 * to capture row 0 of the next frame (see camera_capture_frame). */
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

void camera_load_regs(const struct device *i2c_dev, const struct sensor_reg *reg_list)
{
    int ret;

    for (int i = 0; ; i++)
    {
        uint16_t reg = reg_list[i].reg;
        uint8_t val  = reg_list[i].val;

        // End of list marker
        if (reg == 0xFFFF && val == 0xFF) {
            LOG_INF("Camera register init finished");
            break;
        }

        ret = camera_write_reg(i2c_dev, reg, val);
        // if (ret) {
        //     LOG_ERR("Failed to write reg 0x%04X", reg);
        // }
        k_usleep(500); // small delay to avoid SCCB timing issues
    }
}


///* ---------- MCLK INIT ---------- */
static void configure_mclk_gppi(void)
{
    nrfx_err_t err;
    uint8_t gppi_channel;

    /*  Allocate a GPPI channel */
    err = nrfx_gppi_channel_alloc(&gppi_channel);
    if (err != NRFX_SUCCESS) {
        LOG_ERR("nrfx_gppi_channel_alloc failed: %08x", err);
        return;
    }
    
    uint32_t timer_compare_evt = nrf_timer_event_address_get(NRF_TIMER_REG,
                                                             NRF_TIMER_EVENT_COMPARE0);
    

    uint32_t gpiote_task_addr = nrf_gpiote_task_address_get(NRF_GPIOTE_REG,
                                                            nrf_gpiote_out_task_get(MCLK_GPIOTE_CH));

    /*  - Connect the event to the task */
    nrfx_gppi_channel_endpoints_setup(gppi_channel, 
                                      timer_compare_evt, 
                                      gpiote_task_addr);

    /*  Enable the channel*/
    nrfx_gppi_channels_enable(BIT(gppi_channel));
    LOG_INF("GPPI (DPPI) channel %d configured", gppi_channel);
}

void mclk_init(void)
{
    LOG_INF("Starting 8MHz wave generator on P1.07 using register access + DPPI");

    /* * Configure pin as output*/
    nrf_gpio_cfg_output(MCLK_PIN);

    NRF_GPIOTE_REG->CONFIG[MCLK_GPIOTE_CH] =
                        (GPIOTE_CONFIG_MODE_Task << GPIOTE_CONFIG_MODE_Pos) |
                        (GPIOTE_CONFIG_POLARITY_Toggle << GPIOTE_CONFIG_POLARITY_Pos) |
                        (MCLK_PIN << GPIOTE_CONFIG_PSEL_Pos) |
                        (GPIOTE_CONFIG_OUTINIT_Low << GPIOTE_CONFIG_OUTINIT_Pos);
    LOG_INF("GPIOTE channel %d configured", MCLK_GPIOTE_CH);

    NRF_TIMER_REG->PRESCALER = 0;
    NRF_TIMER_REG->CC[0] = 1;  // 1 tick @ 16MHz = 16M events/sec -> 8MHz toggle
    NRF_TIMER_REG->SHORTS = (TIMER_SHORTS_COMPARE0_CLEAR_Enabled << TIMER_SHORTS_COMPARE0_CLEAR_Pos);
    LOG_INF("Timer configured");

    /* Configure the peripherals */
    configure_mclk_gppi(); // Connects TIMER and GPIOTE

    /* * Start the timer*/
    NRF_TIMER_REG->TASKS_START = 1;
    LOG_INF("Timer started. 8MHz wave should be generating.");

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

void camera_init(void)
{
    // Initialize MCLK
    mclk_init();
    // Initialize I2C
    if (!device_is_ready(i2c_dev)) {
        LOG_ERR("I2C device not ready");
        return;
    }
    // Load Camera Registers
    camera_load_regs(i2c_dev, hm01b0_324x244);

    // Initialize SPI Slave
    spi_slave_init();

    /* Initialize the per-line completion signal exactly once, not per
     * frame. Re-initializing while a transfer is pending would race
     * with the SPIS ISR. */
    k_poll_signal_init(&line_done_sig);

    LOG_INF("Camera initialization complete");
    camera_standby();
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
static int spis_arm_and_wait(uint8_t *dst, k_timeout_t timeout)
{
    struct spi_buf rx_buf = { .buf = dst, .len = IMG_W };
    struct spi_buf_set rx_set = { .buffers = &rx_buf, .count = 1 };
    struct spi_buf tx_buf = { .buf = dst, .len = IMG_W };
    struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1 };

    int ret = spi_transceive_signal(spi_slave_dev, &spi_slave_cfg,
                                    &tx_set, &rx_set, &line_done_sig);
    if (ret != 0) {
        LOG_ERR("spi_transceive_signal failed: %d", ret);
        return ret;
    }

    /* Clear any stale signal raised by the previous transfer's ISR,
     * now that the new transfer is armed but has not yet completed. */
    k_poll_signal_reset(&line_done_sig);

    struct k_poll_event evt;
    k_poll_event_init(&evt, K_POLL_TYPE_SIGNAL,
                      K_POLL_MODE_NOTIFY_ONLY, &line_done_sig);

    return k_poll(&evt, 1, timeout);
}

/* Wait for an already-armed SPIS transfer to complete, without re-arming.
 * Used after spis_arm_and_wait() times out in V-blank, to pick up the
 * row-0 capture when V-blank ends. Returns 0 on completion, -EAGAIN on
 * timeout.
 */
static int spis_wait_only(k_timeout_t timeout)
{
    struct k_poll_event evt;
    k_poll_event_init(&evt, K_POLL_TYPE_SIGNAL,
                      K_POLL_MODE_NOTIFY_ONLY, &line_done_sig);
    return k_poll(&evt, 1, timeout);
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

    /* ---------- Probe until V-blank ------------------------------- */
    bool in_vblank = false;
    int probes;
    for (probes = 0; probes < MAX_PROBE_LINES; probes++) {
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
        return -ETIMEDOUT;
    }

    /* ---------- Harvest row 0 of the new frame -------------------- */
    rc = spis_wait_only(K_MSEC(VBLANK_WAIT_MS));
    if (rc != 0) {
        LOG_WRN("Row 0 wait timed out (rc=%d); caller should retry", rc);
        return -ETIMEDOUT;
    }
    memcpy(&frame_buf[0], probe_buf, IMG_W);

    /* ---------- Capture rows 1..IMG_H-1 --------------------------- */
    for (int line = 1; line < IMG_H; line++) {
        uint8_t *line_ptr = &frame_buf[line * IMG_W];
        rc = spis_arm_and_wait(line_ptr, K_USEC(LINE_TIMEOUT_US));
        if (rc != 0) {
            /* Mid-frame stall (likely a missed line). Drop this frame;
             * next call will resync via the probe phase. */
            LOG_WRN("Line %d stalled (rc=%d), dropping frame", line, rc);
            return -EAGAIN;
        }
    }

    return FRAME_BYTES;
}


void mclk_stop(void)
{
    NRF_TIMER_REG->TASKS_STOP = 1;
    NRF_TIMER_REG->TASKS_CLEAR = 1;
    LOG_INF("MCLK Stopped");
}

void camera_standby(void)
{
    camera_write_reg(i2c_dev, 0x0100, 0x00); 
    k_msleep(10);
    mclk_stop();
    int err = pm_device_action_run(spi_slave_dev, PM_DEVICE_ACTION_SUSPEND);
    if (err) {
        LOG_ERR("Failed to suspend SPI Slave: %d", err);
    }
    LOG_INF("Camera Standby & SPI Slave Suspended");
}

uint8_t* camera_get_frame_buffer(void)
{
    return frame_buf;
}


void camera_wake_up(void)
{
    LOG_INF("Waking up Camera, MCLK & SPI Slave...");
    int err = pm_device_action_run(spi_slave_dev, PM_DEVICE_ACTION_RESUME);
    if (err) {
        LOG_ERR("Failed to resume SPI Slave: %d", err);
    }

    NRF_TIMER_REG->TASKS_START = 1;
    k_msleep(5);
    camera_write_reg(i2c_dev, 0x0100, 0x01); 
    k_msleep(50);
}
