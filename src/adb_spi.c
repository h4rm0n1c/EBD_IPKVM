#include "adb_spi.h"

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

/* SPI0 pin assignments — match the physical wiring to ATtiny85 USI. */
#define ADB_SPI_INST  spi0
#define ADB_PIN_MISO  16   /* GP16 ← ATtiny85 PB0 (DO)   */
#define ADB_PIN_SCK   18   /* GP18 → ATtiny85 PB2 (USCK)  */
#define ADB_PIN_MOSI  19   /* GP19 → ATtiny85 PB1 (DI)    */

/*
 * Clock speed.  The ATtiny85 USI is polled from trabular's main loop
 * (every ~50-150 µs at 8 MHz).  100 kHz gives ~80 µs per byte which
 * is comfortably within the polling window.
 */
#define ADB_SPI_BAUD  100000

/*
 * Inter-byte gap.  After each SPI byte the ATtiny85 needs time to
 * process the command via handle_serial_data() and load the reply
 * into USIDR before the next transfer starts.  200 µs is safe.
 */
#define ADB_SPI_GAP_US  200

static bool spi_active = false;

/*
 * Park all three SPI pins as plain GPIO inputs with no pulls.
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
    gpio_disable_pulls(ADB_PIN_MISO);
    gpio_disable_pulls(ADB_PIN_SCK);
    gpio_disable_pulls(ADB_PIN_MOSI);
}

void adb_spi_init(void) {
    if (spi_active) return;

    /*
     * Glitch-free pin handoff: drive SCK and MOSI to their SPI mode 0
     * idle levels as GPIO outputs BEFORE switching the mux to the SPI
     * peripheral.  This guarantees no spurious edge on SCK that the
     * ATtiny85 USI could interpret as a clock tick.
     *
     *   CPOL=0 → SCK idles LOW
     *   MOSI   → LOW (no data)
     *   MISO   → input (no pre-conditioning needed)
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

    /* SPI mode 0 (CPOL=0, CPHA=0) matches ATtiny85 USI three-wire. */
    spi_set_format(ADB_SPI_INST, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    /* Switch pin mux — SCK is already LOW, so no edge. */
    gpio_set_function(ADB_PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(ADB_PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(ADB_PIN_MISO, GPIO_FUNC_SPI);

    /* Pull MISO weakly to avoid floating when ATtiny isn't driving. */
    gpio_pull_up(ADB_PIN_MISO);

    spi_active = true;

    /*
     * Belt-and-suspenders: flush every trabular buffer in case anything
     * slipped through.
     *
     * Trabular clear commands:
     *   0x05 = clear keyboard ring buffer
     *   0x06 = clear mouse button state
     *   0x07 = clear mouse X accumulator
     *   0x08 = clear mouse Y accumulator
     *   0x03 = clear arbitrary device reg 0
     */
    adb_spi_xfer(0x05);
    adb_spi_xfer(0x06);
    adb_spi_xfer(0x07);
    adb_spi_xfer(0x08);
    adb_spi_xfer(0x03);
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
    uint8_t rx = 0;
    spi_write_read_blocking(ADB_SPI_INST, &cmd, &rx, 1);
    sleep_us(ADB_SPI_GAP_US);
    return rx;
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
    return adb_spi_xfer(0x01);
}
