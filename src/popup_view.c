#define _GNU_SOURCE
#include "demo.h"
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>

static void (*draw_popups)(void *);
static void (*draw_frame)(int,int);
static void *(*get_screen)(void);
static uint16_t backdrop[800*600],native[640*480],presentation[800*600];
static int transforming,presentation_ready;
static void *saved_pixels;

int popup_view_active(void){
    unsigned char *mgr=*(void **)(game_base+0x314c88);
    return mgr&&*(int *)(mgr+0x30)>0;
}
int popup_view_drawing(void){return transforming;}
void popup_view_begin(void){presentation_ready=0;}
void *popup_view_present(void *normal){return presentation_ready?presentation:normal;}
void popup_view_overlay_begin(void){
    if(!presentation_ready)return;
    unsigned char *s=get_screen(),*surface=*(void **)s;
    saved_pixels=*(void **)(surface+0x20);
    *(void **)(surface+0x20)=presentation;
}
void popup_view_overlay_end(void){
    if(!presentation_ready)return;
    unsigned char *s=get_screen(),*surface=*(void **)s;
    *(void **)(surface+0x20)=saved_pixels;
}
void popup_view_touch(int *x,int *y){*x=*x*4/5+80;*y=*y*4/5+60;}
static uint16_t *screen_pixels(int *stride){
    unsigned char *s=get_screen(),*surface=*(void **)s;
    *stride=(int)*(uint64_t *)surface;
    return *(uint16_t **)(surface+0x20)+*(int *)(s+0x88)*(*stride);
}
static void align_revive(unsigned char *popup,unsigned char **objects,int n){
    if(*(uintptr_t *)(popup+8)!=game_base+0x1d1240)return;
    int16_t *panel=(void *)(objects[0]+0xa);
    for(int i=0;i<n;i++)if(objects[i][8]==4){
        char *s=*(char **)(objects[i]+0x20);
        size_t len=strlen(s);
        while(len&&s[len-1]==' ')s[--len]=0;
        if(strncmp(s,"!C",2)){
            int missing_bang=s[0]=='C'&&s[1]=='!';
            size_t prefix=missing_bang?1:2;
            void *(*new_array)(size_t)=(void *)(game_base+0xd63d4);
            void (*delete_array)(void *)=(void *)(game_base+0xd63dc);
            char *fixed=new_array(len+prefix+1);
            if(fixed){
                memcpy(fixed,missing_bang?"!":"!C",prefix);
                memcpy(fixed+prefix,s,len+1);
                *(char **)(objects[i]+0x20)=fixed;delete_array(s);
            }
        }
        int16_t *r=(void *)(objects[i]+0xa);
        int button=-1;
        for(int j=0;j<n;j++)if(objects[j][8]==3){
            int16_t *b=(void *)(objects[j]+0xa);
            if(r[1]>=b[1]&&r[1]<b[1]+b[3]){button=j;break;}
        }
        if(button>=0){
            int16_t *b=(void *)(objects[button]+0xa);
            r[0]=b[0]+6;r[2]=b[2]-12;
        }else{
            r[0]=panel[0]+12;r[2]=panel[2]-24;
            int room=panel[1]+panel[3]-r[1]-4;
            if(room>r[3])r[3]=room<40?room:40;
        }
        /* These short labels now fit in the assigned rows. Their old
           constructor-created scroll keymap would leave a stray scrollbar. */
        void *scroll=*(void **)(objects[i]+0x28);
        if(scroll){
            *(void **)(objects[i]+0x28)=NULL;
            void (*destroy_scroll)(void *)=(void *)(game_base+0x13e2c4);
            destroy_scroll(scroll);
        }
    }
}
static void fit_popup(unsigned char *mgr){
    int count=*(int *)(mgr+0x30);
    void **stack=*(void ***)(mgr+0x28);
    unsigned char *popup=stack[count-1];
    int n=*(int *)(popup+0x28);
    if(!n)return;
    unsigned char **objects=*(unsigned char ***)(popup+0x20);
    if(objects[0][8]==0)align_revive(popup,objects,n);
    if(objects[0][8]!=0)return; /* bitmap-only dialogs retain their own centred layout */
    int16_t *b=(void *)(objects[0]+0xa);
    int x=b[0],y=b[1],w=b[2],h=b[3];
    if(w<=0||h<=0)return;
    int nw=w>620?620:w,nh=h>426?426:h;
    int nx=x,ny=y;
    if(nx+nw>710)nx=710-nw;if(nx<90)nx=90;
    if(ny+nh>530)ny=530-nh;if(ny<104)ny=104;
    if(nx==x&&ny==y&&nw==w&&nh==h)return;
    for(int i=0;i<n;i++){
        int16_t *r=(void *)(objects[i]+0xa);
        r[0]=nx+(r[0]-x)*nw/w;r[1]=ny+(r[1]-y)*nh/h;
        if(r[2]>0)r[2]=r[2]*nw/w;
        if(r[3]>0)r[3]=r[3]*nh/h;
    }
}
static void begin_popups(void *mgr){
    transforming=0;
    if(*(int *)((char *)mgr+0x30)>0){
        fit_popup(mgr);
        int stride;uint16_t *p=screen_pixels(&stride);
        for(int y=0;y<600;y++)memcpy(backdrop+y*800,p+y*stride,800*2);
        /* Keep the existing screen behind the popup at exactly its previous
           scale. The popup itself continues using its native engine pixels. */
        for(int y=0;y<480;y++)for(int x=0;x<640;x++)
            p[(y+60)*stride+x+80]=backdrop[((2*y+1)*5/8)*800+(2*x+1)*5/8];
        transforming=1;
    }
    draw_popups(mgr);
}
static void end_popups(int a,int b){
    draw_frame(a,b);
    if(!transforming)return;
    extern void popup_layout_overlay(void);
    popup_layout_overlay();
    int stride;uint16_t *p=screen_pixels(&stride);
    for(int y=0;y<480;y++)memcpy(native+y*640,p+(y+60)*stride+80,640*2);
    for(int y=0;y<600;y++)for(int x=0;x<800;x++)
        presentation[y*800+x]=native[((2*y+1)*2/5)*640+(2*x+1)*2/5];
    /* Engine freeze/backup buffers must retain their original coordinate
       system. Present the enlarged popup separately instead of feeding it
       back into the engine's next frame. */
    for(int y=0;y<600;y++)memcpy(p+y*stride,backdrop+y*800,800*2);
    presentation_ready=1;
    transforming=0;
    static int logged;
    if(!logged++)game_log("POPUPVIEW all common popup layers native 640x480; underlying UI preserved\n");
}
static void jump(uint32_t *at,void *fn){
    uint32_t code[4]={0x58000050,0xd61f0200,0,0};uintptr_t target=(uintptr_t)fn;
    memcpy(code+2,&target,8);memcpy(at,code,16);
}
void popup_view_init(void){
    get_screen=game_symbol("GcxGetMainScreenBuffer");
    uint32_t *ui=(void *)(game_base+0x14fae8),*frame=(void *)(game_base+0x1ed1f8);
    const uint32_t ui_words[4]={0xa9bf7bfd,0x910003fd,0xb9403001,0x34000141};
    const uint32_t frame_words[4]={0xd10243ff,0x90000922,0xa9017bfd,0x910043fd};
    if(memcmp(ui,ui_words,16)||memcmp(frame,frame_words,16)){
        game_log("POPUPVIEW unsupported drawing prologue\n");exit(3);
    }
    uint32_t *mem=mmap(NULL,4096,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(mem==MAP_FAILED)exit(3);
    /* Relocate DrawPopup's CBZ explicitly, preserving its original epilogue. */
    memcpy(mem,ui,12);mem[3]=0x350000a1; /* cbnz w1,+20 -> second absolute branch */
    jump(mem+4,(void *)(game_base+0x14fb1c));
    jump(mem+8,(void *)(game_base+0x14faf8));
    draw_popups=(void *)mem;
    /* Relocate DrawPopupFrame's ADRP as an absolute page-address load. */
    uint32_t *t=mem+16;
    t[0]=frame[0];t[1]=0x580000a2;t[2]=frame[2];t[3]=frame[3];
    t[4]=0x58000090;t[5]=0xd61f0200;
    uintptr_t page=game_base+0x311000,resume=game_base+0x1ed208;
    memcpy(t+6,&page,8);memcpy(t+8,&resume,8);
    draw_frame=(void *)t;
    __builtin___clear_cache((char *)mem,(char *)mem+104);
    jump(ui,begin_popups);jump(frame,end_popups);
    __builtin___clear_cache((char *)ui,(char *)ui+16);
    __builtin___clear_cache((char *)frame,(char *)frame+16);
    game_log("POPUPVIEW native common popup presentation enabled\n");
}
