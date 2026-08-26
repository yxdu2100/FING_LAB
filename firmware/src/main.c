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

    /* Preserve the proven Phase 1/2 behavior at boot. The iOS app can switch
     * to the isolated streaming profile before Phase 3 or Phase 4. */
    mode_manager_request(FING_MODE_CAPTURE);
    LOG_INF("Ready: app-selectable CAPTURE and 120 Hz STREAM modes");
    mode_manager_run();
    return 0;
}
