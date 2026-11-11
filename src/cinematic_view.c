#define _GNU_SOURCE
#include "demo.h"
#include "profile.h"
#include "pixel_scale.h"
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>

static void (*bar_draw)(void *),(*log_draw)(void *);
static void (*loading_draw)(void *,int,int);
static void (*multi_draw)(void *,int,int,int,int,const char *,unsigned,int);
static void *(*get_screen)(void);
static void (*font_view)(int,int);
static int (*text_draw)(void *,const char *,int,int,int,int,int,int,int,int,int);
static void (*next_button_draw)(void *,uint64_t,uint64_t,int);
static void (*fairy_draw)(void *,int,int,int);
static void (*apply_button_draw)(void *,int,int,int);
static void (*option_apply)(void *);
static int (*main_options_draw)(void *);
static int drawing_chinese_main_menu;
static void *community_label_bitmap;
int menu_translate_community(void){return drawing_chinese_main_menu;}
void *menu_community_bitmap(void){return community_label_bitmap;}
static int draw_main_options(void *state){
    int (*language)(void)=(void *)(game_base+0xca3a8);
    drawing_chinese_main_menu=language()==2;
    community_label_bitmap=NULL;
    if(drawing_chinese_main_menu){
        unsigned char *res=**(unsigned char ***)(game_base+0x311b20);
        unsigned char *book=*(void **)(*(unsigned char **)(res+0x30)+0x20);
        book=*(void **)(book+0x98);book=*(void **)(book+0x10);
        void **animations=*(void ***)(*(unsigned char **)(book+0x20)+0x10);
        unsigned char *ani=animations[0],*frames=*(void **)(ani+0x20);
        unsigned char *frame=*(void **)(frames+(ani[0x28]-1)*16);
        unsigned char *parts=*(void **)(frame+0x10);
        community_label_bitmap=*(void **)(parts+22*40);
    }
    int result=main_options_draw(state);
    drawing_chinese_main_menu=0;
    return result;
}
static void apply_two_languages(void *options){
    unsigned char *language=(unsigned char *)options+8;
    if(*language!=0&&*language!=2)*language=2;
    option_apply(options);
}
static uint16_t before[800*600],native[640*480];
static int depth,old_w,old_h;
static int bottom_only;
static int hide_hints;
void cinematic_view_begin(void){hide_hints=0;}
void cinematic_hide_hints(void){hide_hints=1;}
int cinematic_hints_hidden(void){return hide_hints;}
int cinematic_status_touch(int *x,int *y){
    if(z4_menu_open()||z4_story()||z4_char_select_open())return 0;
    unsigned char *ui=*(void **)(game_base+0x314c70);
    if(!ui||ui[0x23a1])return 0;
    int px=*x*4/5,py=*y*4/5;
    if(px<110||px>530||py<400)return 0;
    *x=px;*y=py;return 1;
}
static unsigned char *gfx(void){return **(unsigned char ***)(game_base+0x3109c8);}
static uint16_t *pixels(int *stride){
    unsigned char *s=get_screen(),*surface=*(void **)s;
    *stride=(int)*(uint64_t *)surface;
    return *(uint16_t **)(surface+0x20)+*(int *)(s+0x88)*(*stride);
}
static void clip(int w,int h){
    unsigned char *s=get_screen();int16_t *r=(void *)(s+0x8c);
    r[0]=r[1]=0;r[2]=w;r[3]=h;
    int rect[4]={0,*(int *)(s+0x88),w,h+*(int *)(s+0x88)};
    void (*set)(void *,void *,int *)=(void *)(game_base+0xcba20);
    set(s+8,NULL,rect);
}
static void enter_region(int bottom){
    if(depth++)return;
    uint64_t t=profile_now();
    bottom_only=bottom;
    int stride;uint16_t *p=pixels(&stride);unsigned char *g=gfx();
    old_w=*(int *)(g+0x64);old_h=*(int *)(g+0x68);
    /* Include padding above the status block: native rows 360..479 are
       sourced from menu rows 450..599. Only save their union. */
    for(int y=bottom?360:0;y<600;y++)memcpy(before+y*800,p+y*stride,1600);
    for(int y=bottom?360:0;y<480;y++)for(int x=0;x<640;x++)
        p[y*stride+x]=before[((2*y+1)*5/8)*800+(2*x+1)*5/8];
    *(int *)(g+0x64)=640;*(int *)(g+0x68)=480;font_view(640,480);clip(640,480);
    profile_add(P_NATIVE_SCALE,t);
}
static void enter(void){enter_region(0);}
static void leave(void){
    if(!depth||--depth)return;
    uint64_t t=profile_now();
    int stride;uint16_t *p=pixels(&stride);unsigned char *g=gfx();
    for(int y=bottom_only?360:0;y<480;y++)memcpy(native+y*640,p+y*stride,1280);
    if(bottom_only)for(int y=360;y<480;y++)memcpy(p+y*stride,before+y*800,1600);
    *(int *)(g+0x64)=old_w;*(int *)(g+0x68)=old_h;font_view(old_w,old_h);clip(old_w,old_h);
    expand_640_to_800_rows(p,stride,native,640,bottom_only?450:0,600);
    profile_add(P_NATIVE_SCALE,t);
}
static void draw_bars(void *ui){
    /* Only the bottom status block. Its centre/bottom anchors are retained;
       minimap, touch controls and the rest of the HUD keep their old layout. */
    enter_region(1);bar_draw(ui);leave();
}
static void draw_log(void *ui){
    /* Event pickups/mining and short info/save notices share these two log
       queues. Render both natively, not only the scripted story log. */
    int visible=*(int *)((char *)ui+0x217c)>0||*(int *)((char *)ui+0x2390)>0;
    int own=!depth&&visible;
    if(own)enter();log_draw(ui);if(own)leave();
}
static void draw_loading(void *map,int id,int flag){
    cinematic_hide_hints();
    int own=!depth;if(own)enter();loading_draw(map,id,flag);if(own)leave();
    static int logged;if(!logged++)game_log("CINEVIEW map loading native 640x480\n");
}
static void draw_multi(void *ui,int x,int y,int w,int h,const char *text,unsigned color,int scroll){
    cinematic_hide_hints();
    int own=!depth;
    if(own){enter();x=x*4/5;y=y*4/5;w=w*4/5;h=h*4/5;scroll=scroll*4/5;}
    multi_draw(ui,x,y,w,h,text,color,scroll);
    if(own)leave();
    static int logged;if(!logged++)game_log("CINEVIEW black-scene text native glyphs\n");
}
static void set_text_clip(const int16_t r[4]){
    unsigned char *s=get_screen();
    memcpy(s+0x8c,r,8);
    int rect[4]={r[0],r[1]+*(int *)(s+0x88),r[0]+r[2],r[1]+r[3]+*(int *)(s+0x88)};
    void (*set)(void *,void *,int *)=(void *)(game_base+0xcba20);
    set(s+8,NULL,rect);
}
static void draw_y_hint(int x,int y){
    static const unsigned char letter[7]={17,17,10,4,4,4,4};
    enter();
    int stride;uint16_t *p=pixels(&stride);
    for(int row=0;row<7;row++)for(int col=0;col<5;col++)
        if(letter[row]&(16>>col))
            for(int dy=0;dy<2;dy++)for(int dx=0;dx<2;dx++){
                int px=x+col*2+dx,py=y+row*2+dy;
                if(px>=0&&px<640&&py>=0&&py<480)p[py*stride+px]=0xffff;
            }
    leave();
}
/* Both quest labels are baked into bitmaps, not DrawTextBBF strings.
   Add the physical key inside their existing left padding. */
static void draw_quest_button(void *ui,uint64_t bounds,uint64_t touch,int image){
    next_button_draw(ui,bounds,touch,image);
    if(z4_menu_page()!=5||(image!=10&&image!=11))return;
    unsigned char *res=**(unsigned char ***)(game_base+0x311b20);
    unsigned char *book=*(void **)(*(unsigned char **)(res+8)+0x20);
    book=*(void **)(book+0x98);
    book=*(void **)(book+0x10);
    book=*(void **)(book+0x10);
    void *bitmap=(*(void ***)(book+0x10))[image];
    void **vt=*(void ***)bitmap;
    int (*width)(void *)=vt[0x38/8],(*height)(void *)=vt[0x40/8];
    int16_t r[4];memcpy(r,&bounds,8);
    draw_y_hint((r[0]+r[2]-width(bitmap))*4/5+6,
                r[1]*4/5+(height(bitmap)*4/5-14)/2);
}
static void draw_fairy(void *menu,int frame,int x,int y){
    fairy_draw(menu,frame,x,y);
    unsigned char *mgr=**(unsigned char ***)(game_base+0x311668);
    unsigned char *fairy=*(void **)(mgr+0x18);
    if(fairy[0xcf8]!=1)return; /* no extraction button without an active fairy */
    uint64_t (*bounds)(void *,int,int)=(void *)(game_base+0x22fcdc);
    /* Box 4 is the extraction button art; box 21 is the Rune slot. */
    uint64_t packed=bounds(menu,frame,4);
    int16_t r[4];memcpy(r,&packed,8);
    draw_y_hint((x+r[0])*4/5+6,(y+r[1])*4/5+(r[3]*4/5-14)/2);
}
static void draw_apply_button(void *menu,int frame,int x,int y){
    apply_button_draw(menu,frame,x,y);
    if(!z4_status_apply_ready(menu))return;
    uint64_t (*bounds)(void *,int,int)=(void *)(game_base+0x22fcdc);
    uint64_t packed=bounds(menu,frame,18);
    int16_t r[4];memcpy(r,&packed,8);
    draw_y_hint((x+r[0])*4/5+6,(y+r[1])*4/5+(r[3]*4/5-14)/2);
}
/* Convert only text calls to the native viewport. The page art and all
   bounding boxes are still drawn at their existing 800x600 coordinates. */
static int draw_menu_text(void *g,const char *text,int x,int y,int w,int count,
                           int color,int mode,int flag,int extra,int extra2){
    extern int popup_view_active(void);
    int page=z4_menu_page();
    if(depth||page<0||(page>7&&page!=11)||!z4_menu_open()||popup_view_active())
        return text_draw(g,text,x,y,w,count,color,mode,flag,extra,extra2);
    unsigned char *s=get_screen();
    int16_t saved_clip[4],scaled_clip[4];
    memcpy(saved_clip,s+0x8c,8);
    for(int i=0;i<4;i++)scaled_clip[i]=saved_clip[i]*4/5;
    enter();
    set_text_clip(scaled_clip);
    int result=text_draw(g,text,x*4/5,y*4/5,w>0?w*4/5:w,count,color,mode,flag,extra,extra2);
    leave();
    set_text_clip(saved_clip);
    return result;
}
/* A single illustrated help sheet replaces native text and paging. */
static int (*system_key)(void *,int);
static int help_key(void *menu,int key){
    if(*(int *)((char *)menu+0x24)==2&&key!=0x17)return -1;
    return system_key(menu,key);
}
static void draw_help(void *menu,uint64_t bounds,int x,int y,int a,int b){
    (void)menu;(void)bounds;(void)a;(void)b;
    unsigned char *g=gfx();
    void (*shade)(int,int,int,int,int)=**(void (***)(int,int,int,int,int))(game_base+0x310478);
    if(shade)shade(0,0,*(int *)(g+0x64),*(int *)(g+0x68)+*(int *)(g+0x4c),8);
    unsigned char *res=**(unsigned char ***)(game_base+0x311b20);
    unsigned char *book=*(void **)(*(unsigned char **)(res+8)+0x20);
    book=*(void **)(book+0x230);book=*(void **)(book+0x10);
    unsigned char *frames=*(void **)(book+0x18);
    void *frame=(*(void ***)(frames+0x10))[2];
    void (*draw)(void *,int,int,int,void *,void *)=(*(void ***)frame)[4];
    draw(frame,x,y,0,NULL,NULL);
    enter();int stride;uint16_t *p=pixels(&stride);
    ui_layout_help_guide(p,stride);
    leave();
}

static void jump(uint32_t *p,uintptr_t to){
    const uint32_t code[2]={0x58000050,0xd61f0200};memcpy(p,code,8);memcpy(p+2,&to,8);
}
static void *hook(unsigned offset,void *fn){
    uint32_t *at=(void *)(game_base+offset);
    uint32_t *t=mmap(NULL,4096,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(t==MAP_FAILED)exit(3);
    memcpy(t,at,16);jump(t+4,game_base+offset+16);
    int pool=8;
    for(int i=0;i<4;i++)if((at[i]&0x9f000000u)==0x90000000u){
        int64_t imm=((at[i]>>29)&3)|(((at[i]>>5)&0x7ffff)<<2);
        if(imm&(1<<20))imm-=1<<21;
        uintptr_t page=((game_base+offset+i*4)&~(uintptr_t)4095)+(imm*4096);
        memcpy(t+pool,&page,8);t[i]=0x58000000u|((pool-i)<<5)|(at[i]&31);pool+=2;
    }
    __builtin___clear_cache((char *)t,(char *)t+pool*4);
    jump(at,(uintptr_t)fn);__builtin___clear_cache((char *)at,(char *)at+16);
    return t;
}
void *demo_hook(unsigned offset,void *fn){return hook(offset,fn);}
void cinematic_view_init(void){
    extern void script_text_init(void);script_text_init();
    get_screen=game_symbol("GcxGetMainScreenBuffer");font_view=(void *)(game_base+0xd68b4);
    bar_draw=hook(0x167178,draw_bars);
    log_draw=hook(0x15d150,draw_log);
    loading_draw=hook(0x18c7bc,draw_loading);
    multi_draw=hook(0x15d51c,draw_multi);
    text_draw=hook(0x13d39c,draw_menu_text);
    next_button_draw=hook(0x1630ac,draw_quest_button);
    fairy_draw=hook(0x177254,draw_fairy);
    apply_button_draw=hook(0x169dfc,draw_apply_button);
    option_apply=hook(0x17b600,apply_two_languages);
    main_options_draw=hook(0x1f682c,draw_main_options);
    hook(0x17e37c,draw_help);
    system_key=hook(0x17ddcc,help_key);
    /* Both LEFT and RIGHT toggle English(0) and translated Chinese(2).
       Do not visit the deleted Korean/French/German resource slots. */
    const uint32_t language_cycle[5]={
        0x7100001fu, /* cmp w0,#0 */
        0x52800041u, /* mov w1,#2 */
        0x1a9f0021u, /* csel w1,w1,wzr,eq */
        0xd503201fu,0xd503201fu
    };
    memcpy((void *)(game_base+0x17e258),language_cycle,sizeof(language_cycle));
    __builtin___clear_cache((char *)game_base+0x17e258,(char *)game_base+0x17e26c);
    game_log("MENUTEXT pause/shop/bank pages use native glyphs; page geometry unchanged\n");
    game_log("CINEVIEW native bottom status block, cinematic text and loading; other HUD unchanged\n");
}
