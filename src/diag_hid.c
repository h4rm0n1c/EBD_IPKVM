#include "diag_hid.h"
#include "adb_spi.h"

#include "pico/stdlib.h"
#include <string.h>

/* ── ADB key codes ────────────────────────────────────────────────── */
/* 0xFF = unmapped.  Index is ASCII code (0x20–0x7E printable range). */

/* ADB scan codes for unshifted keys (index = ASCII - 0x20) */
static const uint8_t adb_unshifted[95] = {
    /* 0x20 ' ' */ 0x31,
    /* 0x21 '!' */ 0x12,  /* shift-1 */
    /* 0x22 '"' */ 0x27,  /* shift-' */
    /* 0x23 '#' */ 0x14,  /* shift-3 */
    /* 0x24 '$' */ 0x15,  /* shift-4 */
    /* 0x25 '%' */ 0x17,  /* shift-5 */
    /* 0x26 '&' */ 0x1A,  /* shift-7 */
    /* 0x27 '\''*/ 0x27,
    /* 0x28 '(' */ 0x19,  /* shift-9 */
    /* 0x29 ')' */ 0x1D,  /* shift-0 */
    /* 0x2A '*' */ 0x1C,  /* shift-8 */
    /* 0x2B '+' */ 0x18,  /* shift-= */
    /* 0x2C ',' */ 0x2B,
    /* 0x2D '-' */ 0x1B,
    /* 0x2E '.' */ 0x2F,
    /* 0x2F '/' */ 0x2C,
    /* 0x30 '0' */ 0x1D,
    /* 0x31 '1' */ 0x12,
    /* 0x32 '2' */ 0x13,
    /* 0x33 '3' */ 0x14,
    /* 0x34 '4' */ 0x15,
    /* 0x35 '5' */ 0x17,
    /* 0x36 '6' */ 0x16,
    /* 0x37 '7' */ 0x1A,
    /* 0x38 '8' */ 0x1C,
    /* 0x39 '9' */ 0x19,
    /* 0x3A ':' */ 0x29,  /* shift-; */
    /* 0x3B ';' */ 0x29,
    /* 0x3C '<' */ 0x2B,  /* shift-, */
    /* 0x3D '=' */ 0x18,
    /* 0x3E '>' */ 0x2F,  /* shift-. */
    /* 0x3F '?' */ 0x2C,  /* shift-/ */
    /* 0x40 '@' */ 0x13,  /* shift-2 */
    /* 0x41 'A' */ 0x00,
    /* 0x42 'B' */ 0x0B,
    /* 0x43 'C' */ 0x08,
    /* 0x44 'D' */ 0x02,
    /* 0x45 'E' */ 0x0E,
    /* 0x46 'F' */ 0x03,
    /* 0x47 'G' */ 0x05,
    /* 0x48 'H' */ 0x04,
    /* 0x49 'I' */ 0x22,
    /* 0x4A 'J' */ 0x26,
    /* 0x4B 'K' */ 0x28,
    /* 0x4C 'L' */ 0x25,
    /* 0x4D 'M' */ 0x2E,
    /* 0x4E 'N' */ 0x2D,
    /* 0x4F 'O' */ 0x1F,
    /* 0x50 'P' */ 0x23,
    /* 0x51 'Q' */ 0x0C,
    /* 0x52 'R' */ 0x0F,
    /* 0x53 'S' */ 0x01,
    /* 0x54 'T' */ 0x11,
    /* 0x55 'U' */ 0x20,
    /* 0x56 'V' */ 0x09,
    /* 0x57 'W' */ 0x0D,
    /* 0x58 'X' */ 0x07,
    /* 0x59 'Y' */ 0x10,
    /* 0x5A 'Z' */ 0x06,
    /* 0x5B '[' */ 0x21,
    /* 0x5C '\\' */ 0x2A,
    /* 0x5D ']' */ 0x1E,
    /* 0x5E '^' */ 0x16,  /* shift-6 */
    /* 0x5F '_' */ 0x1B,  /* shift-- */
    /* 0x60 '`' */ 0x32,
    /* 0x61 'a' */ 0x00,
    /* 0x62 'b' */ 0x0B,
    /* 0x63 'c' */ 0x08,
    /* 0x64 'd' */ 0x02,
    /* 0x65 'e' */ 0x0E,
    /* 0x66 'f' */ 0x03,
    /* 0x67 'g' */ 0x05,
    /* 0x68 'h' */ 0x04,
    /* 0x69 'i' */ 0x22,
    /* 0x6A 'j' */ 0x26,
    /* 0x6B 'k' */ 0x28,
    /* 0x6C 'l' */ 0x25,
    /* 0x6D 'm' */ 0x2E,
    /* 0x6E 'n' */ 0x2D,
    /* 0x6F 'o' */ 0x1F,
    /* 0x70 'p' */ 0x23,
    /* 0x71 'q' */ 0x0C,
    /* 0x72 'r' */ 0x0F,
    /* 0x73 's' */ 0x01,
    /* 0x74 't' */ 0x11,
    /* 0x75 'u' */ 0x20,
    /* 0x76 'v' */ 0x09,
    /* 0x77 'w' */ 0x0D,
    /* 0x78 'x' */ 0x07,
    /* 0x79 'y' */ 0x10,
    /* 0x7A 'z' */ 0x06,
    /* 0x7B '{' */ 0x21,  /* shift-[ */
    /* 0x7C '|' */ 0x2A,  /* shift-\ */
    /* 0x7D '}' */ 0x1E,  /* shift-] */
    /* 0x7E '~' */ 0x32,  /* shift-` */
};

/* Which ASCII codes require Shift held (bitmap for 0x20..0x7E). */
static bool ascii_needs_shift(uint8_t ch) {
    if (ch >= 'A' && ch <= 'Z') return true;
    /* Shifted punctuation: ! " # $ % & ( ) * + : < > ? @ ^ _ { | } ~ */
    static const char shifted_punct[] = "!\"#$%&()*+:<>?@^_{|}~";
    for (const char *p = shifted_punct; *p; p++) {
        if (ch == (uint8_t)*p) return true;
    }
    return false;
}

/* ADB modifier codes */
#define ADB_KEY_COMMAND   0x37
#define ADB_KEY_SHIFT     0x38
#define ADB_KEY_OPTION    0x3A
#define ADB_KEY_CONTROL   0x36
#define ADB_KEY_RETURN    0x24
#define ADB_KEY_DELETE    0x33
#define ADB_KEY_TAB       0x30
#define ADB_KEY_ESCAPE    0x35

/* ── Escape sequence parser states ────────────────────────────────── */
enum esc_state {
    ESC_NONE = 0,
    ESC_GOT_ESC,       /* received 0x1B, waiting for '[' or second ESC */
    ESC_GOT_BRACKET,   /* received ESC [, reading parameter chars      */
};

/* ── Module state ─────────────────────────────────────────────────── */
static bool          active;
static enum esc_state esc;
static uint8_t       esc_param;         /* numeric param accumulator   */
static bool          mouse_btn_held;    /* U-key toggle state          */
static bool          click_pending;     /* momentary click in progress */
static absolute_time_t click_release_at;

/* Boot macro state */
static bool          boot_macro_active;
static absolute_time_t boot_macro_end;

/* Mouse movement step size per key press.
 * Small step (5 px) for mouse-keys testing on the target. */
#define MOUSE_STEP  5

/* ── Helpers ──────────────────────────────────────────────────────── */

static void type_ascii(uint8_t ch) {
    if (ch < 0x20 || ch > 0x7E) return;

    uint8_t adb = adb_unshifted[ch - 0x20];
    bool shift = ascii_needs_shift(ch);

    if (shift) adb_spi_send_key(ADB_KEY_SHIFT, true);
    adb_spi_send_key(adb, true);
    adb_spi_send_key(adb, false);
    if (shift) adb_spi_send_key(ADB_KEY_SHIFT, false);
}

static void handle_special_ascii(uint8_t ch) {
    switch (ch) {
    case '\r':
    case '\n':
        adb_spi_send_key(ADB_KEY_RETURN, true);
        adb_spi_send_key(ADB_KEY_RETURN, false);
        break;
    case '\t':
        adb_spi_send_key(ADB_KEY_TAB, true);
        adb_spi_send_key(ADB_KEY_TAB, false);
        break;
    case 0x08: /* BS */
    case 0x7F: /* DEL */
        adb_spi_send_key(ADB_KEY_DELETE, true);
        adb_spi_send_key(ADB_KEY_DELETE, false);
        break;
    default:
        break;
    }
}

/* ── Mouse control keys (intercepted before type_ascii) ──────────── */
/*   I/i = up, K/k = down, J/j = left, L/l = right
 *   H/h = momentary click, U/u = toggle click-hold              */

static bool is_mouse_key(uint8_t ch) {
    switch (ch) {
    case 'i': case 'I':
    case 'j': case 'J':
    case 'k': case 'K':
    case 'l': case 'L':
    case 'h': case 'H':
    case 'u': case 'U':
        return true;
    default:
        return false;
    }
}

static void handle_mouse_key(uint8_t ch) {
    switch (ch) {
    case 'i': case 'I': adb_spi_move_mouse(0, -MOUSE_STEP); break;
    case 'k': case 'K': adb_spi_move_mouse(0,  MOUSE_STEP); break;
    case 'j': case 'J': adb_spi_move_mouse(-MOUSE_STEP, 0); break;
    case 'l': case 'L': adb_spi_move_mouse( MOUSE_STEP, 0); break;
    case 'h': case 'H':
        /* Momentary click: press now, release after 100 ms via poll. */
        adb_spi_set_buttons(true, false);
        click_pending = true;
        click_release_at = make_timeout_time_ms(100);
        break;
    case 'u': case 'U':
        /* Toggle click: first press = hold down, second = release. */
        mouse_btn_held = !mouse_btn_held;
        adb_spi_set_buttons(mouse_btn_held, false);
        break;
    default:
        break;
    }
}

/* Legacy ANSI escape sequence handlers (arrow keys, PgUp/PgDn) */
static void handle_arrow_up(void)    { adb_spi_move_mouse(0, -MOUSE_STEP); }
static void handle_arrow_down(void)  { adb_spi_move_mouse(0,  MOUSE_STEP); }
static void handle_arrow_right(void) { adb_spi_move_mouse( MOUSE_STEP, 0); }
static void handle_arrow_left(void)  { adb_spi_move_mouse(-MOUSE_STEP, 0); }

/* ── Boot macro: Cmd-Opt-X-O held ~30 s ──────────────────────────── */

static void start_boot_macro(void) {
    boot_macro_active = true;
    boot_macro_end = make_timeout_time_ms(30000);

    /* Press all four keys */
    adb_spi_send_key(ADB_KEY_COMMAND, true);
    adb_spi_send_key(ADB_KEY_OPTION, true);
    adb_spi_send_key(0x07, true);   /* 'x' */
    adb_spi_send_key(0x1F, true);   /* 'o' */
}

static void stop_boot_macro(void) {
    if (!boot_macro_active) return;
    boot_macro_active = false;

    /* Release in reverse order */
    adb_spi_send_key(0x1F, false);  /* 'o' */
    adb_spi_send_key(0x07, false);  /* 'x' */
    adb_spi_send_key(ADB_KEY_OPTION, false);
    adb_spi_send_key(ADB_KEY_COMMAND, false);
}

/* ── Escape sequence finaliser ────────────────────────────────────── */

static void finish_csi(uint8_t final_ch) {
    switch (final_ch) {
    case 'A': handle_arrow_up();    break;
    case 'B': handle_arrow_down();  break;
    case 'C': handle_arrow_right(); break;
    case 'D': handle_arrow_left();  break;
    case '~':
        /* PgUp/PgDn no longer used — mouse click is on H/U keys now */
        break;
    default:
        break;
    }
}

/* ── Public API ───────────────────────────────────────────────────── */

void diag_hid_init(void) {
    active = false;
    esc = ESC_NONE;
    esc_param = 0;
    mouse_btn_held = false;
    click_pending = false;
    boot_macro_active = false;
}

bool diag_hid_active(void) {
    return active;
}

void diag_hid_enter(void) {
    /* Finish any remaining flush steps (usually already done by the
     * deferred flush in app_core_poll, but be safe). */
    while (!adb_spi_flush()) {};
    active = true;
    esc = ESC_NONE;
    esc_param = 0;
    mouse_btn_held = false;
    click_pending = false;
}

void diag_hid_exit(void) {
    stop_boot_macro();
    if (mouse_btn_held) {
        mouse_btn_held = false;
        adb_spi_set_buttons(false, false);
    }
    active = false;
    esc = ESC_NONE;
}

void diag_hid_feed(uint8_t ch) {
    if (!active) return;

    switch (esc) {
    case ESC_NONE:
        if (ch == 0x1B) {
            esc = ESC_GOT_ESC;
        } else if (ch == 0x18) {
            /* Ctrl-X → boot macro */
            start_boot_macro();
        } else if (is_mouse_key(ch)) {
            handle_mouse_key(ch);
        } else if (ch >= 0x20 && ch <= 0x7E) {
            type_ascii(ch);
        } else {
            handle_special_ascii(ch);
        }
        break;

    case ESC_GOT_ESC:
        if (ch == '[') {
            esc = ESC_GOT_BRACKET;
            esc_param = 0;
        } else if (ch == 0x1B) {
            /* Double-ESC → exit diag mode */
            diag_hid_exit();
            return;
        } else {
            /* Bare ESC + something else: send ESC key then process ch */
            adb_spi_send_key(ADB_KEY_ESCAPE, true);
            adb_spi_send_key(ADB_KEY_ESCAPE, false);
            esc = ESC_NONE;
            diag_hid_feed(ch);  /* re-enter for this byte */
            return;
        }
        break;

    case ESC_GOT_BRACKET:
        if (ch >= '0' && ch <= '9') {
            esc_param = (uint8_t)(esc_param * 10 + (ch - '0'));
        } else {
            /* Final character of CSI sequence */
            finish_csi(ch);
            esc = ESC_NONE;
        }
        break;
    }
}

void diag_hid_poll(void) {
    if (!active) return;

    /* Service momentary click release (H key) */
    if (click_pending) {
        if (absolute_time_diff_us(get_absolute_time(), click_release_at) <= 0) {
            click_pending = false;
            /* Restore to the toggle state (PgUp might still be held) */
            adb_spi_set_buttons(mouse_btn_held, false);
        }
    }

    /* Service boot macro timeout */
    if (boot_macro_active) {
        if (absolute_time_diff_us(get_absolute_time(), boot_macro_end) <= 0) {
            stop_boot_macro();
        }
    }
}
