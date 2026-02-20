#include "chat_utils.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

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

static void trim_right(char *s)
{
    int n = (int)strlen(s);
    while (n > 0 && s[n - 1] == ' ') {
        s[n - 1] = '\0';
        n--;
    }
}

void sanitize_server_line(char *s)
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
    if (n < 9) return 0;

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

void sanitize_outgoing_message(char *s)
{
    trim_right(s);

    char *scan = s;
    while ((scan = strchr(scan, '<')) != NULL) {
        char *gt = strchr(scan + 1, '>');
        if (gt == NULL) break;

        int token_len = (int)(gt - (scan + 1));
        if (token_len > 0 && is_endpoint_token(scan + 1, token_len)) {
            memmove(scan, gt + 1, strlen(gt + 1) + 1);
            continue;
        }
        scan++;
    }

    for (;;) {
        int len = (int)strlen(s);
        if (len <= 0 || s[len - 1] != '>') break;

        int lt = len - 1;
        while (lt >= 0 && s[lt] != '<') lt--;
        if (lt < 0) break;

        int token_len = len - lt - 2;
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

static uint32_t color_for_id_text(const char *id)
{
    unsigned int h = 2166136261u;
    for (int i = 0; id[i] != '\0'; i++) {
        h ^= (unsigned char)id[i];
        h *= 16777619u;
    }
    return user_palette[h % (sizeof(user_palette) / sizeof(user_palette[0]))];
}

static int extract_identity_key(const char *line, char *out, size_t outsz)
{
    if (extract_sender_endpoint(line, out, outsz)) return 1;
    return extract_first_endpoint(line, out, outsz);
}

static uint32_t assign_unique_color(const char *identity_key)
{
    for (int i = 0; i < identity_count; i++) {
        if (strcmp(identity_keys[i], identity_key) == 0) return identity_colors[i];
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

    if (!found) chosen = color_for_id_text(identity_key);

    if (identity_count < MAX_IDENTITIES) {
        strncpy(identity_keys[identity_count], identity_key, ID_KEY_LEN - 1);
        identity_keys[identity_count][ID_KEY_LEN - 1] = '\0';
        identity_colors[identity_count] = chosen;
        identity_count++;
    }
    return chosen;
}

uint32_t color_for_message(const char *line)
{
    char identity_key[ID_KEY_LEN];
    if (extract_identity_key(line, identity_key, sizeof(identity_key))) {
        return assign_unique_color(identity_key);
    }
    return 0xFFFFFF;
}
