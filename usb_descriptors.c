#include <stddef.h>
#include <string.h>

#include "bsp/board_api.h"
#include "tusb.h"

#define USB_VID 0xCAFE
#define USB_PID 0x4002
#define USB_BCD 0x0200

static tusb_desc_device_t const device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = USB_BCD,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&device_descriptor;
}

enum {
    ITF_NUM_CDC_WRITER = 0,
    ITF_NUM_CDC_WRITER_DATA,
    ITF_NUM_CDC_FC_LOG,
    ITF_NUM_CDC_FC_LOG_DATA,
    ITF_NUM_TOTAL
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + (2 * TUD_CDC_DESC_LEN))

#define EPNUM_WRITER_NOTIF 0x81
#define EPNUM_WRITER_OUT   0x02
#define EPNUM_WRITER_IN    0x82
#define EPNUM_FC_LOG_NOTIF 0x83
#define EPNUM_FC_LOG_OUT   0x04
#define EPNUM_FC_LOG_IN    0x84

static uint8_t const configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_WRITER, 4,
                       EPNUM_WRITER_NOTIF, 8,
                       EPNUM_WRITER_OUT, EPNUM_WRITER_IN, 64),

    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_FC_LOG, 5,
                       EPNUM_FC_LOG_NOTIF, 8,
                       EPNUM_FC_LOG_OUT, EPNUM_FC_LOG_IN, 64),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return configuration_descriptor;
}

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_WRITER,
    STRID_FC_LOG,
};

static char const *string_descriptors[] = {
    (const char[]){0x09, 0x04},
    "FamiCAS",
    "FamiCAS Dual CDC Bridge",
    NULL,
    "FamiCAS ROM Writer",
    "FamiCAS FC printf",
};

static uint16_t string_descriptor[33];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    size_t count;

    if (index == STRID_LANGID) {
        memcpy(&string_descriptor[1], string_descriptors[0], 2);
        count = 1;
    } else if (index == STRID_SERIAL) {
        count = board_usb_get_serial(string_descriptor + 1, 32);
    } else {
        if (index >= (sizeof(string_descriptors) / sizeof(string_descriptors[0]))) {
            return NULL;
        }
        char const *text = string_descriptors[index];
        count = strlen(text);
        if (count > 32) count = 32;
        for (size_t i = 0; i < count; ++i) {
            string_descriptor[1 + i] = (uint8_t)text[i];
        }
    }

    string_descriptor[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * count + 2));
    return string_descriptor;
}
