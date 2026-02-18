#ifndef _KEY_H
#define _KEY_H

#include <stdint.h>

void key_input_reset(void);
void process_kbd_report(const uint8_t buf[8]);

#endif
