#include <stdbool.h>
#include <stdint.h>

#include "bsp/board_api.h"
#include "hardware/gpio.h"
#include "hardware/uart.h"
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

        // Drain both UART hardware FIFOs first to minimize overrun risk.
        uart_rx_to_ring(WRITER_UART, &writer_uart_to_usb);
        uart_rx_to_ring(FC_LOG_UART, &fc_log_uart_to_usb);

        // CDC0 is a lossless, byte-for-byte ROM writer bridge.
        ring_to_usb(&writer_uart_to_usb, WRITER_CDC);
        usb_to_ring(WRITER_CDC, &writer_usb_to_uart);
        ring_to_uart(&writer_usb_to_uart, WRITER_UART);

        // CDC1 is intentionally receive-only. PC output is discarded by
        // TinyUSB and GPIO8 is never configured as a UART TX pin.
        if (tud_cdc_n_available(FC_LOG_CDC) != 0) {
            uint8_t discard[USB_CHUNK_SIZE];
            tud_cdc_n_read(FC_LOG_CDC, discard, sizeof(discard));
        }
        ring_to_usb(&fc_log_uart_to_usb, FC_LOG_CDC);
    }
}

// Apply the baud rate selected by each Windows COM client. ROMWRITER_Host
// selects 921600 on CDC0. A terminal should select 300000 on CDC1.
void tud_cdc_line_coding_cb(uint8_t cdc, cdc_line_coding_t const *coding) {
    if ((coding == NULL) || (coding->bit_rate == 0)) return;

    if (cdc == WRITER_CDC) {
        uart_set_baudrate(WRITER_UART, coding->bit_rate);
    } else if (cdc == FC_LOG_CDC) {
        uart_set_baudrate(FC_LOG_UART, coding->bit_rate);
    }
}
