/* epd.h - the e-Paper display as a status screen (see epdtext.c). */
#pragma once
#include <sys/types.h>

/* Draws up to 7 lines (separated by '\n', 30 columns, UTF-8) into the picture; a line starting with '!' is shown
 * white on black. flip turns the picture by 180 degrees. Nothing is sent to the panel yet. */
void epd_draw_screen(const char *text, int flip);
/* Writes the drawn picture as a PBM image (for checking the layout without a panel). */
int epd_write_preview(const char *path);
/* Sends the drawn picture to the panel in a child process (power up, update, sleep, power down; about 4 s).
 * Returns the child's pid, or -1 if this build has no display driver. The caller reaps the child. */
pid_t epd_update_start(void);
