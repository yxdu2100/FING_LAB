#include <zephyr/logging/log.h>
#include "ble/ble.h"
#include "modes/mode_manager.h"
#include <zephyr/toolchain.h>
#include <nrf_sys_event.h>

BUILD_ASSERT(sizeof(CONFIG_FING_BOARD_ID) > 1,
             "No ring profile selected. Build with "
             "-DEXTRA_CONF_FILE=configs/fing_NNmm.conf");
BUILD_ASSERT(CONFIG_FING_RING_SIZE_MM > 0,
             "No ring profile selected. Build with "
             "-DEXTRA_CONF_FILE=configs/fing_NNmm.conf");

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

int main(void)
{
    LOG_INF("Starting Fing Lab multi-mode study firmware");

    /* In low-power sleep the CPU was observed stuck in WFI with the GRTC
     * system-timer IRQ pending, freezing the kernel and BLE host until a
     * debugger halt woke it. Constant-latency mode keeps the wake-up path
     * powered so the CPU always leaves WFI, at the cost of some idle current. */
    int err = nrf_sys_event_request_global_constlat();
    if (err < 0) {
        LOG_ERR("Could not request constant-latency mode: %d", err);
    }

    err = mode_manager_init();
    if (err != 0) {
        LOG_ERR("Mode manager initialization failed: %d", err);
        return err;
    }

    err = ble_init(mode_manager_request, mode_manager_request_resend,
                   mode_manager_acknowledge_frame,
                   mode_manager_request_camera_recovery);
    if (err != 0) {
        LOG_ERR("BLE initialization failed: %d", err);
        return err;
    }

    /* Every ring boots in the common low-power IDLE state. The selected app
     * explicitly enters CAPTURE or STREAM after all matching notification
     * channels have been subscribed. */
    LOG_INF("Ready in IDLE: board=%s ring=%d mm firmware=%s",
            CONFIG_FING_BOARD_ID, CONFIG_FING_RING_SIZE_MM,
            CONFIG_FING_FIRMWARE_VERSION);
    mode_manager_run();
    return 0;
}
