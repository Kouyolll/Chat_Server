#ifndef _FBPUTCHAR_H
#  define _FBPUTCHAR_H

#include <stdint.h>

#define FBOPEN_DEV -1          /* Couldn't open the device */
#define FBOPEN_FSCREENINFO -2  /* Couldn't read the fixed info */
#define FBOPEN_VSCREENINFO -3  /* Couldn't read the variable info */
#define FBOPEN_MMAP -4         /* Couldn't mmap the framebuffer memory */
#define FBOPEN_BPP -5          /* Unexpected bits-per-pixel */

extern int fbopen(void);
extern void fbputchar(char, int, int);
extern void fbputs(const char *, int, int);
extern void fbputchar_color(char, int, int, uint8_t, uint8_t, uint8_t);
extern void fbputs_color(const char *, int, int, uint8_t, uint8_t, uint8_t);

#endif
