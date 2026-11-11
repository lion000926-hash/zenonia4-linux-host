#include "demo.h"
#include "profile.h"
#include "pixel_scale.h"
#include <string.h>
#include <stdlib.h>

/* This build draws the world, screen effects, then UI. Keep the original
   800x600 UI coordinates and 640x480 presentation. Only the world beneath
   the UI is replaced by the central 640x480 source pixels. */
static void (*screen_effect)(void *,int,int);
static void (*map_draw)(void *,int);
static void *(*screen_buffer)(void);
static uint16_t crop[640*480];
static int enabled;
static int map_pending;
static int crop_valid;

void scene_view_begin(void){map_pending=0;}

void scene_view_capture(const char *path){
    if(!crop_valid||!getenv("Z4_VIEW_CHECK"))return;
    char name[256];snprintf(name,sizeof(name),"%s.world565",path);
    FILE *f=fopen(name,"wb");
    if(f){fwrite(crop,sizeof(crop),1,f);fclose(f);}
}

static void world_before_ui(void *effect,int x,int y){
    if((uintptr_t)__builtin_return_address(0)==game_base+0x1fa544){
        uint64_t t=profile_now();map_draw(effect,x);profile_add(P_MAP,t);
        map_pending=1;
        return;
    }
    screen_effect(effect,x,y);
    if(!enabled||!map_pending)return;
    map_pending=0;
    uint64_t t=profile_now();
    unsigned char *screen=screen_buffer();
    unsigned char *surface=*(void **)screen;
    int stride=(int)*(uint64_t *)surface;
    uint16_t *pixels=*(uint16_t **)(surface+0x20);
    pixels+=*(int *)(screen+0x88)*stride;
    for(int row=0;row<480;row++)
        memcpy(crop+row*640,pixels+(row+60)*stride+80,640*2);
    crop_valid=1;
    /* Pixel-centred nearest expansion is exactly inverted by SDL's 800->640
       nearest presentation: output[x,y] = crop[x,y], no interpolated pixels. */
    expand_640_to_800_rows(pixels,stride,crop,640,0,600);
    profile_add(P_WORLD_SCALE,t);
    static int logged;
    if(!logged++)game_log("VIEW world=640x480 1:1; UI=800x600; stride=%d\n",stride);
}

void scene_view_init(void){
    /* InitialBFont overwrites the correctly initialised font viewport with
       the Android build's hardcoded 800x480. The UI canvas is 800x600;
       glyphs below y=480 would otherwise disappear in dialogue boxes. */
    const unsigned font_heights[]={0x1f8580,0x1f858c};
    for(int i=0;i<2;i++){
        uint32_t *p=(void *)(game_base+font_heights[i]);
        if(*p!=0x52803c01u){game_log("VIEW unsupported font viewport constant\n");exit(3);}
        *p=0x52800001u|(600u<<5); /* mov w1,#600 */
        __builtin___clear_cache((char *)p,(char *)p+4);
    }
    game_log("VIEW font viewport initialisation fixed to UI 800x600\n");
    uint32_t *call=(void *)(game_base+0x1f9e24);
    uint32_t *map_call=(void *)(game_base+0x1fa540);
    uint32_t *cave=(void *)(game_base+0x14d048);
    /* Exact call and unused 16-byte tail of the existing padding hole. */
    uint32_t expected=0x94000000u|(((0x159a34-0x1f9e24)/4)&0x3ffffffu);
    if(*call!=expected){game_log("VIEW unsupported screen-effect call\n");exit(3);}
    expected=0x94000000u|(((0x18ae20-0x1fa540)/4)&0x3ffffffu);
    if(*map_call!=expected){game_log("VIEW unsupported map call\n");exit(3);}
    for(int i=0;i<4;i++)if(cave[i]){game_log("VIEW occupied trampoline\n");exit(3);}
    screen_effect=(void *)(game_base+0x159a34);
    map_draw=(void *)(game_base+0x18ae20);
    screen_buffer=game_symbol("GcxGetMainScreenBuffer");
    const char *v=getenv("Z4_VIEW_ORIGINAL");
    enabled=!(v&&*v&&*v!='0');
    uintptr_t target=(uintptr_t)world_before_ui;
    uint32_t jump[4]={0x58000050,0xd61f0200,0,0};
    memcpy(jump+2,&target,8);memcpy(cave,jump,16);
    *call=0x94000000u|(((0x14d048-0x1f9e24)/4)&0x3ffffffu);
    *map_call=0x94000000u|(((0x14d048-0x1fa540)/4)&0x3ffffffu);
    __builtin___clear_cache((char *)cave,(char *)cave+16);
    __builtin___clear_cache((char *)call,(char *)call+4);
    __builtin___clear_cache((char *)map_call,(char *)map_call+4);
    game_log("VIEW scene-only hook installed (%s)\n",enabled?"native":"original test");
}
