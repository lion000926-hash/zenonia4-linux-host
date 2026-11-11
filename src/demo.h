#pragma once
#include <jni.h>
#include <stddef.h>
#include <stdint.h>
extern JNIEnv *demo_env;
extern JavaVM *demo_vm;
extern int demo_ui_status;
void demo_jni_init(void);
void demo_measure_text(const char *s,float *width,int *height);
void cinematic_view_begin(void);
void cinematic_hide_hints(void);
int cinematic_hints_hidden(void);
int audio_init(void);
void audio_play(int id, int volume, int loop);
void audio_stop(int id);
void audio_shutdown(void);
void game_log(const char *fmt, ...);
int game_load(const char *path);
void *game_symbol(const char *name);
extern uintptr_t game_base;
extern size_t game_size;

/* logical physical buttons, in the order used by the binding table */
enum{
    Z4_R_UP, Z4_R_DOWN, Z4_R_LEFT, Z4_R_RIGHT,
    Z4_R_A, Z4_R_B, Z4_R_X, Z4_R_Y,
    Z4_R_L1, Z4_R_R1, Z4_R_L2, Z4_R_R2,
    Z4_R_START, Z4_R_SELECT, Z4_PAD_COUNT
};
void input_route_init(void (*emit)(int, int));
void input_route_press(int id, int down);
void input_route_frame(unsigned frame);
int input_route_quit(void);
int input_route_tab(int dir);
int z4_menu_open(void);
int z4_scene(void);
int z4_menu_page(void);
int z4_menu_depth(void);
int z4_story(void);
int z4_char_select_open(void);
void input_route_r3(int down);
void input_route_stick(int rx,int ry);
int z4_mouse_active(void);
void z4_mouse_pos(int *x,int *y);
int z4_menu_state(void);
int z4_status_apply_ready(void *menu);
void input_route_dump_slots(void);
void demo_touch_tap(int x,int y);
void demo_touch_release(int x,int y);

void ui_layout_help_guide(uint16_t *pixels,int stride);

void *demo_hook(unsigned offset,void *fn);
void combat_input_init(void);
void combat_input_button(int id,int down);
void combat_input_frame(void);

void ui_layout_quick_slot_hint(int x,int y,int w,int h,int index);
