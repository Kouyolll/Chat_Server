#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include "fbputchar.h"
#include "key.h"

/* Previous USB Boot keyboard report state. */
static uint8_t prev_keys[6] = {0};
static uint8_t prev_mod = 0;

#define INPUT_ROW 22
#define INPUT_COL0 1
#define INPUT_MAX_COLS 62

static char linebuf[INPUT_MAX_COLS + 1] = {0};
static int line_len = 0;
static int cursor = 0;

static bool contains_key(const uint8_t keys[6], uint8_t kc)
{
  for (int i = 0 ; i < 6 ; i++) {
    if (keys[i] == kc) return true;
  }
  return false;
}

static inline int is_shift(uint8_t mod)
{
  return (mod & (0x02u | 0x20u)) != 0; /* LShift=0x02, RShift=0x20 */
}

static const char hid2ascii_noshift[256] = {
  [0x04] = 'a', [0x05] = 'b', [0x06] = 'c', [0x07] = 'd',
  [0x08] = 'e', [0x09] = 'f', [0x0A] = 'g', [0x0B] = 'h',
  [0x0C] = 'i', [0x0D] = 'j', [0x0E] = 'k', [0x0F] = 'l',
  [0x10] = 'm', [0x11] = 'n', [0x12] = 'o', [0x13] = 'p',
  [0x14] = 'q', [0x15] = 'r', [0x16] = 's', [0x17] = 't',
  [0x18] = 'u', [0x19] = 'v', [0x1A] = 'w', [0x1B] = 'x',
  [0x1C] = 'y', [0x1D] = 'z',

  [0x1E] = '1', [0x1F] = '2', [0x20] = '3', [0x21] = '4',
  [0x22] = '5', [0x23] = '6', [0x24] = '7', [0x25] = '8',
  [0x26] = '9', [0x27] = '0',

  [0x28] = '\n',
  [0x2C] = ' ',
  [0x2D] = '-', [0x2E] = '=', [0x2F] = '[', [0x30] = ']',
  [0x31] = '\\', [0x33] = ';', [0x34] = '\'',
  [0x35] = '`',
  [0x36] = ',', [0x37] = '.', [0x38] = '/',
};

static const char hid2ascii_shift[256] = {
  [0x04] = 'A', [0x05] = 'B', [0x06] = 'C', [0x07] = 'D',
  [0x08] = 'E', [0x09] = 'F', [0x0A] = 'G', [0x0B] = 'H',
  [0x0C] = 'I', [0x0D] = 'J', [0x0E] = 'K', [0x0F] = 'L',
  [0x10] = 'M', [0x11] = 'N', [0x12] = 'O', [0x13] = 'P',
  [0x14] = 'Q', [0x15] = 'R', [0x16] = 'S', [0x17] = 'T',
  [0x18] = 'U', [0x19] = 'V', [0x1A] = 'W', [0x1B] = 'X',
  [0x1C] = 'Y', [0x1D] = 'Z',

  [0x1E] = '!', [0x1F] = '@', [0x20] = '#', [0x21] = '$',
  [0x22] = '%', [0x23] = '^', [0x24] = '&', [0x25] = '*',
  [0x26] = '(', [0x27] = ')',

  [0x28] = '\n',
  [0x2C] = ' ',
  [0x2D] = '_', [0x2E] = '+', [0x2F] = '{', [0x30] = '}',
  [0x31] = '|', [0x33] = ':', [0x34] = '"',
  [0x35] = '~',
  [0x36] = '<', [0x37] = '>', [0x38] = '?',
};

typedef enum {
  KEY_NONE = 0,
  KEY_ASCII,
  KEY_BACKSPACE,
  KEY_LEFT,
  KEY_RIGHT,
  KEY_ENTER
} key_type_t;

typedef struct {
  key_type_t type;
  char ch; /* Valid only when type == KEY_ASCII. */
} key_event_t;

static inline key_event_t decode_key(uint8_t mod, uint8_t kc)
{
  key_event_t ev = { KEY_NONE, 0 };

  if (kc == 0x2A) { ev.type = KEY_BACKSPACE; return ev; }
  if (kc == 0x50) { ev.type = KEY_LEFT;      return ev; }
  if (kc == 0x4F) { ev.type = KEY_RIGHT;     return ev; }
  if (kc == 0x28) { ev.type = KEY_ENTER;     return ev; }

  char ch = is_shift(mod) ? hid2ascii_shift[kc] : hid2ascii_noshift[kc];
  if (ch != 0) {
    ev.type = KEY_ASCII;
    ev.ch = ch;
  }
  return ev;
}

static void redraw_input_line(void)
{
  for (int i = 0 ; i < INPUT_MAX_COLS ; i++) {
    fbputchar(' ', INPUT_ROW, INPUT_COL0 + i);
  }

  for (int i = 0 ; i < line_len ; i++) {
    fbputchar(linebuf[i], INPUT_ROW, INPUT_COL0 + i);
  }

  if (cursor < INPUT_MAX_COLS) {
    fbputchar('_', INPUT_ROW, INPUT_COL0 + cursor);
  }
}

void key_input_reset(void)
{
  line_len = 0;
  cursor = 0;
  linebuf[0] = '\0';
  redraw_input_line();
}

/* Update local line-editing state for one key-down edge. */
static void handle_keydown(uint8_t modifier, uint8_t kc)
{
  key_event_t ev = decode_key(modifier, kc);

  switch (ev.type) {
  case KEY_ASCII:
    if (line_len < INPUT_MAX_COLS) {
      memmove(&linebuf[cursor + 1], &linebuf[cursor], (size_t)(line_len - cursor + 1));
      linebuf[cursor] = ev.ch;
      line_len++;
      cursor++;
    }
    break;
  case KEY_BACKSPACE:
    if (cursor > 0) {
      memmove(&linebuf[cursor - 1], &linebuf[cursor], (size_t)(line_len - cursor + 1));
      cursor--;
      line_len--;
    }
    break;
  case KEY_LEFT:
    if (cursor > 0) cursor--;
    break;
  case KEY_RIGHT:
    if (cursor < line_len) cursor++;
    break;
  case KEY_ENTER:
    line_len = 0;
    cursor = 0;
    linebuf[0] = '\0';
    break;
  default:
    return;
  }

  redraw_input_line();
}

void process_kbd_report(const uint8_t buf[8])
{
  uint8_t modifier = buf[0];
  uint8_t keys[6];
  memcpy(keys, &buf[2], 6);

  /* Fire only on key-down edge (new key present in current frame). */
  for (int i = 0 ; i < 6 ; i++) {
    uint8_t kc = keys[i];
    if (kc == 0) continue;
    if (!contains_key(prev_keys, kc)) {
      handle_keydown(modifier, kc);
    }
  }

  prev_mod = modifier;
  (void) prev_mod;
  memcpy(prev_keys, keys, 6);
}
