#include "adb_test_cdc.h"

#include <ctype.h>
#include <string.h>

#include "tusb.h"

#include "adb_events.h"

#define CDC_ADB 2

#define ADB_KEY_LEFT_SHIFT 0x38

#define ADB_MOUSE_STEP 5
#define ADB_MOUSE_BUTTON_LEFT 0x01

typedef struct {
    bool esc_active;
    bool csi_active;
    int16_t csi_param;
} adb_ansi_state_t;

static adb_ansi_state_t ansi_state;
static uint8_t mouse_buttons = 0;
static volatile bool adb_diag_requested = false;

static void adb_enqueue_key(uint8_t keycode, bool pressed) {
    adb_event_t ev = {
        .type = ADB_EVENT_KEY,
        .a = keycode,
        .b = pressed ? 1 : 0,
        .c = 0,
    };
    adb_events_push(&ev);
}

static void adb_enqueue_mouse(int8_t dx, int8_t dy, uint8_t buttons) {
    adb_event_t ev = {
        .type = ADB_EVENT_MOUSE,
        .a = buttons,
        .b = dx,
        .c = dy,
    };
    adb_events_push(&ev);
}

typedef struct {
    uint8_t keycode;
    bool needs_shift;
} adb_keymap_t;

static bool adb_keycode_from_ascii(char ch, adb_keymap_t *out) {
    if (!out) {
        return false;
    }

    if (ch >= 'A' && ch <= 'Z') {
        ch = (char)tolower((unsigned char)ch);
        out->needs_shift = true;
    } else {
        out->needs_shift = false;
    }

    switch (ch) {
    case 'a': out->keycode = 0x00; return true;
    case 's': out->keycode = 0x01; return true;
    case 'd': out->keycode = 0x02; return true;
    case 'f': out->keycode = 0x03; return true;
    case 'h': out->keycode = 0x04; return true;
    case 'g': out->keycode = 0x05; return true;
    case 'z': out->keycode = 0x06; return true;
    case 'x': out->keycode = 0x07; return true;
    case 'c': out->keycode = 0x08; return true;
    case 'v': out->keycode = 0x09; return true;
    case 'b': out->keycode = 0x0B; return true;
    case 'q': out->keycode = 0x0C; return true;
    case 'w': out->keycode = 0x0D; return true;
    case 'e': out->keycode = 0x0E; return true;
    case 'r': out->keycode = 0x0F; return true;
    case 'y': out->keycode = 0x10; return true;
    case 't': out->keycode = 0x11; return true;
    case '1': out->keycode = 0x12; return true;
    case '2': out->keycode = 0x13; return true;
    case '3': out->keycode = 0x14; return true;
    case '4': out->keycode = 0x15; return true;
    case '6': out->keycode = 0x16; return true;
    case '5': out->keycode = 0x17; return true;
    case '=': out->keycode = 0x18; return true;
    case '9': out->keycode = 0x19; return true;
    case '7': out->keycode = 0x1A; return true;
    case '-': out->keycode = 0x1B; return true;
    case '8': out->keycode = 0x1C; return true;
    case '0': out->keycode = 0x1D; return true;
    case ']': out->keycode = 0x1E; return true;
    case 'o': out->keycode = 0x1F; return true;
    case 'u': out->keycode = 0x20; return true;
    case '[': out->keycode = 0x21; return true;
    case 'i': out->keycode = 0x22; return true;
    case 'p': out->keycode = 0x23; return true;
    case 'l': out->keycode = 0x25; return true;
    case 'j': out->keycode = 0x26; return true;
    case '\'': out->keycode = 0x27; return true;
    case 'k': out->keycode = 0x28; return true;
    case ';': out->keycode = 0x29; return true;
    case '\\': out->keycode = 0x2A; return true;
    case ',': out->keycode = 0x2B; return true;
    case '/': out->keycode = 0x2C; return true;
    case 'n': out->keycode = 0x2D; return true;
    case 'm': out->keycode = 0x2E; return true;
    case '.': out->keycode = 0x2F; return true;
    case '\t': out->keycode = 0x30; return true;
    case ' ': out->keycode = 0x31; return true;
    case '`': out->keycode = 0x32; return true;
    case '\b': out->keycode = 0x33; return true;
    case 0x7F: out->keycode = 0x33; return true;
    case '\r': out->keycode = 0x24; return true;
    case '\n': out->keycode = 0x24; return true;
    case 0x1B: out->keycode = 0x35; return true;
    default:
        break;
    }

    switch (ch) {
    case '!': out->keycode = 0x12; out->needs_shift = true; return true;
    case '@': out->keycode = 0x13; out->needs_shift = true; return true;
    case '#': out->keycode = 0x14; out->needs_shift = true; return true;
    case '$': out->keycode = 0x15; out->needs_shift = true; return true;
    case '%': out->keycode = 0x17; out->needs_shift = true; return true;
    case '^': out->keycode = 0x16; out->needs_shift = true; return true;
    case '&': out->keycode = 0x1A; out->needs_shift = true; return true;
    case '*': out->keycode = 0x1C; out->needs_shift = true; return true;
    case '(': out->keycode = 0x19; out->needs_shift = true; return true;
    case ')': out->keycode = 0x1D; out->needs_shift = true; return true;
    case '_': out->keycode = 0x1B; out->needs_shift = true; return true;
    case '+': out->keycode = 0x18; out->needs_shift = true; return true;
    case '{': out->keycode = 0x21; out->needs_shift = true; return true;
    case '}': out->keycode = 0x1E; out->needs_shift = true; return true;
    case '|': out->keycode = 0x2A; out->needs_shift = true; return true;
    case ':': out->keycode = 0x29; out->needs_shift = true; return true;
    case '"': out->keycode = 0x27; out->needs_shift = true; return true;
    case '<': out->keycode = 0x2B; out->needs_shift = true; return true;
    case '>': out->keycode = 0x2F; out->needs_shift = true; return true;
    case '?': out->keycode = 0x2C; out->needs_shift = true; return true;
    case '~': out->keycode = 0x32; out->needs_shift = true; return true;
    default:
        break;
    }

    return false;
}

static void adb_emit_key(char ch) {
    adb_keymap_t key = {0};
    if (!adb_keycode_from_ascii(ch, &key)) {
        return;
    }
    if (key.needs_shift) {
        adb_enqueue_key(ADB_KEY_LEFT_SHIFT, true);
    }
    adb_enqueue_key(key.keycode, true);
    adb_enqueue_key(key.keycode, false);
    if (key.needs_shift) {
        adb_enqueue_key(ADB_KEY_LEFT_SHIFT, false);
    }
}

static void adb_handle_csi(char code) {
    switch (code) {
    case 'A':
        adb_enqueue_mouse(0, (int8_t)-ADB_MOUSE_STEP, mouse_buttons);
        break;
    case 'B':
        adb_enqueue_mouse(0, (int8_t)ADB_MOUSE_STEP, mouse_buttons);
        break;
    case 'C':
        adb_enqueue_mouse((int8_t)ADB_MOUSE_STEP, 0, mouse_buttons);
        break;
    case 'D':
        adb_enqueue_mouse((int8_t)-ADB_MOUSE_STEP, 0, mouse_buttons);
        break;
    default:
        break;
    }
}

void adb_test_cdc_init(void) {
    memset(&ansi_state, 0, sizeof(ansi_state));
    mouse_buttons = 0;
    adb_diag_requested = false;
}

bool adb_test_cdc_poll(void) {
    if (!tud_cdc_n_connected(CDC_ADB)) {
        return false;
    }

    bool did_work = false;
    while (tud_cdc_n_available(CDC_ADB)) {
        uint8_t ch = 0;
        if (tud_cdc_n_read(CDC_ADB, &ch, 1) != 1) {
            break;
        }
        if (ch == 'A' || ch == 'a') {
            adb_diag_requested = true;
            did_work = true;
            continue;
        }
        did_work = true;

        if (ansi_state.esc_active) {
            if (ansi_state.csi_active) {
                if (ch >= '0' && ch <= '9') {
                    ansi_state.csi_param = (int16_t)(ansi_state.csi_param * 10 + (ch - '0'));
                    continue;
                }
                if (ch == ';') {
                    ansi_state.csi_param = 0;
                    continue;
                }
                adb_handle_csi((char)ch);
                ansi_state.esc_active = false;
                ansi_state.csi_active = false;
                ansi_state.csi_param = 0;
                continue;
            }
            if (ch == '[') {
                ansi_state.csi_active = true;
                ansi_state.csi_param = 0;
                continue;
            }
            ansi_state.esc_active = false;
        }

        if (ch == 0x1B) {
            ansi_state.esc_active = true;
            continue;
        }

        if (ch == '!') {
            mouse_buttons ^= ADB_MOUSE_BUTTON_LEFT;
            adb_enqueue_mouse(0, 0, mouse_buttons);
            continue;
        }

        adb_emit_key((char)ch);
    }

    return did_work;
}

bool adb_test_cdc_take_diag_request(void) {
    return __atomic_exchange_n(&adb_diag_requested, false, __ATOMIC_ACQ_REL);
}
