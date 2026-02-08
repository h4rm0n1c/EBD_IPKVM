#include "adb_spi.h"

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

/* SPI0 pin assignments — match the physical wiring to ATtiny85 USI.
 *
 * ATtiny85 USI three-wire pin roles:
 *   PB0 (pin 5) = DI  — Data In  (slave receives from master MOSI)
 *   PB1 (pin 6) = DO  — Data Out (slave sends to master MISO)
 *   PB2 (pin 7) = USCK — Clock   (from master SCK)
 *   PB4 (pin 3) = CS  — Chip Select (active low, resets USI counter)
 */
#define ADB_SPI_INST  spi0
#define ADB_PIN_MISO  16   /* GP16 ← ATtiny85 PB1 (DO, pin 6)   */
#define ADB_PIN_RESET 17   /* GP17 → ATtiny85 RESET (active low) */
#define ADB_PIN_SCK   18   /* GP18 → ATtiny85 PB2 (USCK, pin 7) */
#define ADB_PIN_MOSI  19   /* GP19 → ATtiny85 PB0 (DI, pin 5)   */
#define ADB_PIN_CS    20   /* GP20 → ATtiny85 PB4 (CS, pin 3)   */

/*
 * Clock speed.  The ATtiny85 USI is polled from trabular's main loop
 * (every ~50-150 µs at 8 MHz).  100 kHz gives ~80 µs per byte which
 * is comfortably within the polling window.
 */
#define ADB_SPI_BAUD  100000

/*
 * Inter-byte gap.  After the 16-bit frame completes, keep CS LOW
 * long enough for handle_data() to poll and process the overflow.
 * handle_data() processes completed bytes regardless of CS state,
 * then uses CS-high to reset the counter for the next frame.
 *
 * The ATtiny85 runs at 8 MHz (fuses confirmed), handle_data()
 * polling gap is ~50-100 µs during active ADB traffic.  150 µs
 * gives comfortable margin.
 */
#define ADB_SPI_GAP_US  150

static bool spi_active = false;
static bool spi_flushed = false;
static uint8_t flush_step = 0;   /* 0 = not started, 1-5 = in progress, 6 = done */

/* ── SPI trace ring buffer ─────────────────────────────────────────── */
static adb_spi_trace_entry_t trace_buf[ADB_SPI_TRACE_LEN];
static uint8_t trace_count = 0;

/* Trabular clear commands sent during flush (one per poll iteration). */
static const uint8_t flush_cmds[] = { 0x05, 0x06, 0x07, 0x08, 0x03 };
#define FLUSH_CMD_COUNT (sizeof(flush_cmds) / sizeof(flush_cmds[0]))

/*
 * Park SPI + CS pins as plain GPIO inputs with no pulls.
 * This is the safe "off" state — no output drivers to glitch the
 * ATtiny85 USI lines.
 */
static void park_pins(void) {
    gpio_set_function(ADB_PIN_MISO, GPIO_FUNC_SIO);
    gpio_set_function(ADB_PIN_SCK,  GPIO_FUNC_SIO);
    gpio_set_function(ADB_PIN_MOSI, GPIO_FUNC_SIO);
    gpio_set_dir(ADB_PIN_MISO, GPIO_IN);
    gpio_set_dir(ADB_PIN_SCK,  GPIO_IN);
    gpio_set_dir(ADB_PIN_MOSI, GPIO_IN);
    gpio_set_dir(ADB_PIN_CS,   GPIO_IN);
    gpio_disable_pulls(ADB_PIN_MISO);
    gpio_disable_pulls(ADB_PIN_SCK);
    gpio_disable_pulls(ADB_PIN_MOSI);
    gpio_disable_pulls(ADB_PIN_CS);
}

void adb_spi_hold_reset(void) {
    /*
     * Drive RESET low as early as possible to hold the ATtiny85 in
     * reset while the Pico boots.  This prevents the USI from counting
     * spurious clock edges on the still-floating SCK line.
     *
     * Also drive CS high (inactive) and SCK low immediately so
     * both lines are stable before the ATtiny85 is released.
     * CS high keeps the ATtiny85 USI idle (PB4 has internal pull-up
     * on the ATtiny side too, but we drive it explicitly for safety).
     *
     * Call this BEFORE stdio_init_all() / tud_init() in main().
     */
    gpio_init(ADB_PIN_RESET);
    gpio_set_dir(ADB_PIN_RESET, GPIO_OUT);
    gpio_put(ADB_PIN_RESET, 0);           /* hold ATtiny85 in reset */

    gpio_init(ADB_PIN_CS);
    gpio_set_dir(ADB_PIN_CS, GPIO_OUT);
    gpio_put(ADB_PIN_CS, 1);              /* CS inactive (high) */

    gpio_init(ADB_PIN_SCK);
    gpio_set_dir(ADB_PIN_SCK, GPIO_OUT);
    gpio_put(ADB_PIN_SCK, 0);             /* SCK low before USI starts */
}

void adb_spi_init(void) {
    if (spi_active) return;

    /*
     * Glitch-free pin handoff: SCK was already driven LOW by
     * adb_spi_hold_reset() at the top of main().  Ensure MOSI is
     * also at its idle level before switching the mux.
     *
     *   CPOL=0 → SCK idles LOW  (already set)
     *   MOSI   → LOW (no data)
     *   MISO   → input
     */
    gpio_init(ADB_PIN_SCK);
    gpio_set_dir(ADB_PIN_SCK, GPIO_OUT);
    gpio_put(ADB_PIN_SCK, 0);

    gpio_init(ADB_PIN_MOSI);
    gpio_set_dir(ADB_PIN_MOSI, GPIO_OUT);
    gpio_put(ADB_PIN_MOSI, 0);

    gpio_init(ADB_PIN_MISO);
    gpio_set_dir(ADB_PIN_MISO, GPIO_IN);

    /* Now init the SPI peripheral (doesn't touch pins yet). */
    spi_init(ADB_SPI_INST, ADB_SPI_BAUD);

    /* SPI mode 0 (CPOL=0, CPHA=0) matches ATtiny85 USI three-wire.
     *
     * 16-bit frame: the ATtiny85 USI 4-bit counter resets to 0 when
     * CS goes high (USISR = 0).  With positive-edge counting
     * (USICS1=1, USICS0=0), 16 rising edges overflow the counter
     * (0→15→overflow).  So we need 16-bit SPI frames.
     *
     * Byte packing (MSB-first):
     *   TX: high byte = 0x00 padding, low byte = command
     *     → first 8 clocks shift in padding (overwritten by cmd)
     *     → last 8 clocks shift in command → USIDR after overflow
     *   RX: high byte = old USIDR (response to prior cmd)
     *     → first 8 clocks shift out old USIDR on MISO
     *     → low byte = echo of padding (0x00) */
    spi_set_format(ADB_SPI_INST, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    /* Switch pin mux — SCK is already LOW, so no edge. */
    gpio_set_function(ADB_PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(ADB_PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(ADB_PIN_MISO, GPIO_FUNC_SPI);

    /* Pull resistors:
     *   SCK  — pull-down (CPOL=0, idle LOW)
     *   MOSI — pull-down (idle LOW when not transmitting)
     *   MISO — pull-down (ATtiny85 PB1 drives this line) */
    gpio_pull_down(ADB_PIN_SCK);
    gpio_pull_down(ADB_PIN_MOSI);
    gpio_pull_down(ADB_PIN_MISO);

    spi_active = true;

    /*
     * Release ATtiny85 from reset.  SCK is now driven LOW by the SPI
     * peripheral, so the ATtiny85 USI counter starts clean at 0.
     */
    gpio_put(ADB_PIN_RESET, 1);

    /* No SPI traffic here — flush is deferred to adb_spi_flush()
     * so that boot-time init is zero-blocking. */
}

bool adb_spi_flush(void) {
    if (!spi_active) return true;
    if (spi_flushed) return true;

    /*
     * Send ONE clear command per call so the caller can service
     * tud_task() between bytes.  Returns true when all done.
     *
     * Trabular clear commands:
     *   0x05 = clear keyboard ring buffer
     *   0x06 = clear mouse button state
     *   0x07 = clear mouse X accumulator
     *   0x08 = clear mouse Y accumulator
     *   0x03 = clear arbitrary device reg 0
     */
    if (flush_step < FLUSH_CMD_COUNT) {
        adb_spi_xfer(flush_cmds[flush_step]);
        flush_step++;
        return false;  /* more to do */
    }

    spi_flushed = true;
    return true;  /* done */
}

void adb_spi_deinit(void) {
    if (!spi_active) return;
    spi_active = false;

    spi_deinit(ADB_SPI_INST);
    park_pins();
}

bool adb_spi_is_active(void) {
    return spi_active;
}

uint8_t adb_spi_xfer(uint8_t cmd) {
    if (!spi_active) return 0;

    /*
     * 16-bit SPI frame, one command per CS cycle.
     *
     * TX word: 0x00 (padding) in high byte, command in low byte.
     * The ATtiny85 USI counter starts at 0 after CS reset.
     * 16 rising edges cause overflow → handle_data() reads USIBR
     * (= low byte = our command) and loads response into USIDR.
     *
     * RX word: high byte = old USIDR (response to prior command),
     * low byte = echo of padding (should be 0x00).
     */
    uint16_t tx16 = (uint16_t)cmd;        /* high = 0x00, low = cmd   */
    uint16_t rx16 = 0;

    gpio_put(ADB_PIN_CS, 0);              /* CS assert (active low)   */
    spi_write16_read16_blocking(ADB_SPI_INST, &tx16, &rx16, 1);
    sleep_us(ADB_SPI_GAP_US);             /* ATtiny processes (CS low)*/
    gpio_put(ADB_PIN_CS, 1);              /* CS deassert → reset ctr  */

    uint8_t response = (uint8_t)(rx16 >> 8);   /* old USIDR            */
    uint8_t echo     = (uint8_t)(rx16 & 0xFF); /* padding echo         */

    /* Record in trace buffer */
    if (trace_count < ADB_SPI_TRACE_LEN) {
        trace_buf[trace_count].tx   = cmd;
        trace_buf[trace_count].rx   = response;
        trace_buf[trace_count].echo = echo;
        trace_count++;
    }

    return response;
}

void adb_spi_send_key(uint8_t adb_code, bool key_down) {
    uint8_t code = key_down ? (adb_code & 0x7F) : (adb_code | 0x80);
    uint8_t lo = code & 0x0F;
    uint8_t hi = (code >> 4) & 0x0F;

    adb_spi_xfer(0x40 | lo);   /* keyboard low nibble  */
    adb_spi_xfer(0x50 | hi);   /* keyboard high nibble → queues keycode */
}

void adb_spi_set_buttons(bool btn1, bool btn2) {
    uint8_t val = 0;
    if (btn1) val |= 0x01;
    if (btn2) val |= 0x02;

    adb_spi_xfer(0x60 | (val & 0x0F));   /* button low nibble  */
    adb_spi_xfer(0x70 | 0x00);           /* button high nibble (always 0) */
}

void adb_spi_move_mouse(int8_t dx, int8_t dy) {
    /* X axis motion */
    if (dx > 0) {
        uint8_t v = (uint8_t)dx;
        if (v > 15) {
            adb_spi_xfer(0xA0 | ((v >> 4) & 0x0F));   /* X += high */
        }
        if (v & 0x0F) {
            adb_spi_xfer(0x80 | (v & 0x0F));           /* X += low  */
        }
    } else if (dx < 0) {
        uint8_t v = (uint8_t)(-dx);
        if (v > 15) {
            adb_spi_xfer(0xB0 | ((v >> 4) & 0x0F));   /* X -= high */
        }
        if (v & 0x0F) {
            adb_spi_xfer(0x90 | (v & 0x0F));           /* X -= low  */
        }
    }

    /* Y axis motion */
    if (dy > 0) {
        uint8_t v = (uint8_t)dy;
        if (v > 15) {
            adb_spi_xfer(0xE0 | ((v >> 4) & 0x0F));   /* Y += high */
        }
        if (v & 0x0F) {
            adb_spi_xfer(0xC0 | (v & 0x0F));           /* Y += low  */
        }
    } else if (dy < 0) {
        uint8_t v = (uint8_t)(-dy);
        if (v > 15) {
            adb_spi_xfer(0xF0 | ((v >> 4) & 0x0F));   /* Y -= high */
        }
        if (v & 0x0F) {
            adb_spi_xfer(0xD0 | (v & 0x0F));           /* Y -= low  */
        }
    }
}

uint8_t adb_spi_status(void) {
    /*
     * Full-duplex one-behind timing: the RX byte of each transfer
     * is the USIDR that handle_data() wrote *after the previous*
     * transfer.  So: send 0x01 (query), then 0x00 (NOP) to clock
     * out the actual status response.
     */
    adb_spi_xfer(0x01);          /* request status         */
    return adb_spi_xfer(0x00);   /* clock out the response */
}

/* ── Trace API ─────────────────────────────────────────────────────── */

void adb_spi_trace_reset(void) {
    trace_count = 0;
}

uint8_t adb_spi_trace_count(void) {
    return trace_count;
}

adb_spi_trace_entry_t adb_spi_trace_entry(uint8_t idx) {
    if (idx < trace_count) return trace_buf[idx];
    adb_spi_trace_entry_t empty = {0, 0, 0};
    return empty;
}
