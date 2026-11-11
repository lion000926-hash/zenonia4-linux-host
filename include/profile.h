#ifndef Z4_PROFILE_H
#define Z4_PROFILE_H
#include <stdint.h>
enum {P_INPUT,P_TICK,P_OVERLAY,P_UPLOAD,P_COPY,P_PRESENT,P_WAIT,
 P_LOGIC,P_DRAW,P_MAP,P_UI,P_WORLD_SCALE,P_NATIVE_SCALE,P_GLYPH,P_CONVERT,P_COUNT};
uint64_t profile_now(void);
void profile_add(int phase,uint64_t start);
void profile_init(void);
void profile_frame_begin(void);
void profile_frame_end(void);
#endif
