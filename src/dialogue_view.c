#define _GNU_SOURCE
#include "demo.h"
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>

enum{ UI_W=800,UI_H=600,W=640,H=480,STRIP_H=140 };
static void (*say_draw)(void *,int,int,int);
static int (*frame_draw)(void *,int,int,int,long,long);
static void *(*get_screen)(void);
static void (*font_size)(int,int);
static uint16_t backdrop[UI_W*UI_H],native[W*H];
static uint16_t strip_bg[UI_W*STRIP_H],strip_black[UI_W*STRIP_H],strip_white[UI_W*STRIP_H];
static int active;
typedef struct{unsigned char *frame;uint16_t width;int count;int16_t rects[16][4];} FrameState;
static FrameState frames[6];

static unsigned char *graphics(void){return **(unsigned char ***)(game_base+0x3109c8);}
static uint16_t *pixels(int *stride){
    unsigned char *screen=get_screen(),*surface=*(void **)screen;
    *stride=(int)*(uint64_t *)surface;
    return *(uint16_t **)(surface+0x20)+*(int *)(screen+0x88)*(*stride);
}
static void clip(int width,int height){
    unsigned char *screen=get_screen();
    int16_t *r=(void *)(screen+0x8c);
    r[0]=r[1]=0;r[2]=width;r[3]=height;
    int context[4]={0,*(int *)(screen+0x88),width,height+*(int *)(screen+0x88)};
    void (*set_context)(void *,void *,int *)=(void *)(game_base+0xcba20);
    set_context(screen+8,NULL,context);
}
static void frame_layout(int enable){
    if(enable){
        unsigned char *res=**(unsigned char ***)(game_base+0x311b20);
        unsigned char *book=*(void **)(*(unsigned char **)(*(unsigned char **)(res+8)+0x20)+0x68);
        book=*(void **)(book+0x10);
        void **list=*(void ***)(*(unsigned char **)(book+0x18)+0x10);
        int (*count_rects)(void *)=(void *)(game_base+0x12c1c8);
        for(int i=0;i<6;i++){
            FrameState *s=&frames[i];s->frame=list[42+i];
            s->width=*(uint16_t *)(s->frame+0x1e);
            s->count=count_rects(s->frame);
            if(s->count>16){game_log("DIALOG unexpected frame bounds\n");exit(3);}
            int16_t (*r)[4]=*(void **)(s->frame+0x28);
            memcpy(s->rects,r,s->count*8);
            *(uint16_t *)(s->frame+0x1e)=W;
            for(int j=0;j<s->count;j++){
                if(r[j][0]>=400)r[j][0]-=160;
                if(r[j][2]>320)r[j][2]-=160;
            }
        }
    }else for(int i=0;i<6;i++){
        FrameState *s=&frames[i];
        *(uint16_t *)(s->frame+0x1e)=s->width;
        memcpy(*(void **)(s->frame+0x28),s->rects,s->count*8);
    }
}

static void draw_name_plate(void *frame,uint16_t *p,int stride,int x,int y){
    int16_t (*r)[4]=*(void **)((char *)frame+0x28);
    /* The engine positions the name with bounding box 2. Use that very box
       for its background too, rather than the old baked sprite position. */
    int left=x+r[2][0]-12,top=y+r[2][1]-4;
    int width=r[2][2]+24,height=r[2][3]+8;
    if(left<2)left=2;
    if(left+width>W-2)left=W-2-width;
    for(int dy=0;dy<height;dy++){
        int inset=dy<3?3-dy:dy>=height-3?dy-(height-4):0;
        for(int dx=inset;dx<width-inset;dx++){
            int px=left+dx,py=top+dy;
            if(px<0||px>=W||py<0||py>=H)continue;
            uint16_t *dst=p+py*stride+px;
            if(dy<2||dy>=height-2||dx<inset+2||dx>=width-inset-2)
                *dst=0xfd20; /* native-size gold frame */
            else{
                uint16_t d=*dst;
                *dst=(((d>>11)/8)<<11)|((((d>>5)&63)/8)<<5)|((d&31)/8);
            }
        }
    }
}

/* The frame's decorative sides stay at native size. Only a 160-pixel
   stretchable middle section is removed; the text/portrait draw afterwards. */
static int draw_dialogue_frame(void *frame,int x,int y,int op,long blend,long palette){
    extern int menu_translate_community(void);
    extern void *menu_community_bitmap(void);
    int part_count=*(uint16_t *)((char *)frame+0x18);
    if(menu_translate_community()&&part_count>0&&part_count<=32){
        /* Cursor and intro animations have different part counts/order.
           Match the text bitmap itself instead of assuming part 22 of 25. */
        unsigned char *original=*(void **)((char *)frame+0x10);
        unsigned char filtered[32*40],*label=NULL;
        void *bitmap=menu_community_bitmap();int kept=0;
        for(int i=0;i<part_count;i++){
            unsigned char *part=original+i*40;
            if(*(void **)part==bitmap)label=part;
            else {memcpy(filtered+kept*40,part,40);kept++;}
        }
        if(!label)return frame_draw(frame,x,y,op,blend,palette);
        int lx=x+*(int16_t *)(label+8),ly=y+*(int16_t *)(label+10);
        int width=*(int16_t *)((char *)bitmap+0x10),height=*(int16_t *)((char *)bitmap+0x12);
        *(void **)((char *)frame+0x10)=filtered;
        *(uint16_t *)((char *)frame+0x18)=kept;
        int result=frame_draw(frame,x,y,op,blend,palette);
        *(void **)((char *)frame+0x10)=original;
        *(uint16_t *)((char *)frame+0x18)=part_count;
        extern void ui_layout_community_label(int,int,int,int);
        ui_layout_community_label(lx,ly,width,height);
        return result;
    }
    if(!active||(uintptr_t)__builtin_return_address(0)!=game_base+0x164070)
        return frame_draw(frame,x,y,op,blend,palette);
    int16_t (*name_bounds)[4]=*(void **)((char *)frame+0x28);
    /* A portrait on the right already has a correctly placed left nameplate. */
    if(name_bounds[2][0]<W/2)return frame_draw(frame,x,y,op,blend,palette);
    int stride;uint16_t *p=pixels(&stride);
    unsigned char *gfx=graphics();
    uint16_t old=*(uint16_t *)((char *)frame+0x1e);
    for(int i=0;i<6;i++)if(frames[i].frame==frame)
        *(uint16_t *)((char *)frame+0x1e)=frames[i].width;
    *(int *)(gfx+0x64)=UI_W;clip(UI_W,H);
    for(int row=0;row<STRIP_H;row++){
        uint16_t *line=p+(H-STRIP_H+row)*stride;
        memcpy(strip_bg+row*UI_W,line,UI_W*2);memset(line,0,UI_W*2);
    }
    int result=frame_draw(frame,x,y,op,blend,palette);
    for(int row=0;row<STRIP_H;row++){
        uint16_t *line=p+(H-STRIP_H+row)*stride;
        memcpy(strip_black+row*UI_W,line,UI_W*2);memset(line,255,UI_W*2);
    }
    frame_draw(frame,x,y,op,blend,palette);
    int body_row=STRIP_H;
    for(int row=0;row<STRIP_H;row++){
        uint16_t *line=p+(H-STRIP_H+row)*stride;
        memcpy(strip_white+row*UI_W,line,UI_W*2);memcpy(line,strip_bg+row*UI_W,UI_W*2);
        int occupied=0;
        for(int col=0;col<UI_W;col++){
            int at=row*UI_W+col;
            int delta=((strip_white[at]>>5)&63)-((strip_black[at]>>5)&63);
            if(delta<60)occupied++;
        }
        if(occupied>=W&&body_row==STRIP_H)body_row=row;
        /* Discard the baked nameplate above the full-width dialogue body. */
        if(row<body_row)continue;
        for(int col=0;col<W;col++){
            int source=row*UI_W+(col<320?col:col+160);
            uint16_t b=strip_black[source],w=strip_white[source],d=line[col];
            int a=256-((((w>>5)&63)-((b>>5)&63))*256/63);
            if(a<0)a=0;if(a>256)a=256;
            int red=(b>>11)+((d>>11)*(256-a)+128)/256;
            int green=((b>>5)&63)+(((d>>5)&63)*(256-a)+128)/256;
            int blue=(b&31)+((d&31)*(256-a)+128)/256;
            if(red>31)red=31;if(green>63)green=63;if(blue>31)blue=31;
            line[col]=(red<<11)|(green<<5)|blue;
        }
    }
    draw_name_plate(frame,p,stride,x,y);
    static int name_logged;
    if(!name_logged++)game_log("DIALOG left portrait: right nameplate rebuilt at %d,%d\n",x+name_bounds[2][0],y+name_bounds[2][1]);
    *(uint16_t *)((char *)frame+0x1e)=old;
    *(int *)(gfx+0x64)=W;clip(W,H);
    return result;
}

static void draw_native_dialogue(void *say,int page,int count,int mode){
    cinematic_hide_hints();
    int stride;uint16_t *p=pixels(&stride);
    unsigned char *gfx=graphics();
    int width=*(int *)(gfx+0x64),height=*(int *)(gfx+0x68);
    for(int row=0;row<UI_H;row++)memcpy(backdrop+row*UI_W,p+row*stride,UI_W*2);
    for(int row=0;row<H;row++)for(int col=0;col<W;col++)
        p[row*stride+col]=backdrop[((2*row+1)*5/8)*UI_W+(2*col+1)*5/8];
    *(int *)(gfx+0x64)=W;*(int *)(gfx+0x68)=H;
    font_size(W,H);clip(W,H);frame_layout(1);active=1;
    say_draw(say,page,count,mode);
    active=0;frame_layout(0);
    for(int row=0;row<H;row++)memcpy(native+row*W,p+row*stride,W*2);
    *(int *)(gfx+0x64)=width;*(int *)(gfx+0x68)=height;
    font_size(width,height);clip(width,height);
    for(int row=0;row<UI_H;row++)for(int col=0;col<UI_W;col++)
        p[row*stride+col]=native[((2*row+1)*2/5)*W+(2*col+1)*2/5];
    static int logged;
    if(!logged++)game_log("DIALOG portraits/text/frame native 640x480; menu canvas restored 800x600\n");
}

static void *hook(unsigned offset,void *fn){
    uint32_t *at=(void *)(game_base+offset);
    uint32_t *t=mmap(NULL,4096,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(t==MAP_FAILED)exit(3);
    memcpy(t,at,16);
    uintptr_t resume=game_base+offset+16,target=(uintptr_t)fn;
    uint32_t jump[4]={0x58000050,0xd61f0200,0,0};
    memcpy(jump+2,&resume,8);memcpy(t+4,jump,16);
    memcpy(jump+2,&target,8);memcpy(at,jump,16);
    __builtin___clear_cache((char *)t,(char *)t+32);
    __builtin___clear_cache((char *)at,(char *)at+16);
    return t;
}
void dialogue_view_init(void){
    get_screen=game_symbol("GcxGetMainScreenBuffer");
    font_size=(void *)(game_base+0xd68b4);
    /* Word wrapping is prepared outside DrawSayUI too: always calculate it
       against the native dialogue width, rather than the menu canvas. */
    uint32_t *width=(void *)(game_base+0x15cfd0);
    if(*width!=0xb9406400u){game_log("DIALOG unexpected width instruction\n");exit(3);}
    *width=0x52800000u|(640u<<5);
    __builtin___clear_cache((char *)width,(char *)width+4);
    say_draw=hook(0x163f68,draw_native_dialogue);
    /* These frames dispatch through EffectExFrame::__Draw, not Frame::Draw. */
    frame_draw=hook(0x12b77c,draw_dialogue_frame);
    game_log("DIALOG native portrait/dialogue enabled\n");
}
