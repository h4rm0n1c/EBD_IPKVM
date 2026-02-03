#include "pico/unique_id.h"
#include "tusb.h"

#ifndef USBD_VID
#define USBD_VID (0x2E8A)
#endif

#ifndef USBD_PID
#if PICO_RP2040
#define USBD_PID (0x000a)
#else
#define USBD_PID (0x0009)
#endif
#endif

#ifndef USBD_MANUFACTURER
#define USBD_MANUFACTURER "Raspberry Pi"
#endif

#ifndef USBD_PRODUCT
#define USBD_PRODUCT "EBD_IPKVM"
#endif

#define USBD_DESC_LEN (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN + TUD_CDC_DESC_LEN * 2)
#define USBD_CONFIGURATION_DESCRIPTOR_ATTRIBUTE (TUSB_DESC_CONFIG_ATT_BUS_POWERED)
#define USBD_MAX_POWER_MA (250)

enum {
    ITF_NUM_VENDOR_STREAM = 0,
    ITF_NUM_CDC_CTRL,
    ITF_NUM_CDC_CTRL_DATA,
    ITF_NUM_CDC_ADB,
    ITF_NUM_CDC_ADB_DATA,
    ITF_NUM_TOTAL
};

#define USBD_STR_0 (0x00)
#define USBD_STR_MANUF (0x01)
#define USBD_STR_PRODUCT (0x02)
#define USBD_STR_SERIAL (0x03)
#define USBD_STR_STREAM (0x04)
#define USBD_STR_CTRL (0x05)
#define USBD_STR_ADB (0x06)

static const tusb_desc_device_t usbd_desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USBD_VID,
    .idProduct = USBD_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = USBD_STR_MANUF,
    .iProduct = USBD_STR_PRODUCT,
    .iSerialNumber = USBD_STR_SERIAL,
    .bNumConfigurations = 1,
};

static const uint8_t usbd_desc_cfg[USBD_DESC_LEN] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, USBD_STR_0, USBD_DESC_LEN,
        USBD_CONFIGURATION_DESCRIPTOR_ATTRIBUTE, USBD_MAX_POWER_MA),

    TUD_VENDOR_DESCRIPTOR(ITF_NUM_VENDOR_STREAM, USBD_STR_STREAM, 0x81, 0x01, 64),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_CTRL, USBD_STR_CTRL, 0x82, 8, 0x02, 0x83, 64),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_ADB, USBD_STR_ADB, 0x84, 8, 0x04, 0x85, 64),
};

static char usbd_serial_str[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];

static const char *const usbd_desc_str[] = {
    [USBD_STR_MANUF] = USBD_MANUFACTURER,
    [USBD_STR_PRODUCT] = USBD_PRODUCT,
    [USBD_STR_SERIAL] = usbd_serial_str,
    [USBD_STR_STREAM] = "EBD_IPKVM stream (bulk)",
    [USBD_STR_CTRL] = "EBD_IPKVM control",
    [USBD_STR_ADB] = "EBD_IPKVM adb test",
};

const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&usbd_desc_device;
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return usbd_desc_cfg;
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    static uint16_t desc_str[32];
    uint8_t chr_count = 0;

    if (index == USBD_STR_0) {
        desc_str[1] = 0x0409;
        chr_count = 1;
    } else {
        if (index == USBD_STR_SERIAL) {
            pico_get_unique_board_id_string(usbd_serial_str, sizeof(usbd_serial_str));
        }

        if (!(index < sizeof(usbd_desc_str) / sizeof(usbd_desc_str[0]))) {
            return NULL;
        }

        const char *str = usbd_desc_str[index];
        if (!str) {
            return NULL;
        }

        while (str[chr_count] && chr_count < (sizeof(desc_str) / sizeof(desc_str[0]) - 1)) {
            desc_str[1 + chr_count] = (uint16_t)str[chr_count];
            chr_count++;
        }
    }

    desc_str[0] = (TUSB_DESC_STRING << 8) | (2 * chr_count + 2);
    return desc_str;
}
