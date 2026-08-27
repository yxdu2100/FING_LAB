#include <zephyr/logging/log.h>

#include "ble/ble.h"
#include "modes/mode_manager.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

int main(void)
{
    LOG_INF("Starting Fing Lab multi-mode study firmware");

    int err = mode_manager_init();
    if (err != 0) {
        LOG_ERR("Mode manager initialization failed: %d", err);
        return err;
    }

    err = ble_init(mode_manager_request, mode_manager_request_resend,
                   mode_manager_acknowledge_frame);
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
