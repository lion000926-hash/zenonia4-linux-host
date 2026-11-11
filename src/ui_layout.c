#include "demo.h"
#include "font.h"
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "native_text_zh.h"

/*
 * Touch-keypad layout.
 *
 * The right column keeps the user's order 1..5 (engine types 10,9,8,7,6).
 * The top-left keeps pause (11) and shop (13); the quick-save button (12) is
 * hidden because L1 does the same job.  Every visible ring is drawn as the
 * same 84-pixel block. Key tags overlap the lower third of the rings.
 */
enum{ W=800, H=600, SIZE=84 };

static const struct{ unsigned char type; short cx, cy; const char *label; } items[]={
    {10,756,196,"R1"},
    { 9,756,279,"X"},
    { 8,756,362,"Y"},
    { 7,756,445,"B"},
    { 6,756,528,"R2"},
    {11, 62, 52,"START"},
    {13,156, 52,"L2"},
};
#define NITEMS ((int)(sizeof(items)/sizeof(items[0])))
/* 12 = quick save button (L1 does it), 14 = circular map shortcut,
   15/16 = on-screen confirm/back
   buttons, both replaced by physical A/B. */
static int hidden(int type){ return type==12||type==14||type==15||type==16; }

static void *(*get_ani)(void *,int,int);
static int (*bbox_count)(void *,int);
static uint64_t (*bbox_rect)(void *,int,int);
static void (*frame_rect)(void *,int,int,uint64_t *,int,int);
static void (*ani_draw)(void *,int,int,int,int,long);
static void *(*screen_buffer)(void);
static int current_type;
static unsigned hud_tag_mask;
static int before_main_menu=1;
static void (*main_menu_draw)(void *);
/* R1's pressed artwork has its own bounds but shares the button centre. */
static struct{int ready,l,t,w,h,box,dx0,dy0;}content[NITEMS+1];
static uint16_t bg[256*256],black[256*256],white[256*256];
/* Cache by the native animation frame and its drawing parameters before
   capturing the sprite. Non-default palettes retain exact render comparison. */
static struct{
    int valid,rw,rh,box;
    uint16_t black[256*256],white[256*256];
    uint32_t rgba[256*256]; /* premultiplied 5/6/5 colour + 9-bit coverage */
} sprite_cache[NITEMS+1];

typedef struct {
    void *ani,*frame;
    int x,y,op,blend,rw,rh;
    long palette;
    unsigned char entry[16],header[32],parts[8*40];
} HudSpriteKey;
static HudSpriteKey sprite_keys[NITEMS+1];
static int sprite_key_valid[NITEMS+1];
static int sprite_key(HudSpriteKey *key,void *ani,int x,int y,int op,int blend,long palette,int rw,int rh){
    if(palette)return 0;
    unsigned char *state=*(void **)((char *)ani+0x10);
    unsigned char *frames=*(void **)((char *)ani+0x20);
    if(!state||!frames)return 0;
    unsigned char *entry=frames+state[0]*16,*frame=*(void **)entry;
    if(!frame)return 0;
    int count=*(uint16_t *)(frame+0x18);
    unsigned char *parts=*(void **)(frame+0x10);
    if(count<1||count>8||!parts)return 0;
    memset(key,0,sizeof(*key));key->ani=ani;key->frame=frame;
    key->x=x;key->y=y;key->op=op;key->blend=blend;key->palette=palette;
    key->rw=rw;key->rh=rh;
    memcpy(key->entry,entry,16);memcpy(key->header,frame,32);
    memcpy(key->parts,parts,count*40);
    return 1;
}

/* Every ring is scaled so its solid part (alpha > 200, shadows excluded)
   spans the same logical diameter.  The sprites differ in baked-in shadow,
   so scaling the whole content box to one size would not look uniform. */
enum{ TARGET_SOLID=66 };

static int slot(int type){
    for(int i=0;i<NITEMS;i++) if(items[i].type==type) return i;
    return -1;
}
static uint64_t pack(int x,int y,int w,int h){
    return(uint16_t)x|((uint64_t)(uint16_t)y<<16)|((uint64_t)(uint16_t)w<<32)|((uint64_t)(uint16_t)h<<48);
}
static void original_offset(int type,int *x,int *y){
    void *global=*(void **)(game_base+0x3109c8);unsigned char *p=*(void **)global;
    if(type<=4){*x=0;*y=*(int *)(p+0x68)+*(int *)(p+0x4c);}
    else if(type<=10){*x=*(int *)(p+0x64);*y=*(int *)(p+0x68)+*(int *)(p+0x4c);}
    else if(type<=13){*x=0;*y=0;}
    else if(type<=16){*x=*(int *)(p+0x64);*y=0;}
}
static void layout_offset(void *keypad,int type,int *x,int *y){original_offset(type,x,y);current_type=type;}
static uint64_t layout_rect(void *keypad,int type,int index){
    if(hidden(type))return 0;                      /* hidden: no touch area */
    int i=slot(type);if(i>=0)return pack(items[i].cx-SIZE/2,items[i].cy-SIZE/2,SIZE,SIZE);
    void *ani=get_ani(keypad,type,0);if(!ani)return 0;
    unsigned char *state=*(void **)((char *)ani+0x10);
    void *frames=*(void **)((char *)ani+0x20);
    void *frame=*(void **)(frames+(*state)*16);
    uint64_t r=bbox_count(frame,0)>0?bbox_rect(frame,0,index):0;
    int x=0,y=0;original_offset(type,&x,&y);
    {static int rl; if(rl<70){int rx=(int16_t)r+x,ry=(int16_t)(r>>16)+y;
        game_log("RECT type=%d idx=%d x=%d y=%d w=%d h=%d\n",type,index,rx,ry,(uint16_t)(r>>32),(uint16_t)(r>>48));rl++;}}
    return pack((int16_t)r+x,(int16_t)(r>>16)+y,(uint16_t)(r>>32),(uint16_t)(r>>48));
}
static int alpha256(uint16_t b,uint16_t w){
    int d=((w>>5)&63)-((b>>5)&63),a=256-d*256/63;
    return a<0?0:a>256?256:a;
}

/* ---- key tags -------------------------------------------------------- */
static const unsigned char glyph_bits[12][7]={
    {0x1e,0x11,0x11,0x1e,0x14,0x12,0x11}, /* R */
    {0x11,0x11,0x0a,0x04,0x0a,0x11,0x11}, /* X */
    {0x11,0x11,0x0a,0x04,0x04,0x04,0x04}, /* Y */
    {0x1e,0x11,0x11,0x1e,0x11,0x11,0x1e}, /* B */
    {0x0f,0x10,0x10,0x0e,0x01,0x01,0x1e}, /* S */
    {0x1f,0x04,0x04,0x04,0x04,0x04,0x04}, /* T */
    {0x0e,0x11,0x11,0x1f,0x11,0x11,0x11}, /* A */
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1f}, /* L */
    {0x04,0x0c,0x04,0x04,0x04,0x04,0x0e}, /* 1 */
    {0x0e,0x11,0x01,0x02,0x04,0x08,0x1f}, /* 2 */
    {0x02,0x04,0x08,0x10,0x08,0x04,0x02}, /* < */
    {0x08,0x04,0x02,0x01,0x02,0x04,0x08}, /* > */
};
static int glyph(char c){
    switch(c){
    case 'R':return 0; case 'X':return 1; case 'Y':return 2; case 'B':return 3;
    case 'S':return 4; case 'T':return 5; case 'A':return 6; case 'L':return 7;
    case '1':return 8; case '2':return 9;
    case '<':return 10; case '>':return 11;
    }
    return -1;
}

#define TAG_FILL   0xFFFFu   /* plain white ellipse, no border */
#define TAG_TEXT   0x0000u

static void put(uint16_t *pixels,int stride,int x,int y,uint16_t c){
    if(x<0||y<0||x>=W||y>=H)return;
    pixels[y*stride+x]=c;
}
/* Small ellipse plate with a bold bitmap caption, attached under a ring. */
static void draw_tag_width(uint16_t *pixels,int stride,int cx,int top,const char *text,int minimum){
    const int scale=3, gh=7*scale, gw=5*scale, adv=gw+2;
    int n=(int)strlen(text);
    int tw=n*adv-2;
    int w=tw+16; if(w<minimum)w=minimum;
    int h=gh+8;
    int x0=cx-w/2, y0=top;
    float rx=w/2.0f, ry=h/2.0f;
    for(int dy=0;dy<h;dy++){
        float ny=((float)dy+0.5f-ry)/ry;
        for(int dx=0;dx<w;dx++){
            float nx=((float)dx+0.5f-rx)/rx;
            float d=nx*nx+ny*ny;
            if(d>1.0f)continue;
            put(pixels,stride,x0+dx,y0+dy,TAG_FILL);
        }
    }
    int tx=x0+(w-tw)/2, ty=y0+(h-gh)/2;
    for(int i=0;i<n;i++){
        int g=glyph(text[i]); if(g<0)continue;
        for(int row=0;row<7;row++){
            unsigned char bits=glyph_bits[g][row];
            for(int col=0;col<5;col++){
                if(!(bits&(0x10>>col)))continue;
                for(int sy=0;sy<scale;sy++)for(int sx=0;sx<scale;sx++)
                    put(pixels,stride,tx+i*adv+col*scale+sx,ty+row*scale+sy,TAG_TEXT);
            }
        }
    }
}
static void draw_tag(uint16_t *pixels,int stride,int cx,int top,const char *text){
    draw_tag_width(pixels,stride,cx,top,text,34);
}

static void layout_draw(void*ani,int x,int y,int op,int blend,long palette){
    {static unsigned seen;if(!(seen&(1u<<current_type))&&z4_menu_open()){seen|=1u<<current_type;game_log("UI menu type %d\n",current_type);}}
    if(hidden(current_type))return;                /* replaced by a physical key */
    if(demo_ui_status==14&&!z4_menu_open()&&current_type!=10){
        /* Keep input, cooldowns and slot state intact; render only the normal
           HUD sprite instead of its pressed variant or pressed draw mode. */
        void *keypad=*(void **)(game_base+0x31df38);
        void *normal=get_ani(keypad,current_type,0);
        if(normal)ani=normal;
        op=1;
    }
    int i=slot(current_type);
    if(i<0){ani_draw(ani,x,y,op,blend,palette);return;}
    int ci=(current_type==10&&op==0)?NITEMS:i;
    uint64_t r=0;frame_rect(ani,x,y,&r,op,blend);
    int rx=(int16_t)r,ry=(int16_t)(r>>16),rw=(uint16_t)(r>>32),rh=(uint16_t)(r>>48);
    if(rx+rw>W)rw=W-rx;if(ry+rh>H)rh=H-ry;
    if(rw<=0||rh<=0||rw>256||rh>256||rx<0||ry<0||rx+rw>W||ry+rh>H){
        game_log("UI unsupported rect type=%d %d,%d %dx%d\n",current_type,rx,ry,rw,rh);
        ani_draw(ani,x,y,op,blend,palette);return;
    }
    unsigned char *buf=screen_buffer();unsigned char *surface=*(void **)buf;
    uint16_t *pixels=*(uint16_t **)(surface+0x20);
    int stride=*(uint64_t *)surface,offset_y=*(int *)(buf+0x88);
    pixels+=offset_y*stride;
    static int logged;if(!logged++){game_log("UI surface=%p pixels=%p stride=%d yoffset=%d\n",surface,pixels,stride,offset_y);}
    HudSpriteKey key;
    int keyable=sprite_key(&key,ani,x,y,op,blend,palette,rw,rh);
    int same_key=keyable&&sprite_key_valid[ci]&&!memcmp(&key,&sprite_keys[ci],sizeof(key));
    int fast_hit=same_key&&sprite_cache[ci].valid&&content[ci].ready;
    if(!fast_hit){
        if(keyable&&!same_key)content[ci].ready=0;
    /* Two temporary backgrounds recover a premultiplied sprite including its original opacity. */
    for(int row=0;row<rh;row++){memcpy(bg+row*rw,pixels+(ry+row)*W+rx,rw*2);memset(pixels+(ry+row)*W+rx,0,rw*2);}
    ani_draw(ani,x,y,op,blend,palette);
    for(int row=0;row<rh;row++){memcpy(black+row*rw,pixels+(ry+row)*W+rx,rw*2);memset(pixels+(ry+row)*W+rx,255,rw*2);}
    ani_draw(ani,x,y,op,blend,palette);
    for(int row=0;row<rh;row++){memcpy(white+row*rw,pixels+(ry+row)*W+rx,rw*2);memcpy(pixels+(ry+row)*W+rx,bg+row*rw,rw*2);}
    }
    if(!content[ci].ready){
        int l=rw,t=rh,rr=-1,bb=-1;
        int sl=rw,st=rh,sr=-1,sb=-1;
        for(int sy=0;sy<rh;sy++)for(int sx=0;sx<rw;sx++){
            int a=alpha256(black[sy*rw+sx],white[sy*rw+sx]);
            if(a>8){if(sx<l)l=sx;if(sx>rr)rr=sx;if(sy<t)t=sy;if(sy>bb)bb=sy;}
            if(a>200){if(sx<sl)sl=sx;if(sx>sr)sr=sx;if(sy<st)st=sy;if(sy>sb)sb=sy;}
        }
        if(rr<l||bb<t)return;
        content[ci].l=l;content[ci].t=t;content[ci].w=rr-l+1;content[ci].h=bb-t+1;
        int sw=(sr>=sl)?sr-sl+1:content[ci].w;
        if(sw<8)sw=content[ci].w;
        int box=(content[ci].w*TARGET_SOLID+sw/2)/sw;
        float scale=(float)box/content[ci].w;
        float cx_s=(sr>=sl)?(sl+sr)/2.0f:(l+rr)/2.0f;
        float cy_s=(sb>=st)?(st+sb)/2.0f:(t+bb)/2.0f;
        content[ci].box=box;
        content[ci].dx0=items[i].cx-(int)((cx_s-content[ci].l)*scale+0.5f);
        content[ci].dy0=items[i].cy-(int)((cy_s-content[ci].t)*scale+0.5f);
        content[ci].ready=1;
        game_log("UI icon=%d type=%d src=%d,%d %dx%d content=%d,%d %dx%d solid=%d box=%d at %d,%d\n",
                 i+1,current_type,rx,ry,rw,rh,l,t,content[ci].w,content[ci].h,sw,box,content[ci].dx0,content[ci].dy0);
    }
    int box=content[ci].box,dx0=content[ci].dx0,dy0=content[ci].dy0;
    int can_cache=box<=256;
    int hit=fast_hit||(can_cache&&sprite_cache[ci].valid&&sprite_cache[ci].rw==rw&&
        sprite_cache[ci].rh==rh&&sprite_cache[ci].box==box&&
        !memcmp(sprite_cache[ci].black,black,rw*rh*2)&&
        !memcmp(sprite_cache[ci].white,white,rw*rh*2));
    for(int dy=0;dy<box;dy++)for(int dx=0;dx<box;dx++){
        int ox=dx0+dx,oy=dy0+dy;
        if(ox<0||oy<0||ox>=W||oy>=H)continue;
        int a,red,green,blue;
        if(hit){
            uint32_t v=sprite_cache[ci].rgba[dy*box+dx];
            a=v>>16;red=v&31;green=(v>>5)&63;blue=(v>>11)&31;
        }else{
        int fx=((2*dx+1)*content[ci].w*128/box)-128+content[ci].l*256;
        int fy=((2*dy+1)*content[ci].h*128/box)-128+content[ci].t*256;
        if(fx<0)fx=0;if(fy<0)fy=0;
        int sx=fx>>8,sy=fy>>8,wx=fx&255,wy=fy&255;
        if(sx>=rw-1){sx=rw-1;wx=0;}if(sy>=rh-1){sy=rh-1;wy=0;}
        int sx1=sx+(wx>0),sy1=sy+(wy>0);
        int at[4]={sy*rw+sx,sy*rw+sx1,sy1*rw+sx,sy1*rw+sx1};
        int weights[4]={(256-wx)*(256-wy),wx*(256-wy),(256-wx)*wy,wx*wy};
        a=red=green=blue=0;
        for(int k=0;k<4;k++){uint16_t b=black[at[k]];int weight=weights[k];a+=alpha256(b,white[at[k]])*weight;red+=(b>>11)*weight;green+=((b>>5)&63)*weight;blue+=(b&31)*weight;}
        a=(a+32768)>>16;
        red=(red+32768)>>16;green=(green+32768)>>16;blue=(blue+32768)>>16;
        if(can_cache)sprite_cache[ci].rgba[dy*box+dx]=red|(green<<5)|(blue<<11)|(a<<16);
        }
        if(!a)continue;
        uint16_t *dst=pixels+oy*W+ox;uint16_t d=*dst;
        red+=((d>>11)*(256-a)+128)/256;
        green+=(((d>>5)&63)*(256-a)+128)/256;
        blue+=((d&31)*(256-a)+128)/256;
        if(red>31)red=31;if(green>63)green=63;if(blue>31)blue=31;
        *dst=(red<<11)|(green<<5)|blue;
    }
    if(can_cache&&!hit){
        memcpy(sprite_cache[ci].black,black,rw*rh*2);
        memcpy(sprite_cache[ci].white,white,rw*rh*2);
        sprite_cache[ci].rw=rw;sprite_cache[ci].rh=rh;sprite_cache[ci].box=box;sprite_cache[ci].valid=1;
    }
    if(can_cache&&keyable){sprite_keys[ci]=key;sprite_key_valid[ci]=1;}
    else sprite_key_valid[ci]=0;
    hud_tag_mask|=1u<<i;
}
static uint16_t *screen_pixels(int *stride_out){
    if(!screen_buffer)return NULL;
    unsigned char *buf=screen_buffer();if(!buf)return NULL;
    unsigned char *surface=*(void **)buf;if(!surface)return NULL;
    uint16_t *pixels=*(uint16_t **)(surface+0x20);if(!pixels)return NULL;
    int stride=(int)*(uint64_t *)surface;
    pixels+=(int)*(int *)(buf+0x88)*stride;
    if(stride_out)*stride_out=stride;
    return pixels;
}

/* Drawn after the engine's frame: page-switch hint inside the pause menu. */
/* --- small CJK labels for the overlay -----------------------------------
 * The hand-made 5x7 bitmap font only covers Latin/digits, so the few Chinese
 * words drawn on top of the game go through the same stb_truetype instance the
 * engine uses (font.ttf).  gfa_font_draw_line fills an ARGB buffer with glyph
 * coverage in the alpha channel; that is blended into the RGB565 frame here. */
static int utf8_cps(const char *s,uint32_t *out,int max){
    int n=0;
    while(*s&&n<max){
        unsigned char c=(unsigned char)*s++;
        uint32_t cp;
        if(c<0x80){cp=c;}
        else if((c&0xE0)==0xC0){cp=(uint32_t)(c&0x1F)<<6;cp|=(unsigned char)*s++&0x3F;}
        else if((c&0xF0)==0xE0){cp=(uint32_t)(c&0x0F)<<12;cp|=((unsigned char)*s&0x3F)<<6;s++;cp|=(unsigned char)*s++&0x3F;}
        else{cp='?';}
        out[n++]=cp;
    }
    return n;
}
static void draw_cn_text(uint16_t *pixels,int stride,int x,int y,const char *text,float px,uint16_t fg){
    enum{ MAXW=512, MAXH=96 };
    static uint32_t tmp[MAXW*MAXH];
    uint32_t cps[128];
    int n=utf8_cps(text,cps,128);
    if(!n||!gfa_font_ready())return;
    float tw=gfa_font_text_width(px,cps,n);
    int bw=(int)(tw+0.5f)+8;
    int bh=gfa_font_ascent(px)+gfa_font_descent(px)+8;
    if(bw>MAXW)bw=MAXW;
    if(bh>MAXH)bh=MAXH;
    if(bw<8||bh<8)return;
    memset(tmp,0,(size_t)bw*bh*4);
    gfa_font_draw_line(px,cps,n,tmp,bw,bh,4.0f,4.0f+gfa_font_ascent(px),0xFFFFFFFFu);
    int fr=(fg>>11)&31, fg2=(fg>>5)&63, fb=fg&31;
    for(int yy=0;yy<bh;yy++)for(int xx=0;xx<bw;xx++){
        unsigned int a=tmp[yy*bw+xx]>>24;
        if(!a)continue;
        int dx=x+xx,dy=y+yy;
        if(dx<0||dy<0||dx>=W||dy>=H)continue;
        uint16_t *dst=pixels+dy*stride+dx;
        uint16_t d=*dst;
        int dr=(d>>11)&31, dg=(d>>5)&63, db=d&31;
        int nr=(fr*(int)a+dr*(255-(int)a))/255;
        int ng=(fg2*(int)a+dg*(255-(int)a))/255;
        int nb=(fb*(int)a+db*(255-(int)a))/255;
        *dst=(uint16_t)((nr<<11)|(ng<<5)|nb);
    }
}

/* The two four-slot banks share the same physical buttons. The native
   slot indices follow keypad 1/3/7/9, hence R2/B/Y/X in grid order. */
void ui_layout_quick_slot_hint(int x,int y,int w,int h,int index){
    static const char *labels[4]={"R2","B","Y","X"};
    int stride;uint16_t *p=screen_pixels(&stride);if(!p||index<0||index>=8)return;
    const char *label=labels[index%4];
    const int pw=28,ph=20;
    int left=x+(w-pw)/2,top=y+(h-ph)/2;
    for(int j=0;j<ph;j++)for(int i=0;i<pw;i++){
        int dx=2*i-(pw-1),dy=2*j-(ph-1);
        int xx=left+i,yy=top+j;
        if(dx*dx*ph*ph+dy*dy*pw*pw<=pw*pw*ph*ph&&xx>=0&&xx<W&&yy>=0&&yy<H)
            p[yy*stride+xx]=0xffff;
    }
    uint32_t cps[4];int n=utf8_cps(label,cps,4);float size=15.0f;
    int tw=(int)(gfa_font_text_width(size,cps,n)+0.5f);
    int bh=gfa_font_ascent(size)+gfa_font_descent(size)+8;
    draw_cn_text(p,stride,x+(w-tw)/2-4,y+(h-bh)/2,label,size,0x0000);
}

static void draw_action_tag(uint16_t *pixels,int stride,int cx,int top,const char *caption){
    uint32_t cps[16];int n=utf8_cps(caption,cps,16);
    float size=20.0f;
    int tw=(int)(gfa_font_text_width(size,cps,n)+0.5f);
    draw_tag_width(pixels,stride,cx,top,"",tw+24);
    int bh=gfa_font_ascent(size)+gfa_font_descent(size)+8;
    draw_cn_text(pixels,stride,cx-(tw+8)/2,top+(29-bh)/2,caption,size,TAG_TEXT);
}
static void draw_main_menu(void *menu){
    before_main_menu=0;
    main_menu_draw(menu);
}
static uint64_t title_bounds(int index){
    unsigned char *res=**(unsigned char ***)(game_base+0x311b20);
    unsigned char *book=*(void **)(*(unsigned char **)(res+0x30)+0x20);
    book=*(void **)(book+0x68);
    book=*(void **)(book+0x10);
    unsigned char *frames=*(void **)(book+0x18);
    void *frame=(*(void ***)(frames+0x10))[7];
    return bbox_rect(frame,0,index);
}
static int caption_width(const char *caption,float size){
    uint32_t cps[128];int n=utf8_cps(caption,cps,128);
    return (int)(gfa_font_text_width(size,cps,n)+0.5f);
}
void ui_layout_community_label(int x,int y,int width,int height){
    const char *caption="社区";float size=30.0f;
    int tw=caption_width(caption,size);
    int bh=gfa_font_ascent(size)+gfa_font_descent(size)+8;
    int left=x+(width-tw-8)/2,top=y+(height-bh)/2;
    int stride;uint16_t *p=screen_pixels(&stride);
    draw_cn_text(p,stride,left+1,top+1,caption,size,0x0000);
    draw_cn_text(p,stride,left,top,caption,size,0xffff);
}
static void draw_title_credit(void *state){
    (void)state;
    const char *caption="适配：B站阿良Lion";
    float size=20.0f;
    int tw=caption_width(caption,size);
    int bh=gfa_font_ascent(size)+gfa_font_descent(size)+8;
    int stride;uint16_t *p=screen_pixels(&stride);
    draw_cn_text(p,stride,W-tw-12,H-bh-2,caption,size,0x0000);
}
static void draw_title_copyright(void *state,int x,int y){
    (void)state;
    uint64_t packed=title_bounds(1);
    int16_t r[4];memcpy(r,&packed,8);
    const char *caption="Copyright ©2026 GAMEVIL Inc.";
    float size=14.0f;
    int left=x+r[0]+r[2]/2-(caption_width(caption,size)+8)/2;
    int top=y+r[1]-4;
    int stride;uint16_t *p=screen_pixels(&stride);
    draw_cn_text(p,stride,left+1,top+1,caption,size,0x0000);
    draw_cn_text(p,stride,left,top,caption,size,0xffff);
}
/* Preserve the title's native input lock and blink phase, replacing only
   its English bitmap with a centred Chinese caption at the same bounds. */
static void draw_title_continue(void *state,int x,int y){
    before_main_menu=1;
    unsigned char *timer=**(unsigned char ***)(game_base+0x3105b0);
    if(*(int *)(timer+0xc)){
        ((unsigned char *)state)[0x60]=0;
        return;
    }
    unsigned char *(*get_app)(void)=(void *)(game_base+0xd375c);
    if(!(*(int *)(get_app()+0x13c)&4))return;
    ((unsigned char *)state)[0x60]=1;
    (void)x;(void)y;
    const char *caption="按任意键继续";
    uint32_t cps[16];int n=utf8_cps(caption,cps,16);
    float size=52.0f;
    int tw=(int)(gfa_font_text_width(size,cps,n)+0.5f);
    int stride;uint16_t *p=screen_pixels(&stride);
    int bh=gfa_font_ascent(size)+gfa_font_descent(size)+8;
    int left=(W-tw-8)/2,top=H-bh-4;
    draw_cn_text(p,stride,left+3,top+3,caption,size,0x0000);
    draw_cn_text(p,stride,left,top,caption,size,0xfea0);
}

/* Pointer for the virtual mouse: a filled arrow with a 1px dark outline so it
   stays visible on both light and dark game art. */
static void draw_mouse_cursor(uint16_t *pixels,int stride,int x,int y){
    enum{ CW=14, CH=20 };
    static unsigned char m[CH][CW];
    memset(m,0,sizeof(m));
    for(int yy=0;yy<10;yy++)for(int xx=0;xx<=yy&&xx<CW;xx++)m[yy][xx]=1;
    for(int yy=10;yy<CH;yy++)for(int xx=4;xx<=6;xx++)m[yy][xx]=1;
    for(int yy=-1;yy<=CH;yy++)for(int xx=-1;xx<=CW;xx++){
        int inside=(yy>=0&&yy<CH&&xx>=0&&xx<CW)?m[yy][xx]:0;
        if(inside)continue;
        int near=0;
        for(int dy=-1;dy<=1&&!near;dy++)for(int dx=-1;dx<=1;dx++){
            int ny=yy+dy,nx=xx+dx;
            if(ny>=0&&ny<CH&&nx>=0&&nx<CW&&m[ny][nx]){near=1;break;}
        }
        if(near)put(pixels,stride,x+xx,y+yy,0x0000);
    }
    for(int yy=0;yy<CH;yy++)for(int xx=0;xx<CW;xx++)
        if(m[yy][xx])put(pixels,stride,x+xx,y+yy,0xFFFF);
}

void ui_layout_overlay(void){
    unsigned hud_tags=hud_tag_mask;hud_tag_mask=0;
    /* These hints are painted after the engine's frame, so suppress them
       explicitly whenever dialogue, loading or cinematic text owns it. */
    if(z4_story()||cinematic_hints_hidden())return;
    if(before_main_menu&&demo_ui_status!=14)return;
    int want_menu=z4_menu_open();
    int want_char=z4_char_select_open();
    int want_mouse=z4_mouse_active();
    extern int popup_view_active(void);
    unsigned char *ui=*(void **)(game_base+0x314c70);
    int want_actions=want_menu||want_char||want_mouse||popup_view_active()||
                     (ui&&ui[0x23a1])||demo_ui_status!=14;
    /* Z4_HINT=1 forces the class-picker hint on, so it can be inspected
       offscreen without walking to that screen. */
    {static int force=-1;
     if(force<0){const char *v=getenv("Z4_HINT");force=v&&*v&&*v!='0';}
     if(force)want_char=1;}
    if(!want_actions&&!hud_tags)return;
    int stride=0;uint16_t *pixels=screen_pixels(&stride);
    if(!pixels)return;
    if(want_actions){
        draw_action_tag(pixels,stride,650,H-36,"A确认");
        draw_action_tag(pixels,stride,744,H-36,"B取消");
    }
    if(want_mouse){
        int mx=0,my=0;z4_mouse_pos(&mx,&my);
        draw_mouse_cursor(pixels,stride,mx,my);
        return;
    }
    if(want_char){
        return;
    }
    if(!want_menu){
        /* Run after mapped icons; START now shares the lower-third anchor. */
        for(int i=0;!want_actions&&i<NITEMS;i++)if(hud_tags&(1u<<i))
            draw_tag_width(pixels,stride,items[i].cx,
                           items[i].cy+TARGET_SOLID/2-TARGET_SOLID/3,
                           items[i].label,items[i].type==11?34:48);
        return;
    }
    /* The engine still paints its circular touch back button at the right end
       of the tab bar; B does that job, so cover it with the bar's own colour. */
    for(int y=62;y<108;y++){
        uint16_t bar=pixels[y*stride+734];   /* column just left of the icon */
        for(int x=736;x<797;x++)put(pixels,stride,x,y,bar);
    }
    /* page hints sit in the top corners of the menu:  <<L1      R1>>  */
    draw_tag(pixels,stride,72,6,"<<L1");
    draw_tag(pixels,stride,726,6,"R1>>");
}

static void jump(void *entry,void *fn){
    uint32_t p[4]={0x58000050,0xd61f0200,0,0};uintptr_t target=(uintptr_t)fn;
    memcpy(p+2,&target,8);memcpy(entry,p,16);__builtin___clear_cache(entry,(char *)entry+16);
}
void ui_layout_init(void){
    native_zh_init();
    /* GsGetLastJongsungString uses eight four-byte Japanese particles for
       locale 2. Chinese reuses that locale but needs no は/が/を/で suffix.
       The separate Korean/other-language table starts immediately after. */
    memset((void *)(game_base+0x274d60),0,8*4);
    /* Rename the existing translated Japanese slot; retain its native ID. */
    memset((void *)(game_base+0x2784f8),0,20);
    memcpy((void *)(game_base+0x2784f8),"简体中文",12);
    /* Mining and other item pickups share this native Japanese-slot format,
       outside the external language tables. Keep its single %s argument. */
    static const char item_obtained[]="!C获得%s!cFFFFFF！";
    memcpy((void *)(game_base+0x2829e0),item_obtained,sizeof(item_obtained));
    get_ani=game_symbol("_ZN14CZnTouchKeypad6GetAniE16EnumTouchPadTypeb");
    bbox_count=game_symbol("_ZN11CGxPZxFrame19GetBoundingBoxCountEi");
    bbox_rect=game_symbol("_ZN11CGxPZxFrame14GetBoundingBoxEii");
    frame_rect=game_symbol("_ZN9CGxPZxAni23GetCurrentPlayFrameRectEiiR7TGXRECT10enumDrawOPi");
    screen_buffer=game_symbol("GcxGetMainScreenBuffer");
    ani_draw=game_symbol("_ZN9CGxPZxAni4DrawEii10enumDrawOPil");
    /* MenuState::Draw's first four instructions are stack/register only. */
    uint32_t *menu_entry=(void *)(game_base+0x1f30e4);
    uint32_t *menu_trampoline=mmap(NULL,4096,PROT_READ|PROT_WRITE|PROT_EXEC,
                                  MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(menu_trampoline==MAP_FAILED)exit(3);
    memcpy(menu_trampoline,menu_entry,16);
    jump(menu_trampoline+4,menu_entry+4);
    __builtin___clear_cache((char *)menu_trampoline,(char *)menu_trampoline+32);
    main_menu_draw=(void *)menu_trampoline;
    jump(menu_entry,draw_main_menu);
    jump((void *)(game_base+0x1f2a0c),draw_title_continue);
    jump((void *)(game_base+0x1f2b18),draw_title_credit);
    jump((void *)(game_base+0x1f2948),draw_title_copyright);
    jump(game_symbol("_ZN14CZnTouchKeypad19GetKeypadRectOffsetE16EnumTouchPadTypeRiS1_"),layout_offset);
    jump(game_symbol("_ZN14CZnTouchKeypad18GetTouchKeyPadRectE16EnumTouchPadTypei"),layout_rect);
    /* The first 16 bytes in this padding already contain hide05b; use the following 16. */
    jump((void*)(game_base+0x14d038),layout_draw);
    uint32_t bl=0x94000000|(((0x14d038-0x247e3c)/4)&0x3ffffff);
    memcpy((void*)(game_base+0x247e3c),&bl,4);
    __builtin___clear_cache((char*)game_base+0x247e3c,(char*)game_base+0x247e40);
    game_log("UI layout: 5 right rings + pause/shop, quick-save hidden, key tags on\n");
}
/* Ink-and-brass handheld illustration, cached at the physical 640x480 size. */
static uint16_t help_sheet[640*480];
static const uint16_t HELP_INK=0x3922,HELP_GOLD=0xc4cc,HELP_LIGHT=0xe633;
static void help_rect(int x,int y,int w,int h,uint16_t c){
    for(int j=y;j<y+h;j++)for(int i=x;i<x+w;i++)
        if(i>=0&&i<640&&j>=0&&j<480)help_sheet[j*640+i]=c;
}
static void help_line(int x,int y,int xx,int yy,uint16_t c){
    int dx=abs(xx-x),sx=x<xx?1:-1,dy=-abs(yy-y),sy=y<yy?1:-1,e=dx+dy;
    for(;;){help_rect(x,y,1,1,c);if(x==xx&&y==yy)break;
        int e2=2*e;if(e2>=dy){e+=dy;x+=sx;}if(e2<=dx){e+=dx;y+=sy;}}
}
static void help_round(int x,int y,int w,int h,int r,uint16_t c){
    for(int j=0;j<h;j++)for(int i=0;i<w;i++){
        int dx=i<r?r-i:i>=w-r?i-(w-r-1):0;
        int dy=j<r?r-j:j>=h-r?j-(h-r-1):0;
        if(dx*dx+dy*dy<=r*r)help_rect(x+i,y+j,1,1,c);
    }
}
static void help_text(int x,int y,const char *s,int size,uint16_t color){
    draw_cn_text(help_sheet,640,x-4,y-4,s,(float)size,color);
}
static void help_button(int x,int y,const char *s){
    help_round(x-11,y-11,23,23,11,HELP_INK);
    help_round(x-9,y-10,19,19,9,HELP_LIGHT);
    help_text(x-5,y-8,s,13,HELP_INK);
}
static void help_leader(int x,int y,int bend,int xx,int yy){
    help_line(x,y,bend,y,HELP_INK);help_line(bend,y,xx,yy,HELP_INK);
    help_round(xx-2,yy-2,5,5,2,HELP_INK);
}
void ui_layout_help_guide(uint16_t *p,int stride){
    static int ready;
    if(!ready&&gfa_font_ready()){
        help_text(254,111,"掌机按键指南",21,HELP_INK);
        help_line(22,140,618,140,HELP_GOLD);
        /* Shoulder keys and riveted outer shell. */
        help_round(190,202,53,18,5,HELP_INK);help_round(397,202,53,18,5,HELP_INK);
        help_round(198,193,35,13,4,HELP_GOLD);help_round(407,193,35,13,4,HELP_GOLD);
        help_round(173,216,296,127,19,HELP_INK);
        help_round(175,211,290,127,18,HELP_GOLD);
        help_round(178,214,284,119,16,HELP_LIGHT);
        help_round(182,218,276,111,13,0xa409);
        help_round(236,223,168,91,6,HELP_INK);
        help_round(240,227,160,83,3,0x6b65);
        help_line(244,231,396,231,0x9c6a);
        help_text(268,247,"ZENONIA 4",18,HELP_LIGHT);
        help_text(251,275,"短按攻击 · 长按蓄力",12,HELP_LIGHT);
        /* Direction pad, dual sticks, and Nintendo-layout face buttons. */
        help_round(203,237,12,36,2,HELP_INK);help_round(191,249,36,12,2,HELP_INK);
        help_rect(206,241,6,28,HELP_GOLD);help_rect(195,252,28,6,HELP_GOLD);
        help_button(209,304,"");help_round(203,297,13,13,6,0x7b86);
        help_button(430,304,"");help_round(424,297,13,13,6,0x7b86);
        help_button(430,240,"X");help_button(449,259,"A");
        help_button(411,259,"Y");help_button(430,278,"B");
        help_round(269,319,25,9,4,HELP_INK);help_round(346,319,25,9,4,HELP_INK);
        help_text(264,331,"SELECT",9,HELP_INK);help_text(346,331,"START",9,HELP_INK);
        for(int i=0;i<3;i++){help_line(244,320+i*3,256,320+i*3,HELP_INK);
                            help_line(384,320+i*3,396,320+i*3,HELP_INK);}
        /* Labels sit outside the device; each leader ends at its control. */
        help_text(22,150,"L2  打开商店",15,HELP_INK);
        help_leader(132,172,162,215,198);
        help_text(22,183,"L1  快速保存",15,HELP_INK);
        help_text(22,202,"菜单中：上一页",13,HELP_INK);
        help_leader(133,223,163,195,211);
        help_text(22,249,"方向键／左摇杆",15,HELP_INK);
        help_text(22,269,"移动／菜单选择",14,HELP_INK);
        help_leader(140,290,164,204,303);
        help_text(22,321,"SELECT 世界地图",14,HELP_INK);
        help_leader(150,344,238,279,325);
        help_text(480,150,"R2  快捷键1",15,HELP_INK);
        help_leader(480,172,470,424,198);
        help_text(480,183,"R1  切换快捷栏",14,HELP_INK);
        help_text(480,202,"菜单中：下一页",13,HELP_INK);
        help_leader(480,223,473,446,211);
        help_text(480,239,"A 确认/对话/攻击",14,HELP_INK);
        help_leader(480,259,470,449,259);
        help_text(480,275,"X / Y / B",14,HELP_INK);
        help_text(480,294,"快捷键 2 / 3 / 4",13,HELP_INK);
        help_leader(480,314,470,433,280);
        help_text(480,330,"START 暂停/跳过",14,HELP_INK);
        help_leader(480,350,393,359,325);
        help_line(22,361,618,361,HELP_GOLD);
        help_text(23,369,"B 取消／返回     Y 任务详情／精灵提取／加点适用",14,HELP_INK);
        help_text(23,390,"R3 切换鼠标 · 右摇杆移动 · A 点击 · B 退出鼠标",14,HELP_INK);
        help_text(23,409,"START + SELECT 退出游戏",12,HELP_INK);
        ready=1;
    }
    if(ready)for(int y=104;y<426;y++)for(int x=16;x<624;x++)
        if(help_sheet[y*640+x])p[y*stride+x]=help_sheet[y*640+x];
}
