#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>

#include "bsp/board_api.h"
#include "hardware/gpio.h"
#include "hardware/regs/uart.h"
#include "hardware/uart.h"
#include "pico/bootrom.h"
#include "pico/time.h"
#include "tusb.h"

// USB CDC 0: ROMWRITER_Host <-> UART0 <-> ROMWRITER_handmade
#define WRITER_CDC       0
#define WRITER_UART      uart0
#define WRITER_TX_PIN    12
#define WRITER_RX_PIN    13
#define WRITER_BAUD      921600u

// USB CDC 1: FC $4016 bit 0 soft-UART -> UART1 RX -> terminal
#define FC_LOG_CDC       1
#define FC_LOG_UART      uart1
#define FC_LOG_RX_PIN    9
#define FC_LOG_BAUD      300000u

// Controller-style Pico -> FC reply path on the expansion connector:
//   pin 13 DATA  <- GPIO8
//   pin 14 CLOCK -> GPIO10
// The FC presents CLOCK automatically whenever it reads $4016. GPIO9 remains
// dedicated to the 300 kbit/s command/printf stream on pin 12 P/S.
#define FC_REPLY_DATA_PIN  8
#define FC_REPLY_CLOCK_PIN 10
#define FC_TEST_REPLY_SIZE 16u
#define FC_RAMGO_MAX_SIZE  1024u
#define FC_RAMGO_HEADER_SIZE 8u
#define FC_REPLY_BUFFER_SIZE (FC_RAMGO_HEADER_SIZE + FC_RAMGO_MAX_SIZE)

#define RING_SIZE        4096u
#define RING_MASK        (RING_SIZE - 1u)
#define USB_CHUNK_SIZE   64u

typedef struct {
    uint8_t data[RING_SIZE];
    uint32_t read_index;
    uint32_t write_index;
    uint32_t overflow_count;
} byte_ring_t;

static byte_ring_t writer_usb_to_uart;
static byte_ring_t writer_uart_to_usb;
static byte_ring_t fc_log_uart_to_usb;
static uint32_t fc_log_rx_count;
static uint32_t fc_log_error_count;
static volatile uint32_t fc_log_edge_count;
static volatile uint32_t fc_reply_clock_edge_count;
static uint32_t fc_log_baud = FC_LOG_BAUD;
static uint64_t fc_log_next_report_us;
static volatile bool bootsel_requested;
static volatile bool fc_reply_active;
static volatile uint32_t fc_reply_bit_index;
static volatile uint32_t fc_reply_bit_count;
static volatile uint32_t fc_reply_completed_count;
static volatile uint32_t fc_reply_kind;
static uint8_t fc_reply_buffer[FC_REPLY_BUFFER_SIZE];
static uint32_t fc_hdx_command_match_index;
static uint32_t fc_ram_command_match_index;

static uint8_t fc_ramgo_frame[FC_REPLY_BUFFER_SIZE];
static uint8_t fc_ramgo_host_frame[FC_REPLY_BUFFER_SIZE];
static volatile bool fc_ramgo_pending;
static uint16_t fc_ramgo_length;
static uint32_t fc_ramgo_upload_count;
static uint32_t fc_ramgo_upload_error_count;
static volatile uint32_t fc_ramgo_generation;
static volatile uint32_t fc_reply_ramgo_generation;
static uint32_t fc_ramgo_host_state;
static uint32_t fc_ramgo_host_magic_index;
static uint32_t fc_ramgo_host_meta_index;
static uint32_t fc_ramgo_host_payload_index;
static uint8_t fc_ramgo_host_meta[4];
static uint16_t fc_ramgo_host_length;
static uint16_t fc_ramgo_host_expected_crc;
static uint16_t fc_ramgo_host_running_crc;

enum {
    FC_REPLY_KIND_NONE = 0,
    FC_REPLY_KIND_HDX = 1,
    FC_REPLY_KIND_RAMGO = 2,
};

static const uint8_t fc_test_reply_data[FC_TEST_REPLY_SIZE] = {
    0x55, 0xAA, 0x00, 0xFF, 0x01, 0x02, 0x04, 0x08,
    0x10, 0x20, 0x40, 0x80, 0xC3, 0x3C, 0x5A, 0xA5,
};

static const uint8_t fc_hdx_command[] = {'H', 'D', 'X', '?', '\r', '\n'};
static const uint8_t fc_ram_command[] = {'R', 'A', 'M', '?', '\r', '\n'};
static const uint8_t fc_ramgo_magic[] = {'F', 'C', 'R', 'G'};
static const uint8_t fc_ramgo_empty[FC_RAMGO_HEADER_SIZE] = {
    'N', 'O', 'N', 'E', 0, 0, 0, 0,
};

static inline uint32_t ring_count(const byte_ring_t *ring) {
    return ring->write_index - ring->read_index;
}

static inline uint32_t ring_space(const byte_ring_t *ring) {
    return RING_SIZE - ring_count(ring);
}

static inline bool ring_push(byte_ring_t *ring, uint8_t value) {
    if (ring_space(ring) == 0) {
        ring->overflow_count++;
        return false;
    }
    ring->data[ring->write_index & RING_MASK] = value;
    ring->write_index++;
    return true;
}

static inline uint8_t ring_peek(const byte_ring_t *ring, uint32_t offset) {
    return ring->data[(ring->read_index + offset) & RING_MASK];
}

static inline void ring_discard(byte_ring_t *ring, uint32_t count) {
    ring->read_index += count;
}

static void uart_rx_to_ring(uart_inst_t *uart, byte_ring_t *ring) {
    while (uart_is_readable(uart)) {
        if (!ring_push(ring, (uint8_t)uart_getc(uart))) {
            // Continue reading to clear the hardware FIFO. The overflow
            // counter remains available to a debugger.
        }
    }
}

static uint16_t crc16_ccitt_update(uint16_t crc, uint8_t value) {
    crc ^= (uint16_t)value << 8u;
    for (uint32_t i = 0; i < 8u; ++i) {
        crc = (crc & 0x8000u) ? (uint16_t)((crc << 1u) ^ 0x1021u)
                              : (uint16_t)(crc << 1u);
    }
    return crc;
}

static bool fc_arm_reply(const uint8_t *data, uint32_t length,
                         uint32_t kind) {
    if (fc_reply_active || (length == 0u) ||
        (length > FC_REPLY_BUFFER_SIZE)) {
        return false;
    }

    for (uint32_t i = 0; i < length; ++i) fc_reply_buffer[i] = data[i];
    fc_reply_kind = kind;
    fc_reply_bit_index = 0;
    fc_reply_bit_count = length * 8u;
    // Expansion pin 13 is /D1 (active-low), so drive the complement of the
    // logical reply bit. The FC then reads the original value at $4016 bit 1.
    gpio_put(FC_REPLY_DATA_PIN, (fc_reply_buffer[0] & 1u) ^ 1u);
    fc_reply_active = true;
    return true;
}

static bool match_command(uint8_t value, const uint8_t *command,
                          uint32_t length, uint32_t *index) {
    if (value == command[*index]) {
        (*index)++;
        if (*index == length) {
            *index = 0;
            return true;
        }
    } else {
        *index = (value == command[0]) ? 1u : 0u;
    }
    return false;
}

static void fc_handle_command_byte(uint8_t value) {
    if (fc_reply_active) return;

    if (match_command(value, fc_hdx_command, sizeof(fc_hdx_command),
                      &fc_hdx_command_match_index)) {
        fc_arm_reply(fc_test_reply_data, sizeof(fc_test_reply_data),
                     FC_REPLY_KIND_HDX);
    }

    if (match_command(value, fc_ram_command, sizeof(fc_ram_command),
                      &fc_ram_command_match_index)) {
        if (fc_ramgo_pending) {
            fc_reply_ramgo_generation = fc_ramgo_generation;
            fc_arm_reply(fc_ramgo_frame,
                         FC_RAMGO_HEADER_SIZE + fc_ramgo_length,
                         FC_REPLY_KIND_RAMGO);
        } else {
            fc_arm_reply(fc_ramgo_empty, sizeof(fc_ramgo_empty),
                         FC_REPLY_KIND_NONE);
        }
    }
}

static void fc_ramgo_reset_host_parser(void) {
    fc_ramgo_host_state = 0;
    fc_ramgo_host_magic_index = 0;
    fc_ramgo_host_meta_index = 0;
    fc_ramgo_host_payload_index = 0;
}

static void fc_ramgo_host_byte(uint8_t value) {
    if (fc_ramgo_host_state == 0u) {
        if (value == fc_ramgo_magic[fc_ramgo_host_magic_index]) {
            fc_ramgo_host_magic_index++;
            if (fc_ramgo_host_magic_index == sizeof(fc_ramgo_magic)) {
                fc_ramgo_host_state = 1u;
                fc_ramgo_host_magic_index = 0;
                fc_ramgo_host_meta_index = 0;
            }
        } else {
            fc_ramgo_host_magic_index =
                (value == fc_ramgo_magic[0]) ? 1u : 0u;
        }
        return;
    }

    if (fc_ramgo_host_state == 1u) {
        fc_ramgo_host_meta[fc_ramgo_host_meta_index++] = value;
        if (fc_ramgo_host_meta_index != sizeof(fc_ramgo_host_meta)) return;

        fc_ramgo_host_length = (uint16_t)fc_ramgo_host_meta[0] |
                               ((uint16_t)fc_ramgo_host_meta[1] << 8u);
        fc_ramgo_host_expected_crc =
            (uint16_t)fc_ramgo_host_meta[2] |
            ((uint16_t)fc_ramgo_host_meta[3] << 8u);
        if ((fc_ramgo_host_length == 0u) ||
            (fc_ramgo_host_length > FC_RAMGO_MAX_SIZE)) {
            fc_ramgo_upload_error_count++;
            fc_ramgo_reset_host_parser();
            return;
        }

        for (uint32_t i = 0; i < sizeof(fc_ramgo_magic); ++i) {
            fc_ramgo_host_frame[i] = fc_ramgo_magic[i];
        }
        for (uint32_t i = 0; i < sizeof(fc_ramgo_host_meta); ++i) {
            fc_ramgo_host_frame[4u + i] = fc_ramgo_host_meta[i];
        }
        fc_ramgo_host_running_crc = 0xFFFFu;
        fc_ramgo_host_payload_index = 0;
        fc_ramgo_host_state = 2u;
        return;
    }

    fc_ramgo_host_frame[FC_RAMGO_HEADER_SIZE + fc_ramgo_host_payload_index] = value;
    fc_ramgo_host_running_crc =
        crc16_ccitt_update(fc_ramgo_host_running_crc, value);
    fc_ramgo_host_payload_index++;
    if (fc_ramgo_host_payload_index == fc_ramgo_host_length) {
        if (fc_ramgo_host_running_crc == fc_ramgo_host_expected_crc) {
            for (uint32_t i = 0;
                 i < (FC_RAMGO_HEADER_SIZE + fc_ramgo_host_length); ++i) {
                fc_ramgo_frame[i] = fc_ramgo_host_frame[i];
            }
            fc_ramgo_length = fc_ramgo_host_length;
            fc_ramgo_generation++;
            fc_ramgo_pending = true;
            fc_ramgo_upload_count++;
        } else {
            fc_ramgo_upload_error_count++;
        }
        fc_ramgo_reset_host_parser();
    }
}

static void fc_ramgo_usb_receive(void) {
    uint8_t buffer[USB_CHUNK_SIZE];
    while (tud_cdc_n_available(FC_LOG_CDC) != 0u) {
        uint32_t count = tud_cdc_n_read(FC_LOG_CDC, buffer, sizeof(buffer));
        for (uint32_t i = 0; i < count; ++i) fc_ramgo_host_byte(buffer[i]);
    }
}

static void fc_uart_rx_to_ring(void) {
    while (uart_is_readable(FC_LOG_UART)) {
        uint32_t raw = uart_get_hw(FC_LOG_UART)->dr;
        uint8_t value = (uint8_t)raw;
        fc_log_rx_count++;
        if ((raw & (UART_UARTDR_OE_BITS | UART_UARTDR_BE_BITS |
                    UART_UARTDR_PE_BITS | UART_UARTDR_FE_BITS)) != 0u) {
            fc_log_error_count++;
        }
        if (!ring_push(&fc_log_uart_to_usb, value)) {
            // Continue draining the hardware FIFO. The ring records overflow.
        }

        // Commands remain visible on CDC1 while also arming the synchronous
        // controller-style reply path.
        fc_handle_command_byte(value);
    }
}

static void fc_log_gpio_irq(uint gpio, uint32_t events) {
    if (gpio == FC_LOG_RX_PIN) {
        fc_log_edge_count++;
        return;
    }

    // A controller clock is idle high. The FC samples DATA during the low
    // part of its $4016 read and the rising edge ends that read, so advance
    // DATA only on the rising edge for the following read.
    if ((gpio == FC_REPLY_CLOCK_PIN) &&
        ((events & (GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL)) != 0u)) {
        fc_reply_clock_edge_count++;
    }
    if ((gpio == FC_REPLY_CLOCK_PIN) &&
        ((events & GPIO_IRQ_EDGE_RISE) != 0u) && fc_reply_active) {
        uint32_t next = fc_reply_bit_index + 1u;
        fc_reply_bit_index = next;
        if (next < fc_reply_bit_count) {
            uint8_t value = fc_reply_buffer[next >> 3u];
            gpio_put(FC_REPLY_DATA_PIN,
                     ((value >> (next & 7u)) & 1u) ^ 1u);
        } else {
            gpio_put(FC_REPLY_DATA_PIN, 1u);
            fc_reply_active = false;
            fc_reply_completed_count++;
            if ((fc_reply_kind == FC_REPLY_KIND_RAMGO) &&
                (fc_reply_ramgo_generation == fc_ramgo_generation)) {
                // One-shot by design: a console reset must not re-run an old
                // RAM payload. Re-upload it if FC-side CRC verification fails.
                fc_ramgo_pending = false;
            }
            fc_reply_kind = FC_REPLY_KIND_NONE;
        }
    }
}

static void queue_fc_status(void) {
    char text[256];
    uint64_t now = time_us_64();

    // Never let Pico-generated diagnostics fill the same ring that protects
    // real FC output while the CDC terminal is closed. FC bytes may remain
    // buffered for the next open, but synthetic status lines are disposable.
    if (!tud_cdc_n_connected(FC_LOG_CDC)) {
        fc_log_next_report_us = now + 1000000u;
        return;
    }
    if (now < fc_log_next_report_us) return;
    fc_log_next_report_us = now + 1000000u;

    int length = snprintf(
        text, sizeof(text),
        "[PICO] GPIO9_RX bytes=%lu err=%lu edges=%lu overflow=%lu level=%u baud=%lu HDX=%s bit=%lu done=%lu CLK10=%u/%lu DATA8=%u RAMGO=%s len=%u upload=%lu bad=%lu\r\n",
        (unsigned long)fc_log_rx_count,
        (unsigned long)fc_log_error_count,
        (unsigned long)fc_log_edge_count,
        (unsigned long)fc_log_uart_to_usb.overflow_count,
        gpio_get(FC_LOG_RX_PIN) ? 1u : 0u,
        (unsigned long)fc_log_baud,
        fc_reply_active ? "ACTIVE" : "IDLE",
        (unsigned long)fc_reply_bit_index,
        (unsigned long)fc_reply_completed_count,
        gpio_get(FC_REPLY_CLOCK_PIN) ? 1u : 0u,
        (unsigned long)fc_reply_clock_edge_count,
        gpio_get(FC_REPLY_DATA_PIN) ? 1u : 0u,
        fc_ramgo_pending ? "READY" : "EMPTY",
        (unsigned)fc_ramgo_length,
        (unsigned long)fc_ramgo_upload_count,
        (unsigned long)fc_ramgo_upload_error_count);
    if (length <= 0) return;
    if ((size_t)length >= sizeof(text)) length = (int)sizeof(text) - 1;
    if (ring_space(&fc_log_uart_to_usb) < (uint32_t)length) return;
    for (int i = 0; i < length; ++i) {
        ring_push(&fc_log_uart_to_usb, (uint8_t)text[i]);
    }
}

static void ring_to_uart(byte_ring_t *ring, uart_inst_t *uart) {
    while ((ring_count(ring) != 0) && uart_is_writable(uart)) {
        uart_putc_raw(uart, ring_peek(ring, 0));
        ring_discard(ring, 1);
    }
}

static void usb_to_ring(uint8_t cdc, byte_ring_t *ring) {
    uint8_t buffer[USB_CHUNK_SIZE];

    while ((tud_cdc_n_available(cdc) != 0) && (ring_space(ring) != 0)) {
        uint32_t count = tud_cdc_n_available(cdc);
        if (count > sizeof(buffer)) count = sizeof(buffer);
        if (count > ring_space(ring)) count = ring_space(ring);

        count = tud_cdc_n_read(cdc, buffer, count);
        for (uint32_t i = 0; i < count; ++i) {
            ring_push(ring, buffer[i]);
        }
    }
}

static void ring_to_usb(byte_ring_t *ring, uint8_t cdc) {
    uint8_t buffer[USB_CHUNK_SIZE];
    uint32_t count = ring_count(ring);
    uint32_t available = tud_cdc_n_write_available(cdc);

    if (count > sizeof(buffer)) count = sizeof(buffer);
    if (count > available) count = available;
    if (count == 0) return;

    for (uint32_t i = 0; i < count; ++i) {
        buffer[i] = ring_peek(ring, i);
    }

    uint32_t written = tud_cdc_n_write(cdc, buffer, count);
    ring_discard(ring, written);
    if (written != 0) tud_cdc_n_write_flush(cdc);
}

static void configure_uarts(void) {
    uart_init(WRITER_UART, WRITER_BAUD);
    uart_set_format(WRITER_UART, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(WRITER_UART, true);
    gpio_set_function(WRITER_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(WRITER_RX_PIN, GPIO_FUNC_UART);
    gpio_pull_up(WRITER_RX_PIN);

    uart_init(FC_LOG_UART, FC_LOG_BAUD);
    uart_set_format(FC_LOG_UART, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(FC_LOG_UART, true);
    gpio_set_function(FC_LOG_RX_PIN, GPIO_FUNC_UART);
    gpio_pull_up(FC_LOG_RX_PIN);
    gpio_set_irq_enabled_with_callback(
        FC_LOG_RX_PIN, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true,
        &fc_log_gpio_irq);

    gpio_init(FC_REPLY_DATA_PIN);
    gpio_set_dir(FC_REPLY_DATA_PIN, GPIO_OUT);
    gpio_put(FC_REPLY_DATA_PIN, 1u);

    gpio_init(FC_REPLY_CLOCK_PIN);
    gpio_set_dir(FC_REPLY_CLOCK_PIN, GPIO_IN);
    gpio_pull_up(FC_REPLY_CLOCK_PIN);
    gpio_set_irq_enabled(
        FC_REPLY_CLOCK_PIN, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);
    fc_log_next_report_us = time_us_64() + 1000000u;
}

int main(void) {
    board_init();
    configure_uarts();
    tusb_rhport_init_t usb_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO,
    };
    tusb_init(0, &usb_init);

    while (true) {
        tud_task();

        // A 1200-baud touch on either CDC requests BOOTSEL. Defer the reset
        // out of TinyUSB's control callback so the line-coding request can
        // finish cleanly before USB disconnects.
        if (bootsel_requested) {
            sleep_ms(20);
            reset_usb_boot(0u, 0u);
        }

        // Drain both UART hardware FIFOs first to minimize overrun risk.
        uart_rx_to_ring(WRITER_UART, &writer_uart_to_usb);
        fc_uart_rx_to_ring();

        // CDC0 is a lossless, byte-for-byte ROM writer bridge.
        ring_to_usb(&writer_uart_to_usb, WRITER_CDC);
        usb_to_ring(WRITER_CDC, &writer_usb_to_uart);
        ring_to_uart(&writer_usb_to_uart, WRITER_UART);

        // CDC1 accepts only a framed RAM-and-GO upload from the PC. Arbitrary
        // terminal text is ignored until the FCRG magic is found.
        fc_ramgo_usb_receive();
        queue_fc_status();
        ring_to_usb(&fc_log_uart_to_usb, FC_LOG_CDC);
    }
}

// Apply the baud rate selected by each Windows COM client. ROMWRITER_Host
// selects 921600 on CDC0. A terminal should select 300000 on CDC1.
void tud_cdc_line_coding_cb(uint8_t cdc, cdc_line_coding_t const *coding) {
    if ((coding == NULL) || (coding->bit_rate == 0)) return;

    if (coding->bit_rate == 1200u) {
        bootsel_requested = true;
        return;
    }

    if (cdc == WRITER_CDC) {
        uart_set_baudrate(WRITER_UART, coding->bit_rate);
    } else if (cdc == FC_LOG_CDC) {
        fc_log_baud = uart_set_baudrate(FC_LOG_UART, coding->bit_rate);
    }
}
