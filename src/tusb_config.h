/*
 * TinyUSB configuration for EBD_IPKVM.
 */

#ifndef EBD_IPKVM_TUSB_CONFIG_H
#define EBD_IPKVM_TUSB_CONFIG_H

#include "pico/stdlib.h"

#ifndef CFG_TUSB_RHPORT0_MODE
#define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_DEVICE)
#endif

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE  (64)
#endif

#define CFG_TUD_CDC             (1)

#if CFG_TUD_CDC != 1
#error "EBD_IPKVM firmware supports exactly one CDC interface for control/debug."
#endif

#ifndef CFG_TUD_CDC_RX_BUFSIZE
#define CFG_TUD_CDC_RX_BUFSIZE  (TUD_OPT_HIGH_SPEED ? 512 : 64)
#endif
#ifndef CFG_TUD_CDC_TX_BUFSIZE
#define CFG_TUD_CDC_TX_BUFSIZE  (TUD_OPT_HIGH_SPEED ? 512 : 256)
#endif
#ifndef CFG_TUD_CDC_EP_BUFSIZE
#define CFG_TUD_CDC_EP_BUFSIZE  (TUD_OPT_HIGH_SPEED ? 512 : 64)
#endif

#define CFG_TUD_VENDOR          (1)

#ifndef CFG_TUD_VENDOR_RX_BUFSIZE
#define CFG_TUD_VENDOR_RX_BUFSIZE  (TUD_OPT_HIGH_SPEED ? 512 : 64)
#endif
#ifndef CFG_TUD_VENDOR_TX_BUFSIZE
#define CFG_TUD_VENDOR_TX_BUFSIZE  (TUD_OPT_HIGH_SPEED ? 512 : 64)
#endif

#endif
