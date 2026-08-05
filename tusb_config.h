#ifndef FAMICAS_TUSB_CONFIG_H
#define FAMICAS_TUSB_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif
#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_NONE
#endif

#define CFG_TUSB_DEBUG             0
#define CFG_TUD_ENABLED            1
#define CFG_TUD_ENDPOINT0_SIZE     64

#define CFG_TUD_CDC               2
#define CFG_TUD_MSC               0
#define CFG_TUD_HID               0
#define CFG_TUD_MIDI              0
#define CFG_TUD_VENDOR            0

#define CFG_TUD_CDC_RX_BUFSIZE    512
#define CFG_TUD_CDC_TX_BUFSIZE    512
#define CFG_TUD_CDC_EP_BUFSIZE    64

#ifdef __cplusplus
}
#endif

#endif
