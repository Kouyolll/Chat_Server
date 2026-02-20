#ifndef CHAT_UTILS_H
#define CHAT_UTILS_H

#include <stdint.h>

void sanitize_server_line(char *s);
void sanitize_outgoing_message(char *s);
uint32_t color_for_message(const char *line);

#endif
