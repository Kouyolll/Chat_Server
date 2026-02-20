/*
 *
 * CSEE 4840 Lab 2 for 2019
 *
 * Name/UNI: baba
 */
#include "fbputchar.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "usbkeyboard.h"
#include <pthread.h>
#include <linux/fb.h>
#include <ctype.h>
#include <stdint.h>
#include <time.h>

extern struct fb_var_screeninfo fb_vinfo;
extern struct fb_fix_screeninfo fb_finfo;
extern unsigned char *framebuffer;

#define FONT_WIDTH 8
#define FONT_HEIGHT 16

/* Update SERVER_HOST to be the IP address of
 * the chat server you are connecting to
 */
/* arthur.cs.columbia.edu */
#define SERVER_HOST "128.59.19.114"
#define SERVER_PORT 42000
#define BUFFER_SIZE 128
#define INPUT_PROMPT "INPUT > "
#define INPUT_ROWS 2

#define KEY_ESC 0x29
#define KEY_ENTER 0x28
#define KEY_BACKSPACE 0x2a
#define KEY_CAPS_LOCK 0x39
#define KEY_C 0x06
#define KEY_RIGHT 0x4f
#define KEY_LEFT 0x50
#define KEY_F12 0x45

#define MOD_LALT 0x04
#define MOD_LGUI 0x08
#define MOD_RALT 0x40
#define MOD_RGUI 0x80

int sockfd; /* Socket file descriptor */

struct libusb_device_handle *keyboard;
uint8_t endpoint_address;

pthread_t network_thread;
void *network_thread_f(void *);
static void redraw_input_line(void);

int divider_row;
int input_row;
int screen_cols;
int input_rows = INPUT_ROWS;

static char input_buf[BUFFER_SIZE];
static int input_len = 0;
static int input_cursor = 0;
static int cursor_visible = 1;
static uint64_t cursor_blink_start_ms = 0;
static int chat_top = 1;
static int chat_height = 0;
static int chat_used = 0;
static int caps_lock_on = 0;

static char **chat_lines = NULL;
static uint32_t *chat_colors = NULL;

static pthread_mutex_t fb_lock = PTHREAD_MUTEX_INITIALIZER;

#define MAX_IDENTITIES 128
#define ID_KEY_LEN 64

static const uint32_t user_palette[] = {
    0x5BC0EB, 0xF25F5C, 0x9BC53D, 0xFDE74C, 0xE55934, 0xFA7921,
    0xB8C0FF, 0xA29BFE, 0x00BBF9, 0x00F5D4, 0x06D6A0, 0xF15BB5,
    0xF28482, 0x84A59D, 0xF6BD60, 0x43AA8B, 0x577590, 0x90BE6D,
    0x4CC9F0, 0xFF99C8, 0xFFD166, 0xE56B6F, 0xC77DFF, 0x80ED99
};

static char identity_keys[MAX_IDENTITIES][ID_KEY_LEN];
static uint32_t identity_colors[MAX_IDENTITIES];
static int identity_count = 0;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void reset_cursor_blink(void)
{
    cursor_blink_start_ms = now_ms();
    cursor_visible = 1;
}

static void maybe_update_cursor_blink(void)
{
    uint64_t elapsed = now_ms() - cursor_blink_start_ms;
    int visible = ((elapsed / 500u) % 2u) == 0u;
    if (visible != cursor_visible) {
        cursor_visible = visible;
        redraw_input_line();
    }
}

static int key_down_now(uint8_t key, const uint8_t keys[6])
{
    for (int i = 0; i < 6; i++) {
        if (keys[i] == key) return 1;
    }
    return 0;
}

static int key_in_prev(uint8_t key, uint8_t prev[6])
{
    for (int i = 0; i < 6; i++) {
        if (prev[i] == key) return 1;
    }
    return 0;
}

static uint32_t make_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void split_rgb(uint32_t color, uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = (uint8_t)((color >> 16) & 0xffu);
    *g = (uint8_t)((color >> 8) & 0xffu);
    *b = (uint8_t)(color & 0xffu);
}

static char hid_to_ascii(uint8_t keycode, int shifted, int caps_on)
{
    if (keycode >= 0x04 && keycode <= 0x1d) {
        char c = 'a' + (keycode - 0x04);
        if ((shifted ^ caps_on) != 0) c = (char)(c - 'a' + 'A');
        return c;
    }

    if (keycode >= 0x1e && keycode <= 0x27) {
        const char normal[] = "1234567890";
        const char shiftd[] = "!@#$%^&*()";
        return shifted ? shiftd[keycode - 0x1e] : normal[keycode - 0x1e];
    }

    switch (keycode) {
    case 0x2c: return ' ';
    case 0x2d: return shifted ? '_' : '-';
    case 0x2e: return shifted ? '+' : '=';
    case 0x2f: return shifted ? '{' : '[';
    case 0x30: return shifted ? '}' : ']';
    case 0x31: return shifted ? '|' : '\\';
    case 0x33: return shifted ? ':' : ';';
    case 0x34: return shifted ? '"' : '\'';
    case 0x35: return shifted ? '~' : '`';
    case 0x36: return shifted ? '<' : ',';
    case 0x37: return shifted ? '>' : '.';
    case 0x38: return shifted ? '?' : '/';
    default: return 0;
    }
}

static int next_wrap_chunk(const char *s, int start, int width, char *out, int outsz, int *next_start)
{
    int len = (int)strlen(s);
    int i = start;

    while (i < len && s[i] == ' ') i++;
    if (i >= len || width <= 0) {
        out[0] = '\0';
        *next_start = len;
        return 0;
    }

    if (len - i <= width) {
        int n = len - i;
        if (n >= outsz) n = outsz - 1;
        memcpy(out, s + i, n);
        out[n] = '\0';
        *next_start = len;
        return n;
    }

    int hard = i + width;
    int split = -1;
    for (int p = hard; p > i; p--) {
        if (s[p] == ' ') {
            split = p;
            break;
        }
    }

    int end = (split == -1) ? hard : split;
    int n = end - i;
    if (n >= outsz) n = outsz - 1;
    memcpy(out, s + i, n);
    out[n] = '\0';

    i = end;
    while (i < len && s[i] == ' ') i++;
    *next_start = i;
    return n;
}

static int input_line_end(int start, int width)
{
    if (start >= input_len) return input_len;
    if (width <= 0) return start;

    int hard = start + width;
    if (hard >= input_len) return input_len;

    for (int p = hard; p > start; p--) {
        if (input_buf[p - 1] == ' ') return p;
    }
    return hard;
}

static void redraw_input_line_locked(void)
{
    int prompt_len = (int)strlen(INPUT_PROMPT);

    for (int r = 0; r < input_rows; r++) {
        int fb_row = input_row + r;
        for (int c = 0; c < screen_cols; c++) fbputchar(' ', fb_row, c);
    }

    fbputs(INPUT_PROMPT, input_row, 0);

    int pos = 0;
    int row = 0;
    while (pos < input_len && row < input_rows) {
        int width = (row == 0) ? (screen_cols - prompt_len) : screen_cols;
        int col = (row == 0) ? prompt_len : 0;
        char chunk[BUFFER_SIZE];
        int next = input_line_end(pos, width);
        int n = next - pos;
        if (n <= 0) break;
        if (n >= (int)sizeof(chunk)) n = (int)sizeof(chunk) - 1;
        memcpy(chunk, input_buf + pos, (size_t)n);
        chunk[n] = '\0';
        fbputs(chunk, input_row + row, col);

        pos = next;
        row++;
    }

    int c_row = input_row + input_rows - 1;
    int c_col = screen_cols - 1;
    int start = 0;
    row = 0;
    while (row < input_rows) {
        int width = (row == 0) ? (screen_cols - prompt_len) : screen_cols;
        int base_col = (row == 0) ? prompt_len : 0;
        int hard = start + width;
        int end = input_line_end(start, width);
        int cursor_on_this_row =
            (input_cursor < end) ||
            (input_cursor == end && (end == input_len || end != hard)) ||
            (row == input_rows - 1);

        if (cursor_on_this_row) {
            int rel = input_cursor - start;
            if (rel < 0) rel = 0;
            if (rel >= width) rel = width - 1;
            if (rel < 0) rel = 0;
            c_row = input_row + row;
            c_col = base_col + rel;
            if (c_col >= screen_cols) c_col = screen_cols - 1;
            break;
        }

        start = end;
        row++;
    }

    if (cursor_visible) fbputchar('|', c_row, c_col);
}

static void redraw_input_line(void)
{
    pthread_mutex_lock(&fb_lock);
    redraw_input_line_locked();
    pthread_mutex_unlock(&fb_lock);
}

static void delete_before_cursor(void)
{
    if (input_cursor <= 0) return;
    memmove(input_buf + input_cursor - 1,
            input_buf + input_cursor,
            (size_t)(input_len - input_cursor + 1));
    input_cursor--;
    input_len--;
}

static void clear_input_buffer(void)
{
    input_len = 0;
    input_cursor = 0;
    input_buf[0] = '\0';
    reset_cursor_blink();
    redraw_input_line();
}

static void process_hold_repeats(const uint8_t keys[6],
                                 int *backspace_repeat_ticks,
                                 int *left_repeat_ticks,
                                 int *right_repeat_ticks)
{
    if (key_down_now(KEY_BACKSPACE, keys)) {
        (*backspace_repeat_ticks)++;
        if (*backspace_repeat_ticks >= 9 &&
            ((*backspace_repeat_ticks) % 2) == 0 &&
            input_cursor > 0) {
            delete_before_cursor();
            reset_cursor_blink();
            redraw_input_line();
        }
    } else {
        *backspace_repeat_ticks = 0;
    }

    if (key_down_now(KEY_LEFT, keys)) {
        (*left_repeat_ticks)++;
        if (*left_repeat_ticks >= 9 &&
            ((*left_repeat_ticks) % 2) == 0 &&
            input_cursor > 0) {
            input_cursor--;
            reset_cursor_blink();
            redraw_input_line();
        }
    } else {
        *left_repeat_ticks = 0;
    }

    if (key_down_now(KEY_RIGHT, keys)) {
        (*right_repeat_ticks)++;
        if (*right_repeat_ticks >= 9 &&
            ((*right_repeat_ticks) % 2) == 0 &&
            input_cursor < input_len) {
            input_cursor++;
            reset_cursor_blink();
            redraw_input_line();
        }
    } else {
        *right_repeat_ticks = 0;
    }
}

static void chat_push_line(const char *s, uint32_t color)
{
    char *dst = NULL;

    if (chat_used < chat_height) {
        dst = chat_lines[chat_used];
        chat_used++;
    } else {
        char *tmp = chat_lines[0];
        for (int r = 0; r < chat_height - 1; r++) {
            chat_lines[r] = chat_lines[r + 1];
            chat_colors[r] = chat_colors[r + 1];
        }
        chat_lines[chat_height - 1] = tmp;
        dst = chat_lines[chat_height - 1];
    }

    memset(dst, ' ', screen_cols);
    for (int i = 0; s[i] && i < screen_cols; i++) dst[i] = s[i];
    dst[screen_cols] = '\0';
    chat_colors[chat_used - 1] = color;
}

static void chat_push_wrapped(const char *msg, uint32_t color)
{
    int len = (int)strlen(msg);
    int pos = 0;
    if (len == 0) return;

    while (pos < len) {
        char chunk[1024];
        int next = pos;
        if (next_wrap_chunk(msg, pos, screen_cols, chunk, sizeof(chunk), &next) <= 0) break;
        chat_push_line(chunk, color);
        pos = next;
    }
}

static void chat_redraw_locked(void)
{
    for (int r = 0; r < chat_height; r++) {
        int fb_row = chat_top + r;
        for (int c = 0; c < screen_cols; c++) fbputchar(' ', fb_row, c);

        if (r < chat_used) {
            uint8_t cr, cg, cb;
            split_rgb(chat_colors[r], &cr, &cg, &cb);
            fbputs_color(chat_lines[r], fb_row, 0, cr, cg, cb);
        }
    }
}

static void trim_right(char *s)
{
    int n = (int)strlen(s);
    while (n > 0 && s[n - 1] == ' ') {
        s[n - 1] = '\0';
        n--;
    }
}

static void sanitize_server_line(char *s)
{
    trim_right(s);
    if (s[0] == '<') {
        char *gt = strchr(s, '>');
        if (gt != NULL) {
            int tag_len = (int)(gt - s + 1);
            char sender_tag[128];
            if (tag_len >= (int)sizeof(sender_tag)) tag_len = (int)sizeof(sender_tag) - 1;
            memcpy(sender_tag, s, (size_t)tag_len);
            sender_tag[tag_len] = '\0';

            char *dup_same = strstr(gt + 1, sender_tag);
            if (dup_same != NULL) {
                *dup_same = '\0';
            } else {
                char *dup_any = strchr(gt + 1, '<');
                if (dup_any != NULL) *dup_any = '\0';
            }
            trim_right(s);
        }
    }
}

static int extract_sender_endpoint(const char *s, char *out, size_t outsz)
{
    if (s[0] != '<') return 0;
    const char *gt = strchr(s, '>');
    if (gt == NULL) return 0;

    size_t n = (size_t)(gt - s - 1);
    if (n == 0) return 0;
    if (n >= outsz) n = outsz - 1;
    memcpy(out, s + 1, n);
    out[n] = '\0';
    return 1;
}

static int extract_first_ipv4(const char *s, char *out, size_t outsz)
{
    for (int i = 0; s[i] != '\0'; i++) {
        if (!isdigit((unsigned char)s[i])) continue;

        unsigned int a, b, c, d;
        int n = 0;
        if (sscanf(s + i, "%3u.%3u.%3u.%3u%n", &a, &b, &c, &d, &n) == 4) {
            if (a <= 255 && b <= 255 && c <= 255 && d <= 255 && n > 0) {
                if ((size_t)n >= outsz) n = (int)outsz - 1;
                memcpy(out, s + i, (size_t)n);
                out[n] = '\0';
                return 1;
            }
        }
    }
    return 0;
}

static int extract_first_endpoint(const char *s, char *out, size_t outsz)
{
    char ip[64];
    if (!extract_first_ipv4(s, ip, sizeof(ip))) return 0;

    const char *p = strstr(s, ip);
    if (p == NULL) return 0;

    size_t ip_len = strlen(ip);
    size_t n = ip_len;
    if (p[ip_len] == ':') {
        size_t k = ip_len + 1;
        int digits = 0;
        while (p[k] >= '0' && p[k] <= '9' && digits < 5) {
            k++;
            digits++;
        }
        if (digits > 0) n = k;
    }

    if (n >= outsz) n = outsz - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return 1;
}

static int is_endpoint_token(const char *s, int n)
{
    if (n < 9) return 0; /* at least 1.1.1.1:1 */

    int i = 0, part = 0;
    while (part < 4) {
        int digits = 0;
        int value = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9' && digits < 3) {
            value = value * 10 + (s[i] - '0');
            i++;
            digits++;
        }
        if (digits == 0 || value > 255) return 0;
        if (part < 3) {
            if (i >= n || s[i] != '.') return 0;
            i++;
        }
        part++;
    }

    if (i >= n || s[i] != ':') return 0;
    i++;
    int pd = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9' && pd < 5) {
        i++;
        pd++;
    }
    if (pd == 0) return 0;
    return i == n;
}

static void sanitize_outgoing_message(char *s)
{
    trim_right(s);

    for (;;) {
        int len = (int)strlen(s);
        if (len <= 0 || s[len - 1] != '>') break;

        int lt = len - 1;
        while (lt >= 0 && s[lt] != '<') lt--;
        if (lt < 0) break;

        int token_len = len - lt - 2; /* between < and > */
        if (!is_endpoint_token(s + lt + 1, token_len)) break;

        s[lt] = '\0';
        trim_right(s);
    }

    if (s[0] == '<') {
        char *gt = strchr(s, '>');
        if (gt != NULL) {
            int token_len = (int)(gt - (s + 1));
            if (token_len > 0 && is_endpoint_token(s + 1, token_len)) {
                memmove(s, gt + 1, strlen(gt + 1) + 1);
                while (*s == ' ') memmove(s, s + 1, strlen(s));
            }
        }
    }
}

static uint32_t color_for_ip_text(const char *ip)
{
    unsigned int h = 2166136261u;
    for (int i = 0; ip[i] != '\0'; i++) {
        h ^= (unsigned char)ip[i];
        h *= 16777619u;
    }
    return user_palette[h % (sizeof(user_palette) / sizeof(user_palette[0]))];
}

static int extract_identity_key(const char *line, char *out, size_t outsz)
{
    if (extract_sender_endpoint(line, out, outsz)) {
        return 1;
    }
    return extract_first_endpoint(line, out, outsz);
}

static uint32_t assign_unique_color(const char *identity_key)
{
    for (int i = 0; i < identity_count; i++) {
        if (strcmp(identity_keys[i], identity_key) == 0) {
            return identity_colors[i];
        }
    }

    size_t palette_n = sizeof(user_palette) / sizeof(user_palette[0]);
    uint32_t chosen = 0;
    int found = 0;
    for (size_t p = 0; p < palette_n; p++) {
        uint32_t c = user_palette[p];
        int used = 0;
        for (int i = 0; i < identity_count; i++) {
            if (identity_colors[i] == c) {
                used = 1;
                break;
            }
        }
        if (!used) {
            chosen = c;
            found = 1;
            break;
        }
    }

    if (!found) chosen = color_for_ip_text(identity_key);

    if (identity_count < MAX_IDENTITIES) {
        strncpy(identity_keys[identity_count], identity_key, ID_KEY_LEN - 1);
        identity_keys[identity_count][ID_KEY_LEN - 1] = '\0';
        identity_colors[identity_count] = chosen;
        identity_count++;
    }
    return chosen;
}

static uint32_t color_for_message(const char *line)
{
    char identity_key[ID_KEY_LEN];
    if (extract_identity_key(line, identity_key, sizeof(identity_key))) {
        return assign_unique_color(identity_key);
    }
    return make_rgb(255, 255, 255);
}

static void clear_chat_and_input_locked(void)
{
    memset(framebuffer, 0, fb_finfo.smem_len);

    chat_used = 0;
    for (int r = 0; r < chat_height; r++) {
        memset(chat_lines[r], ' ', screen_cols);
        chat_lines[r][screen_cols] = '\0';
        chat_colors[r] = make_rgb(255, 255, 255);
    }

    input_len = 0;
    input_cursor = 0;
    input_buf[0] = '\0';
    reset_cursor_blink();

    for (int c = 0; c < screen_cols; c++) fbputchar('-', divider_row, c);
    fbputs("CHAT", 0, 0);
    redraw_input_line_locked();
}

int main()
{
    int err;
    struct sockaddr_in serv_addr;
    struct usb_keyboard_packet packet;
    int transferred;
    uint8_t prev_keys[6] = {0};
    uint8_t prev_modifiers = 0;
    uint8_t held_keys[6] = {0};
    int have_held_report = 0;
    int backspace_repeat_ticks = 0;
    int left_repeat_ticks = 0;
    int right_repeat_ticks = 0;

    if ((err = fbopen()) != 0) {
        fprintf(stderr, "Error: Could not open framebuffer: %d\n", err);
        exit(1);
    }

    memset(framebuffer, 0, fb_finfo.smem_len);

    int screen_rows = fb_vinfo.yres / (FONT_HEIGHT * 2);
    screen_cols = fb_vinfo.xres / (FONT_WIDTH * 2);

    if (screen_rows < 8) {
        fprintf(stderr, "Error: screen too small\n");
        exit(1);
    }

    divider_row = screen_rows - (INPUT_ROWS + 1);
    input_row = divider_row + 1;
    input_rows = screen_rows - input_row;

    chat_height = divider_row - chat_top;

    chat_lines = calloc(chat_height, sizeof(char *));
    chat_colors = calloc(chat_height, sizeof(uint32_t));
    for (int r = 0; r < chat_height; r++) {
        chat_lines[r] = calloc((size_t)screen_cols + 1, 1);
        memset(chat_lines[r], ' ', screen_cols);
        chat_lines[r][screen_cols] = '\0';
        chat_colors[r] = make_rgb(255, 255, 255);
    }

    for (int c = 0; c < screen_cols; c++) fbputchar('-', divider_row, c);
    fbputs("CHAT", 0, 0);
    reset_cursor_blink();
    redraw_input_line();

    if ((keyboard = openkeyboard(&endpoint_address)) == NULL) {
        fprintf(stderr, "Did not find a keyboard\n");
        exit(1);
    }

    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        fprintf(stderr, "Error: Could not create socket\n");
        exit(1);
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(SERVER_PORT);

    if (inet_pton(AF_INET, SERVER_HOST, &serv_addr.sin_addr) <= 0) {
        fprintf(stderr, "Error: Could not convert host IP \"%s\"\n", SERVER_HOST);
        exit(1);
    }

    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        fprintf(stderr, "Error: connect() failed. Is the server running?\n");
        exit(1);
    }

    pthread_create(&network_thread, NULL, network_thread_f, NULL);

    for (;;) {
        int rc = libusb_interrupt_transfer(keyboard, endpoint_address,
                                           (unsigned char *)&packet, sizeof(packet),
                                           &transferred, 30);
        if (rc == LIBUSB_ERROR_TIMEOUT) {
            if (have_held_report) {
                process_hold_repeats(held_keys,
                                     &backspace_repeat_ticks,
                                     &left_repeat_ticks,
                                     &right_repeat_ticks);
            }
            maybe_update_cursor_blink();
            continue;
        }
        if (rc != 0 || transferred != sizeof(packet)) continue;

        int shifted = (packet.modifiers & 0x22) != 0;
        int win_down = (packet.modifiers & (MOD_LGUI | MOD_RGUI)) != 0;
        int win_prev_down = (prev_modifiers & (MOD_LGUI | MOD_RGUI)) != 0;
        int new_printable_count = 0;
        if (win_down && !win_prev_down) {
            pthread_mutex_lock(&fb_lock);
            clear_chat_and_input_locked();
            pthread_mutex_unlock(&fb_lock);
        }

        for (int i = 0; i < 6; i++) {
            uint8_t key = packet.keycode[i];
            if (key == 0) continue;
            if (key_in_prev(key, prev_keys)) continue;
            if (hid_to_ascii(key, shifted, caps_lock_on) != 0) new_printable_count++;
        }

        int suppress_printables = (new_printable_count > 1);

        for (int i = 0; i < 6; i++) {
            uint8_t key = packet.keycode[i];
            if (key == 0) continue;
            if (key_in_prev(key, prev_keys)) continue;

            if (key == KEY_ESC) {
                goto done;
            } else if (key == KEY_CAPS_LOCK) {
                caps_lock_on = !caps_lock_on;
            } else if (key == KEY_F12 &&
                       (packet.modifiers & (MOD_LALT | MOD_RALT)) != 0) {
                pthread_mutex_lock(&fb_lock);
                clear_chat_and_input_locked();
                pthread_mutex_unlock(&fb_lock);
                reset_cursor_blink();
            } else if (key == KEY_C &&
                       (packet.modifiers & (MOD_LALT | MOD_RALT)) != 0) {
                clear_input_buffer();
            } else if (key == KEY_ENTER) {
                if (input_len > 0) {
                    char tx[BUFFER_SIZE];
                    memcpy(tx, input_buf, (size_t)input_len);
                    tx[input_len] = '\0';
                    sanitize_outgoing_message(tx);
                    int tx_len = (int)strlen(tx);
                    if (tx_len > 0) {
                        write(sockfd, tx, tx_len);
                        write(sockfd, "\n", 1);
                    }
                    input_len = 0;
                    input_cursor = 0;
                    input_buf[0] = '\0';
                    reset_cursor_blink();
                    redraw_input_line();
                }
            } else if (key == KEY_LEFT) {
                if (input_cursor > 0) {
                    input_cursor--;
                    reset_cursor_blink();
                    redraw_input_line();
                }
            } else if (key == KEY_RIGHT) {
                if (input_cursor < input_len) {
                    input_cursor++;
                    reset_cursor_blink();
                    redraw_input_line();
                }
            } else if (key == KEY_BACKSPACE) {
                if (input_cursor > 0) {
                    delete_before_cursor();
                    reset_cursor_blink();
                    redraw_input_line();
                }
            } else {
                char ch = hid_to_ascii(key, shifted, caps_lock_on);
                if (suppress_printables) continue;
                if (ch && input_len < BUFFER_SIZE - 1) {
                    memmove(input_buf + input_cursor + 1,
                            input_buf + input_cursor,
                            (size_t)(input_len - input_cursor + 1));
                    input_buf[input_cursor] = ch;
                    input_cursor++;
                    input_len++;
                    reset_cursor_blink();
                    redraw_input_line();
                }
            }
        }

        process_hold_repeats(packet.keycode,
                             &backspace_repeat_ticks,
                             &left_repeat_ticks,
                             &right_repeat_ticks);

        memcpy(prev_keys, packet.keycode, 6);
        memcpy(held_keys, packet.keycode, 6);
        have_held_report = 1;
        prev_modifiers = packet.modifiers;
        maybe_update_cursor_blink();
    }

done:
    pthread_cancel(network_thread);
    pthread_join(network_thread, NULL);
    return 0;
}

void *network_thread_f(void *ignored)
{
    char rx[BUFFER_SIZE];
    char line[1024];
    int line_len = 0;
    (void)ignored;

    while (1) {
        int n = read(sockfd, rx, sizeof(rx));
        if (n <= 0) break;

        for (int i = 0; i < n; i++) {
            char ch = rx[i];

            if (ch == '\r') continue;
            if (ch == '\n') {
                line[line_len] = '\0';
                if (line_len > 0) {
                    sanitize_server_line(line);
                    if (line[0] != '\0') {
                        uint32_t color = color_for_message(line);
                        pthread_mutex_lock(&fb_lock);
                        chat_push_wrapped(line, color);
                        chat_redraw_locked();
                        redraw_input_line_locked();
                        pthread_mutex_unlock(&fb_lock);
                    }
                }
                line_len = 0;
                continue;
            }

            if ((unsigned char)ch >= 32 && (unsigned char)ch <= 126) {
                if (line_len < (int)sizeof(line) - 1) {
                    line[line_len++] = ch;
                }
            }
        }
    }

    return NULL;
}
