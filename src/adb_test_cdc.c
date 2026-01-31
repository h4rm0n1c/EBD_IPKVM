#include "adb_test_cdc.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "tusb.h"

#include "core_bridge.h"

#define CDC_ADB 2
#define ADB_MOUSE_BUTTON_LEFT 0x01u
#define ADB_KEYCODE_LEFT_SHIFT 0x38u

typedef struct {
    uint8_t state;
    char params[12];
    uint8_t param_len;
} adb_csi_parser_t;

static adb_csi_parser_t csi_parser;
static uint8_t mouse_buttons = 0;
static int8_t mouse_step = 4;

static bool adb_test_push_key(uint8_t keycode, bool down) {
    adb_event_t event = {
        .type = ADB_EVENT_KEY,
        .a = keycode,
        .b = down ? 1 : 0,
        .c = 0,
    };
    return core_bridge_adb_push(&event);
}

static bool adb_test_push_mouse(int8_t dx, int8_t dy, uint8_t buttons) {
    adb_event_t event = {
        .type = ADB_EVENT_MOUSE,
        .a = buttons,
        .b = dx,
        .c = dy,
    };
    return core_bridge_adb_push(&event);
}

static void adb_test_send_click(void) {
    mouse_buttons |= ADB_MOUSE_BUTTON_LEFT;
    adb_test_push_mouse(0, 0, mouse_buttons);
    mouse_buttons &= (uint8_t)~ADB_MOUSE_BUTTON_LEFT;
    adb_test_push_mouse(0, 0, mouse_buttons);
}

static bool adb_test_map_ascii(uint8_t ch, uint8_t *keycode, bool *needs_shift) {
    if (!keycode || !needs_shift) {
        return false;
    }
    *needs_shift = false;
    if (ch >= 'a' && ch <= 'z') {
        static const uint8_t map[] = {
            0x00, 0x0b, 0x08, 0x02, 0x0e, 0x03, 0x05, 0x04, 0x22, 0x26, 0x28, 0x25, 0x2e,
            0x2d, 0x1f, 0x23, 0x0c, 0x0f, 0x01, 0x11, 0x20, 0x09, 0x0d, 0x07, 0x10, 0x06
        };
        *keycode = map[ch - 'a'];
        return true;
    }
    if (ch >= 'A' && ch <= 'Z') {
        if (adb_test_map_ascii((uint8_t)tolower(ch), keycode, needs_shift)) {
            *needs_shift = true;
            return true;
        }
        return false;
    }

    switch (ch) {
    case '0': *keycode = 0x1d; return true;
    case '1': *keycode = 0x12; return true;
    case '2': *keycode = 0x13; return true;
    case '3': *keycode = 0x14; return true;
    case '4': *keycode = 0x15; return true;
    case '5': *keycode = 0x17; return true;
    case '6': *keycode = 0x16; return true;
    case '7': *keycode = 0x1a; return true;
    case '8': *keycode = 0x1c; return true;
    case '9': *keycode = 0x19; return true;
    case ' ': *keycode = 0x31; return true;
    case '\t': *keycode = 0x30; return true;
    case '\r': *keycode = 0x24; return true;
    case '\n': *keycode = 0x24; return true;
    case 0x1b: *keycode = 0x35; return true;
    case 0x7f: *keycode = 0x33; return true;
    case '-': *keycode = 0x1b; return true;
    case '=': *keycode = 0x18; return true;
    case '[': *keycode = 0x21; return true;
    case ']': *keycode = 0x1e; return true;
    case '\\': *keycode = 0x2a; return true;
    case ';': *keycode = 0x29; return true;
    case '\'': *keycode = 0x27; return true;
    case ',': *keycode = 0x2b; return true;
    case '.': *keycode = 0x2f; return true;
    case '/': *keycode = 0x2c; return true;
    case '`': *keycode = 0x32; return true;
    case '!': *keycode = 0x12; *needs_shift = true; return true;
    case '@': *keycode = 0x13; *needs_shift = true; return true;
    case '#': *keycode = 0x14; *needs_shift = true; return true;
    case '$': *keycode = 0x15; *needs_shift = true; return true;
    case '%': *keycode = 0x17; *needs_shift = true; return true;
    case '^': *keycode = 0x16; *needs_shift = true; return true;
    case '&': *keycode = 0x1a; *needs_shift = true; return true;
    case '*': *keycode = 0x1c; *needs_shift = true; return true;
    case '(': *keycode = 0x19; *needs_shift = true; return true;
    case ')': *keycode = 0x1d; *needs_shift = true; return true;
    case '_': *keycode = 0x1b; *needs_shift = true; return true;
    case '+': *keycode = 0x18; *needs_shift = true; return true;
    case '{': *keycode = 0x21; *needs_shift = true; return true;
    case '}': *keycode = 0x1e; *needs_shift = true; return true;
    case '|': *keycode = 0x2a; *needs_shift = true; return true;
    case ':': *keycode = 0x29; *needs_shift = true; return true;
    case '"': *keycode = 0x27; *needs_shift = true; return true;
    case '<': *keycode = 0x2b; *needs_shift = true; return true;
    case '>': *keycode = 0x2f; *needs_shift = true; return true;
    case '?': *keycode = 0x2c; *needs_shift = true; return true;
    case '~': *keycode = 0x32; *needs_shift = true; return true;
    default:
        return false;
    }
}

static void adb_test_send_ascii(uint8_t ch) {
    uint8_t keycode = 0;
    bool needs_shift = false;
    if (!adb_test_map_ascii(ch, &keycode, &needs_shift)) {
        return;
    }
    if (needs_shift) {
        adb_test_push_key(ADB_KEYCODE_LEFT_SHIFT, true);
    }
    adb_test_push_key(keycode, true);
    adb_test_push_key(keycode, false);
    if (needs_shift) {
        adb_test_push_key(ADB_KEYCODE_LEFT_SHIFT, false);
    }
}

static void adb_test_apply_mouse_delta(int8_t dx, int8_t dy) {
    adb_test_push_mouse(dx, dy, mouse_buttons);
}

static void adb_test_handle_csi(uint8_t final) {
    bool shift = false;
    if (csi_parser.param_len > 0) {
        csi_parser.params[csi_parser.param_len] = '\0';
        if (strstr(csi_parser.params, ";2") != NULL) {
            shift = true;
        }
    }

    int8_t step = shift ? (int8_t)(mouse_step * 3) : mouse_step;
    switch (final) {
    case 'A':
        adb_test_apply_mouse_delta(0, (int8_t)-step);
        break;
    case 'B':
        adb_test_apply_mouse_delta(0, step);
        break;
    case 'C':
        adb_test_apply_mouse_delta(step, 0);
        break;
    case 'D':
        adb_test_apply_mouse_delta((int8_t)-step, 0);
        break;
    case 'Z':
        adb_test_send_click();
        break;
    default:
        break;
    }
}

static void adb_test_parse_byte(uint8_t ch) {
    if (csi_parser.state == 0) {
        if (ch == 0x1b) {
            csi_parser.state = 1;
            return;
        }
        adb_test_send_ascii(ch);
        return;
    }

    if (csi_parser.state == 1) {
        if (ch == '[') {
            csi_parser.state = 2;
            csi_parser.param_len = 0;
            return;
        }
        csi_parser.state = 0;
        adb_test_send_ascii(ch);
        return;
    }

    if (csi_parser.state == 2) {
        if ((ch >= '0' && ch <= '9') || ch == ';') {
            if (csi_parser.param_len + 1 < sizeof(csi_parser.params)) {
                csi_parser.params[csi_parser.param_len++] = (char)ch;
            }
            return;
        }
        adb_test_handle_csi(ch);
        csi_parser.state = 0;
    }
}

void adb_test_cdc_init(void) {
    csi_parser.state = 0;
    csi_parser.param_len = 0;
    mouse_buttons = 0;
    mouse_step = 4;
}

void adb_test_cdc_poll(void) {
    while (tud_cdc_n_available(CDC_ADB)) {
        uint8_t ch = 0;
        if (tud_cdc_n_read(CDC_ADB, &ch, 1) != 1) {
            break;
        }
        adb_test_parse_byte(ch);
    }
}
