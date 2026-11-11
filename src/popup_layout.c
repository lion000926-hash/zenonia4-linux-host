#define _GNU_SOURCE
#include "demo.h"
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>

static int (*original_open)(void *);
static void (*original_list)(void *,uint64_t,void *);
static int (*original_item_quick)(void *,int,int,int);
static int (*original_quick_popup)(void *,uint64_t,void *,int);
static void (*original_button_draw)(void *,int,int,int);
static unsigned char *quick_popup;
static void *quick_callback;

/* SetQuickSlot accepts a binding only while CMvApp's drag/mount flag is
   active. The skill popup sets it, but the item's button-driven popup omits
   it. Mirror the skill path without changing the native binding callback. */
static int open_item_quick(void *menu,int item,int selected,int popup_mode){
    int result=original_item_quick(menu,item,selected,popup_mode);
    if(result&&popup_mode){
        unsigned char *(*get_app)(void)=(void *)(game_base+0xd375c);
        get_app()[0x172]=1;
        game_log("POPUP item quick binding enabled\n");
    }
    return result;
}

static unsigned char *top_popup(void){
    void *mgr=*(void **)(game_base+0x314c88);
    if(!mgr)return NULL;
    int n=*(int *)((char *)mgr+0x30);
    if(n<=0)return NULL;
    void **stack=*(void ***)((char *)mgr+0x28);
    return stack[n-1];
}
/* The shared eight-slot popup inserts each button before its item/skill
   image. Drawing the key on that button puts it underneath the mapped icon. */
static int create_quick_popup(void *menu,uint64_t rect,void *callback,int selected){
    int result=original_quick_popup(menu,rect,callback,selected);
    quick_popup=result?top_popup():NULL;
    quick_callback=callback;
    return result;
}
static void draw_quick_button(void *button,int x,int y,int mode){
    original_button_draw(button,x,y,mode);
    unsigned char *p=top_popup();
    if(!p||p!=quick_popup||*(void **)(p+8)!=quick_callback)return;
    unsigned char **objects=*(unsigned char ***)(p+0x20);
    int n=*(int *)(p+0x28),slot=0;
    for(int i=0;i<n;i++)if(objects[i][8]==3){
        if(objects[i]==button){
            int16_t *r=(void *)(objects[i]+0xa);
            if(slot<8)ui_layout_quick_slot_hint(x+r[0],y+r[1],r[2],r[3],slot);
            return;
        }
        slot++;
    }
}

static unsigned char *revive_popup(void){
    unsigned char *p=top_popup();
    return p&&*(uintptr_t *)(p+8)==game_base+0x1d1240?p:NULL;
}

int popup_layout_press(int id,int down){
    unsigned char *p=revive_popup();
    if(!p)return 0;
    if(id!=Z4_R_UP&&id!=Z4_R_DOWN&&id!=Z4_R_LEFT&&id!=Z4_R_RIGHT)return 0;
    if(down){
        /* Native button indices are not in screen order. Select by their
           actual vertical positions while retaining the native action IDs. */
        unsigned char **objects=*(unsigned char ***)(p+0x20);
        int n=*(int *)(p+0x28),ys[3],count=0;
        for(int i=0;i<n&&count<3;i++)if(objects[i][8]==3)
            ys[count++]=*(int16_t *)(objects[i]+0xc);
        if(!count)return 1;
        int current=*(int16_t *)(p+0x36);
        if(current<0||current>=count)current=0;
        int up=id==Z4_R_UP||id==Z4_R_LEFT,next=-1;
        for(int i=0;i<count;i++){
            if(up?ys[i]<ys[current]:ys[i]>ys[current])
                if(next<0||(up?ys[i]>ys[next]:ys[i]<ys[next]))next=i;
        }
        if(next<0){
            next=0;
            for(int i=1;i<count;i++)if(up?ys[i]>ys[next]:ys[i]<ys[next])next=i;
        }
        *(int16_t *)(p+0x36)=next;
        game_log("POPUP revive focus=%d\n",*(int16_t *)(p+0x36));
    }
    return 1;
}

/* This touch-first popup explicitly disables DrawSelect on its textured
   buttons. Draw an outline instead of painting over their text/art. */
void popup_layout_overlay(void){
    extern int popup_view_active(void),popup_view_drawing(void);
    if(popup_view_active()&&!popup_view_drawing())return;
    unsigned char *p=revive_popup();
    if(!p)return;
    void *(*get_button)(void *,int *)=(void *)(game_base+0x150e54);
    int index=0;
    unsigned char *button=get_button(p,&index);
    if(!button)return;
    int16_t *r=(void *)(button+0xa);
    void *(*get_screen)(void)=game_symbol("GcxGetMainScreenBuffer");
    unsigned char *screen=get_screen();
    unsigned char *surface=*(void **)screen;
    int stride=(int)*(uint64_t *)surface;
    uint16_t *pixels=*(uint16_t **)(surface+0x20);
    pixels+=*(int *)(screen+0x88)*stride;
    for(int t=0;t<3;t++){
        int x0=r[0]-2+t,y0=r[1]-2+t,x1=r[0]+r[2]+1-t,y1=r[1]+r[3]+1-t;
        uint16_t color=t==1?0xffe0:0x0000;
        for(int x=x0;x<=x1;x++)if(x>=0&&x<800){
            if(y0>=0&&y0<600)pixels[y0*stride+x]=color;
            if(y1>=0&&y1<600)pixels[y1*stride+x]=color;
        }
        for(int y=y0;y<=y1;y++)if(y>=0&&y<600){
            if(x0>=0&&x0<800)pixels[y*stride+x0]=color;
            if(x1>=0&&x1<800)pixels[y*stride+x1]=color;
        }
    }
}

/* Both the bank and its inventory use the same item-action callback. Keep
   the engine's actions and input handling, replacing only the radial view. */
static int open_context_list(void *menu){
    int page=z4_menu_page();
    if((page==7||page==11)&&z4_menu_open()){
        uint64_t (*cursor_rect)(void *)=(void *)(game_base+0x16e4cc);
        int (*create_list)(void *,uint64_t)=(void *)(game_base+0x16da50);
        game_log("POPUP shop/bank item action list page=%d state=%d\n",page,*(int *)((char *)menu+0x24));
        return create_list(menu,cursor_rect(menu));
    }
    return original_open(menu);
}

/* The shared text-action popup uses textured selection caps. Tiny widths
   make those caps overlap. Widen the panel and every corresponding hitbox
   together, then keep the whole list inside the original 800x600 UI canvas. */
static void make_action_list(void *menu,uint64_t rect,void *callback){
    /* Unlike MvCreatePopup, CreateMainPopup bypasses the native title reset.
       A previous resurrection popup otherwise leaks its shared title here. */
    void (*title_index)(int)=(void *)(game_base+0x1ed544);
    void (*title_msg)(const char *)=(void *)(game_base+0x1ed0b8);
    title_index(-1);
    title_msg(NULL);
    original_list(menu,rect,callback);
    void *mgr=*(void **)(game_base+0x314c88);
    int count=*(int *)((char *)mgr+0x30);
    if(count<=0)return;
    void **stack=*(void ***)((char *)mgr+0x28);
    unsigned char *popup=stack[count-1];
    int n=*(int *)(popup+0x28);
    if(n<=0)return;
    unsigned char **objects=*(unsigned char ***)(popup+0x20);
    unsigned char *bg=objects[0];
    int16_t *bounds=(void *)(bg+0xa);
    int x=bounds[0],y=bounds[1],w=bounds[2],h=bounds[3];
    int width=w<180?180:w;
    if(width>784)width=784;
    int nx=x,ny=y;
    if(nx+width>792)nx=792-width;
    if(nx<8)nx=8;
    if(ny+h>592)ny=592-h;
    if(ny<8)ny=8;
    for(int i=0;i<n;i++){
        unsigned char *obj=objects[i];
        int16_t *r=(void *)(obj+0xa);
        r[0]+=nx-x;r[1]+=ny-y;
        if(i==0)r[2]=width;
        else if(obj[8]==3)r[2]=width-4;
        else if(obj[8]==4&&i>0&&objects[i-1][8]==3){
            const char *text=*(const char **)(obj+0x20);
            /* CreateMainPopup formats every option as !cFFFFFF%s. This is
               a colour command, not eight visible characters. */
            if(!strncmp(text,"!cFFFFFF",8))text+=8;
            float actual_width=0;
            int th=0;
            demo_measure_text(text,&actual_width,&th);
            int tw=(int)(actual_width+0.5f);
            int16_t *row=(void *)(objects[i-1]+0xa);
            r[0]=row[0]+(row[2]-tw)/2;
            r[1]=row[1]+(row[3]-th)/2;
            game_log("POPUP text width=%d row=%d x=%d center=%d\n",tw,row[2],r[0],row[0]+row[2]/2);
        }
    }
    game_log("POPUP list bounds %d,%d %dx%d -> %d,%d %dx%d\n",x,y,w,h,nx,ny,width,h);
}

/* These two prologues contain only register/stack operations. Relocate
   exactly those instructions; no PC-relative instructions are moved. */
static void *hook(unsigned offset,void *fn,const uint32_t expected[4]){
    uint32_t *at=(void *)(game_base+offset);
    if(memcmp(at,expected,16)){game_log("POPUP unsupported prologue %x\n",offset);exit(3);}
    uint32_t *tramp=mmap(NULL,4096,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(tramp==MAP_FAILED){game_log("POPUP trampoline failed\n");exit(3);}
    memcpy(tramp,at,16);
    uintptr_t resume=game_base+offset+16,target=(uintptr_t)fn;
    uint32_t jump[4]={0x58000050,0xd61f0200,0,0};
    memcpy(jump+2,&resume,8);memcpy(tramp+4,jump,16);
    memcpy(jump+2,&target,8);memcpy(at,jump,16);
    __builtin___clear_cache((char *)tramp,(char *)tramp+32);
    __builtin___clear_cache((char *)at,(char *)at+16);
    return tramp;
}
void popup_layout_init(void){
    const uint32_t open_words[4]={0xa9be7bfd,0x910003fd,0xb9419001,0xf9000bf3};
    const uint32_t list_words[4]={0xd10403ff,0x93507c24,0x9360bc23,0xa9047bfd};
    original_list=hook(0x232458,make_action_list,list_words);
    original_open=hook(0x23214c,open_context_list,open_words);
    const uint32_t item_quick_words[4]={0xa9bd7bfd,0x53001c64,0x2a0203e3,0x910003fd};
    original_item_quick=hook(0x16dc68,open_item_quick,item_quick_words);
    original_quick_popup=demo_hook(0x2313d0,create_quick_popup);
    original_button_draw=demo_hook(0x1509a8,draw_quick_button);
    game_log("POPUP quick binding grid key hints below mapped icons\n");
    game_log("POPUP bank/inventory/shop lists enabled\n");
}
