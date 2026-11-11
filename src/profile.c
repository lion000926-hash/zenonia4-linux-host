#include "demo.h"
#include "profile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static int active,frames,last_ui=-999,last_menu=-999;
static uint64_t sums[P_COUNT],start_frame,window_total,worst;
static double frame_ms[120];
static void (*update_native)(void *),(*draw_native)(void *),(*ui_native)(void *);
uint64_t profile_now(void){
    if(!active)return 0;
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;
}
void profile_add(int phase,uint64_t start){if(start)sums[phase]+=profile_now()-start;}
static void update(void *s){uint64_t t=profile_now();update_native(s);profile_add(P_LOGIC,t);}
static void draw(void *s){uint64_t t=profile_now();draw_native(s);profile_add(P_DRAW,t);}
static void ui(void *s){uint64_t t=profile_now();ui_native(s);profile_add(P_UI,t);}
void profile_init(void){
    active=access("logs/profile.enabled",F_OK)==0;
    if(!active)return;
    update_native=demo_hook(0x1fa928,update);
    draw_native=demo_hook(0x1f9ae8,draw);
    ui_native=demo_hook(0x16852c,ui);
    game_log("PERF enabled: milliseconds/frame; details are nested, not additive to top-level phases\n");
}
void profile_frame_begin(void){
    if(!active)return;
    int m=z4_menu_page();
    if(demo_ui_status!=last_ui||m!=last_menu){
        frames=0;window_total=worst=0;memset(sums,0,sizeof(sums));
        last_ui=demo_ui_status;last_menu=m;
    }
    start_frame=profile_now();
}
static int compare(const void *a,const void *b){double x=*(const double *)a,y=*(const double *)b;return (x>y)-(x<y);}
void profile_frame_end(void){
    if(!active)return;
    uint64_t ns=profile_now()-start_frame;
    window_total+=ns;if(ns>worst)worst=ns;
    frame_ms[frames++]=ns/1e6;
    if(frames<120)return;
    qsort(frame_ms,120,sizeof(double),compare);
    game_log("PERF frames=120 ui=%d menu=%d fps=%.2f avg_ms=%.3f p95_ms=%.3f max_ms=%.3f",
       last_ui,last_menu,120e9/window_total,window_total/120e6,frame_ms[113],worst/1e6);
    static const char *names[P_COUNT]={"input","tick","overlay","upload","copy","present","wait",
       "logic","draw","map","ui_draw","world_scale","native_scale","glyph","convert"};
    for(int i=0;i<P_COUNT;i++)game_log(" %s=%.3f",names[i],sums[i]/120e6);
    game_log("\n");frames=0;window_total=worst=0;memset(sums,0,sizeof(sums));
}
