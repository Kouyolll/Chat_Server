/*
 * Userspace program that communicates with the vga_ball device driver
 * through ioctls
 *
 * Stephen A. Edwards
 * Columbia University
 */

#include <stdio.h>
#include "vga_ball.h"
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

int vga_ball_fd;

/* Read and print the current ball coordinates. */
void print_ball_coord(void)
{
  vga_ball_arg_t vla;
  
  if (ioctl(vga_ball_fd, VGA_BALL_READ_COORD, &vla)) {
      perror("ioctl(VGA_BALL_READ_COORD) failed");
      return;
  }
  printf("x=%u y=%u\n", vla.x, vla.y);
}

/* Set the ball coordinates. */
void set_ball_coord(unsigned short x, unsigned short y)
{
  vga_ball_arg_t vla;
  vla.x = x;
  vla.y = y;
  if (ioctl(vga_ball_fd, VGA_BALL_WRITE_COORD, &vla)) {
      perror("ioctl(VGA_BALL_WRITE_COORD) failed");
      return;
  }
}

int main()
{
  unsigned short x = 320;
  unsigned short y = 240;
  int dx = 5;
  int dy = 4;
  int i;
  static const char filename[] = "/dev/vga_ball";

  printf("VGA ball Userspace program started\n");

  if ( (vga_ball_fd = open(filename, O_RDWR)) == -1) {
    fprintf(stderr, "could not open %s\n", filename);
    return -1;
  }

  printf("initial state: ");
  print_ball_coord();

  for (i = 0 ; i < 400 ; i++) {
    set_ball_coord(x, y);
    print_ball_coord();

    if (x <= 8 || x >= 631) dx = -dx;
    if (y <= 8 || y >= 471) dy = -dy;

    x += dx;
    y += dy;
    usleep(30000);
  }
  
  printf("VGA BALL Userspace program terminating\n");
  return 0;
}
