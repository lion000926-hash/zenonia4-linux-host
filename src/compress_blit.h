#pragma once
#include <stdint.h>
void compress_blit_init(void);
/* Host-side pixel check against native DrawOP_* at game_base. Returns 0 on pass. */
int compress_blit_pixel_check(uintptr_t game_base);
