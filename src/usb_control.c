#include "tusb.h"

#include "app_core.h"
#include "usb_control.h"

bool tud_vendor_control_xfer_cb(uint8_t rhport,
                                uint8_t stage,
                                tusb_control_request_t const *request) {
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }
    if (request->bmRequestType_bit.type != TUSB_REQ_TYPE_VENDOR) {
        return false;
    }
    if (request->bmRequestType_bit.direction != TUSB_DIR_OUT) {
        return false;
    }
    if (request->wLength != 0) {
        return false;
    }

    if (!app_core_enqueue_ep0_command(request->bRequest)) {
        return false;
    }

    return tud_control_status(rhport, request);
}
