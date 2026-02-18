/*
 *
 * CSEE 4840 Lab 2 for 2019
 *
 * Name/UNI: 爸爸
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

int sockfd; /* Socket file descriptor */

struct libusb_device_handle *keyboard;
uint8_t endpoint_address;

pthread_t network_thread;
void *network_thread_f(void *);

int chat_row = 1;
int divider_row;
int input_row;
int screen_cols;

static char input_buf[BUFFER_SIZE];
static int input_len = 0;

static int key_in_prev(uint8_t key, uint8_t prev[6])
{
    for (int i = 0; i < 6; i++)
        if (prev[i] == key) return 1;
    return 0;
}

static char hid_to_ascii(uint8_t keycode, int shifted)
{
    if (keycode >= 0x04 && keycode <= 0x1d) { /* a-z */
        char c = 'a' + (keycode - 0x04);
        if (shifted) c = c - 'a' + 'A';
        return c;
    }

    /* 1-0 */
    if (keycode >= 0x1e && keycode <= 0x27) {
        const char normal[] = "1234567890";
        const char shiftd[] = "!@#$%^&*()";
        return shifted ? shiftd[keycode - 0x1e] : normal[keycode - 0x1e];
    }

    switch (keycode) {
    case 0x2c: return ' ';                      /* space */
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

static void redraw_input_line(void)
{
    for (int c = 0; c < screen_cols; c++) fbputchar(' ', input_row, c);
    fbputs("INPUT >", input_row, 0);
    fbputs(input_buf, input_row, 8);
}

int main()
{
    int err;
    struct sockaddr_in serv_addr;
    struct usb_keyboard_packet packet;
    int transferred;

    uint8_t prev_keys[6] = {0};

    if ((err = fbopen()) != 0) {
        fprintf(stderr, "Error: Could not open framebuffer: %d\n", err);
        exit(1);
    }

    memset(framebuffer, 0, fb_finfo.smem_len);

    int screen_rows = fb_vinfo.yres / (FONT_HEIGHT * 2);
    screen_cols = fb_vinfo.xres / (FONT_WIDTH * 2);

    divider_row = screen_rows - 3;
    input_row = screen_rows - 2;

    for (int c = 0; c < screen_cols; c++) fbputchar('-', divider_row, c);
    fbputs("CHAT", 0, 0);
    redraw_input_line();

    /* Open the keyboard */
    if ((keyboard = openkeyboard(&endpoint_address)) == NULL) {
        fprintf(stderr, "Did not find a keyboard\n");
        exit(1);
    }

    /* Create a TCP communications socket */
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        fprintf(stderr, "Error: Could not create socket\n");
        exit(1);
    }

    /* Get the server address */
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, SERVER_HOST, &serv_addr.sin_addr) <= 0) {
        fprintf(stderr, "Error: Could not convert host IP \"%s\"\n", SERVER_HOST);
        exit(1);
    }

    /* Connect the socket to the server */
    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        fprintf(stderr, "Error: connect() failed.  Is the server running?\n");
        exit(1);
    }

    /* Start the network thread */
    pthread_create(&network_thread, NULL, network_thread_f, NULL);

    /* Look for and handle keypresses */
    for (;;) {
        libusb_interrupt_transfer(keyboard, endpoint_address,
                                  (unsigned char *)&packet, sizeof(packet),
                                  &transferred, 0);

        if (transferred != sizeof(packet)) continue;

        int shifted = (packet.modifiers & 0x22) != 0; /* LSHIFT/RSHIFT */

        for (int i = 0; i < 6; i++) {
            uint8_t key = packet.keycode[i];
            if (key == 0) continue;
            if (key_in_prev(key, prev_keys)) continue; /* new key press only */

            if (key == 0x29) { /* ESC */
                goto done;
            } else if (key == 0x28) { /* ENTER -> send */
                if (input_len > 0) {
                    write(sockfd, input_buf, input_len);
                    write(sockfd, "\n", 1);
                    input_len = 0;
                    input_buf[0] = '\0';
                    redraw_input_line();
                }
            } else if (key == 0x2a) { /* BACKSPACE */
                if (input_len > 0) {
                    input_len--;
                    input_buf[input_len] = '\0';
                    redraw_input_line();
                }
            } else {
                char ch = hid_to_ascii(key, shifted);
                if (ch && input_len < BUFFER_SIZE - 1) {
                    input_buf[input_len++] = ch;
                    input_buf[input_len] = '\0';
                    redraw_input_line();
                }
            }
        }

        memcpy(prev_keys, packet.keycode, 6);
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

    while (1) {
        int n = read(sockfd, rx, sizeof(rx));
        if (n <= 0) break;

        for (int i = 0; i < n; i++) {
            char ch = rx[i];

            if (ch == '\r') continue;  // 去掉 CR，避免方块
            if (ch == '\n') {
                line[line_len] = '\0';
                if (line_len > 0) {
                    /* 去掉末尾重复的 <ip:port>，只做显示修正 */
                    static void trim_trailing_addr(char *s)
                    {
                        size_t n = strlen(s);
                        if (n < 3) return;

                        char *last_lt = strrchr(s, '<');
                        char *last_gt = strrchr(s, '>');
                        if (!last_lt || !last_gt || last_gt < last_lt) return;

                        /* 仅当这个 <...> 在行尾时才删 */
                        if (*(last_gt + 1) == '\0') {
                       /* 保守判断：里面要有冒号，像 ip:port */
                        if (strchr(last_lt, ':')) {
                       *last_lt = '\0';
                          /* 顺手去掉末尾空格 */
                       while (strlen(s) > 0 && s[strlen(s) - 1] == ' ')
                      s[strlen(s) - 1] = '\0';
        }
    }
}

                    fbputs(line, chat_row++, 0);
                    if (chat_row >= divider_row) chat_row = 1;
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

