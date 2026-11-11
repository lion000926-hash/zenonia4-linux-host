#define _GNU_SOURCE
#include "demo.h"
#include "compress_blit.h"
#include "profile.h"
#include "rgb565_upload.h"
#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#include <time.h>

void game_log(const char *fmt,...){va_list ap;va_start(ap,fmt);vfprintf(stderr,fmt,ap);va_end(ap);fflush(stderr);}

/*
 * Preserve the existing 800x600 UI canvas and its 640x480 presentation.
 * scene_view.c separately presents world pixels at 1:1 before UI drawing.
 */
#ifndef GAME_W
#define GAME_W 800
#endif
#ifndef GAME_H
#define GAME_H 600
#endif
/* 设备信息尺寸：UI 层排版参考。默认与画面一致，可单独覆盖做实验 */
#ifndef UI_W
#define UI_W GAME_W
#endif
#ifndef UI_H
#define UI_H GAME_H
#endif
static void fatal_signal(int sig,siginfo_t *info,void *context){
    ucontext_t *uc=context;uintptr_t pc=uc->uc_mcontext.pc,lr=uc->uc_mcontext.regs[30];
    game_log("\nCRASH signal=%d address=%p pc=%p game_offset=0x%lx lr=%p lr_offset=0x%lx\n",sig,info->si_addr,(void*)pc,pc-game_base,(void*)lr,lr-game_base);
    Dl_info host;if(dladdr((void*)pc,&host))game_log("HOST module=%s symbol=%s\n",host.dli_fname,host.dli_sname?host.dli_sname:"?");
    if(dladdr((void*)lr,&host))game_log("LR module=%s symbol=%s offset=0x%lx\n",host.dli_fname,host.dli_sname?host.dli_sname:"?",lr-(uintptr_t)host.dli_fbase);
    for(int i=0;i<31;i++)game_log("x%d=0x%lx%s",i,(unsigned long)uc->uc_mcontext.regs[i],i%4==3?"\n":" ");
    game_log("\n");_exit(128+sig);
}
#define SDL_API(F) __typeof__(SDL_##F) *F;
static struct{
    SDL_API(Init) SDL_API(GetError) SDL_API(Quit) SDL_API(CreateWindow) SDL_API(DestroyWindow)
    SDL_API(PollEvent) SDL_API(Delay) SDL_API(GetTicks)
    SDL_API(NumJoysticks) SDL_API(IsGameController) SDL_API(GameControllerOpen) SDL_API(GameControllerGetButton)
    SDL_API(GameControllerGetAxis) SDL_API(GameControllerName) SDL_API(GameControllerAddMappingsFromRW) SDL_API(RWFromFile)
    SDL_API(GetKeyboardState) SDL_API(GetMouseState) SDL_API(ShowCursor)
    SDL_API(ConvertPixels) SDL_API(CreateRenderer) SDL_API(CreateTexture) SDL_API(UpdateTexture) SDL_API(RenderClear)
    SDL_API(SetRenderDrawColor) SDL_API(RenderCopy) SDL_API(RenderPresent) SDL_API(RenderReadPixels)
    SDL_API(DestroyTexture) SDL_API(DestroyRenderer) SDL_API(GetRendererInfo)
}S;
static void *sdl_handle;
static int load_sdl(void){
    sdl_handle=dlopen("libSDL2-2.0.so.0",RTLD_NOW|RTLD_GLOBAL);
    if(!sdl_handle){game_log("SDL load failed %s\n",dlerror());return -1;}
#define LOAD(F) S.F=dlsym(sdl_handle,"SDL_"#F);if(!S.F){game_log("Missing SDL_"#F"\n");return -1;}
    LOAD(Init) LOAD(GetError) LOAD(Quit) LOAD(CreateWindow) LOAD(DestroyWindow)
    LOAD(PollEvent) LOAD(Delay) LOAD(GetTicks)
    LOAD(NumJoysticks) LOAD(IsGameController) LOAD(GameControllerOpen) LOAD(GameControllerGetButton)
    LOAD(GameControllerGetAxis) LOAD(GameControllerName) LOAD(GameControllerAddMappingsFromRW) LOAD(RWFromFile)
    LOAD(GetKeyboardState) LOAD(GetMouseState) LOAD(ShowCursor)
    LOAD(ConvertPixels) LOAD(CreateRenderer) LOAD(CreateTexture) LOAD(UpdateTexture) LOAD(RenderClear)
    LOAD(SetRenderDrawColor) LOAD(RenderCopy) LOAD(RenderPresent) LOAD(RenderReadPixels)
    LOAD(DestroyTexture) LOAD(DestroyRenderer) LOAD(GetRendererInfo)
    return 0;
}
static UploadColors upload_colors;
static uint32_t upload_pixels[640*480];
static int init_upload_colors(void){
    uint16_t src[128];uint32_t dst[128];
    for(int i=0;i<32;i++){src[i]=i<<11;src[96+i]=i;}
    for(int i=0;i<64;i++)src[32+i]=i<<5;
    if(S.ConvertPixels(128,1,SDL_PIXELFORMAT_RGB565,src,256,SDL_PIXELFORMAT_ABGR8888,dst,512))return 0;
    for(int i=0;i<32;i++){upload_colors.r[i]=dst[i]&255;upload_colors.b[i]=(dst[96+i]>>16)&255;}
    for(int i=0;i<64;i++)upload_colors.g[i]=(dst[32+i]>>8)&255;
    return 1;
}

typedef void(*InputFn)(JNIEnv*,jobject,jint,jlong,jlong,jlong);
static InputFn input;
static SDL_Renderer *renderer;
static void capture_to(const char*path){
    {extern void scene_view_capture(const char *);scene_view_capture(path);}
    unsigned char*p=malloc(640*480*4);
    if(S.RenderReadPixels(renderer,NULL,SDL_PIXELFORMAT_ABGR8888,p,640*4)){game_log("Capture: %s\n",S.GetError());free(p);return;}
    FILE*f=fopen(path,"wb");if(f){fprintf(f,"P6\n640 480\n255\n");for(int y=0;y<480;y++)for(int x=0;x<640;x++)fwrite(p+(y*640+x)*4,1,3,f);fclose(f);}free(p);
}
static void capture(void){capture_to("logs/frame.ppm");}
static void capture_page(int n){char p[64];snprintf(p,sizeof(p),"logs/page%d.ppm",n);capture_to(p);}

/* Scripted key pad for --keys: Z4_KEYS="600:start,640:r1,760:up,800:cap2" */
static struct{int frame,kind,arg,arg2;}steps[64];
static int step_count;
static int test_view;
static int release_at[Z4_PAD_COUNT];
static int step_by_name(const char*n){
    static const char *names[Z4_PAD_COUNT]={"up","down","left","right","a","b","x","y","l1","r1","l2","r2","start","select"};
    for(int i=0;i<Z4_PAD_COUNT;i++)if(!strcmp(n,names[i]))return i;
    return -1;
}
static void parse_steps(void){
    const char*s=getenv("Z4_KEYS");if(!s)return;
    char buf[512];snprintf(buf,sizeof(buf),"%s",s);
    for(char*t=strtok(buf,",");t&&step_count<64;t=strtok(NULL,",")){
        int f,arg=0,arg2=0;char name[32]={0};
        if(sscanf(t,"%d:%31s",&f,name)!=2)continue;
        {size_t nl=strlen(name);if(nl>2&&!strcmp(name+nl-2,"_h")){name[nl-2]=0;arg=1;}
         else if(name[0]=='-'){memmove(name,name+1,strlen(name));arg=2;}}
        int kind=step_by_name(name);
        if(kind<0){
            if(!strncmp(name,"cap",3)){kind=100;arg=atoi(name+3);}
            else if(!strcmp(name,"log"))kind=101;
            else if(!strcmp(name,"quit"))kind=102;
            else if(!strcmp(name,"dump"))kind=103;
            else if(!strcmp(name,"say"))kind=105;
            else if(!strcmp(name,"notice"))kind=106;
            else if(!strcmp(name,"loading"))kind=107;
            else if(!strcmp(name,"blacktext"))kind=108;
            else if(!strcmp(name,"normal"))kind=109;
            else if(!strcmp(name,"revive"))kind=110;
            else if(!strncmp(name,"tap",3)){kind=104;sscanf(name+3,"%d_%d",&arg,&arg2);}
            else continue;
        }
        steps[step_count].frame=f;steps[step_count].kind=kind;steps[step_count].arg=arg;steps[step_count].arg2=arg2;step_count++;
    }
    game_log("KEYS parsed %d steps\n",step_count);
}
static void run_steps(int frame){
    for(int i=0;i<step_count;i++){
        if(steps[i].frame!=frame)continue;
        int k=steps[i].kind;
        if(k<Z4_PAD_COUNT){
            if(steps[i].arg==2){game_log("KEYS release %d\n",k);input_route_press(k,0);release_at[k]=0;continue;}
            game_log("KEYS tap %d%s\n",k,steps[i].arg?" hold":"");
            input_route_press(k,1);if(!steps[i].arg)release_at[k]=frame+2;
        }else if(k==100){game_log("KEYS cap%d menu=%d page=%d depth=%d state=%d story=%d\n",steps[i].arg,z4_menu_open(),z4_menu_page(),z4_menu_depth(),z4_menu_state(),z4_story());capture_page(steps[i].arg);}
        else if(k==103){game_log("KEYS dump menu=%d depth=%d story=%d\n",z4_menu_open(),z4_menu_depth(),z4_story());input_route_dump_slots();}
        else if(k==104){
            game_log("KEYS tap screen %d,%d\n",steps[i].arg,steps[i].arg2);
            input(demo_env,NULL,23,steps[i].arg,steps[i].arg2,0);
            input(demo_env,NULL,24,steps[i].arg,steps[i].arg2,0);
        }
        else if(k==105){
            void *ui=*(void **)(game_base+0x314c70);
            void *mgr=**(void ***)(game_base+0x311668);
            void *player=*(void **)((char *)mgr+0x10);
            void (*open_say)(void *,void *,const char *,int,int)=(void *)(game_base+0x163c48);
            open_say((char *)ui+8,player,"这是对话显示测试。立绘、名字和正文现在按屏幕像素显示，文字保持原始大小。较长的句子需要正确换行和分页，菜单里的字体与布局保持原来的设置。",0,0);
        }
        else if(k==106){
            int (*notice)(int,int,int,const char *,int,int,int,int,int)=(void *)(game_base+0x1f07d0);
            void (*title)(const char *)=(void *)(game_base+0x1ed0b8);
            notice(400,300,0,"!cFFFF00回到村子吧。!N!N!cFFFFFF取得了5个青铜，回到阿雷斯村庄吧。",-1,400,300,1,0);
            title("任务开始");
            unsigned char *mgr=*(void **)(game_base+0x314c88);
            int n=*(int *)(mgr+0x30);
            if(n>0){
                void **stack=*(void ***)(mgr+0x28);
                unsigned char *popup=stack[n-1];
                void *(*add)(void *,int,int,int,int,int,void *,unsigned,unsigned)=(void *)(game_base+0x151730);
                add(popup,3,320,410,160,36,NULL,0xffff,0x39e7);
                add(popup,4,400,418,-1,-1,"!C确认",0xffff,0);
                *(int16_t *)(popup+0x36)=0;
            }
        }
        else if(k>=107&&k<=109)test_view=k==109?0:k;
        else if(k==110){
            test_view=0;
            void *mgr=**(void ***)(game_base+0x311668);
            void *player=*(void **)((char *)mgr+0x10);
            void (*revive)(void *)=(void *)(game_base+0x1c4a5c);
            revive(player);
        }
        else if(k==101)game_log("KEYS log menu=%d page=%d depth=%d state=%d story=%d\n",z4_menu_open(),z4_menu_page(),z4_menu_depth(),z4_menu_state(),z4_story());
    }
    for(int i=0;i<Z4_PAD_COUNT;i++)if(release_at[i]&&frame>=release_at[i]){input_route_press(i,0);release_at[i]=0;}
}
static void input_key(int key,int pressed){input(demo_env,NULL,pressed?2:3,key,0,0);}
static int tap_x,tap_y;
void demo_touch_tap(int x,int y){
    extern int popup_view_active(void);extern void popup_view_touch(int *,int *);
    if(popup_view_active())popup_view_touch(&x,&y);
    else{extern int cinematic_status_touch(int *,int *);cinematic_status_touch(&x,&y);}
    tap_x=x;tap_y=y;input(demo_env,NULL,23,x,y,0);
}
void demo_touch_release(int x,int y){input(demo_env,NULL,24,tap_x,tap_y,0);}

/* One row per physical button.  axis >= 0 means the button is an analog
   trigger or stick direction; neg selects which side of the axis counts. */
typedef struct{int id,button,scancode,axis,neg;}Bind;
static const Bind binds[]={
    {Z4_R_UP,    SDL_CONTROLLER_BUTTON_DPAD_UP,         SDL_SCANCODE_UP,     SDL_CONTROLLER_AXIS_LEFTY, 1},
    {Z4_R_DOWN,  SDL_CONTROLLER_BUTTON_DPAD_DOWN,       SDL_SCANCODE_DOWN,   SDL_CONTROLLER_AXIS_LEFTY, 0},
    {Z4_R_LEFT,  SDL_CONTROLLER_BUTTON_DPAD_LEFT,       SDL_SCANCODE_LEFT,   SDL_CONTROLLER_AXIS_LEFTX, 1},
    {Z4_R_RIGHT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,      SDL_SCANCODE_RIGHT,  SDL_CONTROLLER_AXIS_LEFTX, 0},
    {Z4_R_A,     SDL_CONTROLLER_BUTTON_A,               SDL_SCANCODE_Z,      -1,0},
    {Z4_R_B,     SDL_CONTROLLER_BUTTON_B,               SDL_SCANCODE_X,      -1,0},
    {Z4_R_X,     SDL_CONTROLLER_BUTTON_X,               SDL_SCANCODE_A,      -1,0},
    {Z4_R_Y,     SDL_CONTROLLER_BUTTON_Y,               SDL_SCANCODE_S,      -1,0},
    {Z4_R_L1,    SDL_CONTROLLER_BUTTON_LEFTSHOULDER,    SDL_SCANCODE_Q,      -1,0},
    {Z4_R_R1,    SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,   SDL_SCANCODE_W,      -1,0},
    {Z4_R_L2,    -1,                                    SDL_SCANCODE_1,      SDL_CONTROLLER_AXIS_TRIGGERLEFT,  0},
    {Z4_R_R2,    -1,                                    SDL_SCANCODE_3,      SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 0},
    {Z4_R_START, SDL_CONTROLLER_BUTTON_START,           SDL_SCANCODE_RETURN, -1,0},
    {Z4_R_SELECT,SDL_CONTROLLER_BUTTON_BACK,            SDL_SCANCODE_TAB,    -1,0},
};
#define BIND_COUNT ((int)(sizeof(binds)/sizeof(binds[0])))
int main(int argc,char**argv){
    setvbuf(stderr,NULL,_IONBF,0);
    struct sigaction act={.sa_sigaction=fatal_signal,.sa_flags=SA_SIGINFO};
    sigaction(SIGSEGV,&act,NULL);sigaction(SIGBUS,&act,NULL);sigaction(SIGILL,&act,NULL);sigaction(SIGABRT,&act,NULL);
    game_log("泽诺尼亚传奇IV · R36S CPU opt (ARM64 Linux)\n");
    dlopen("libm.so.6",RTLD_NOW|RTLD_GLOBAL);
    const char*lib=getenv("Z4_LIB");if(!lib||!*lib)lib="libgameDSO.so";
    game_log("LIB %s\n",lib);
    if(game_load(lib))return 3;
    profile_init();
    extern void ui_layout_init(void);
    ui_layout_init();
    extern void scene_view_init(void);
    scene_view_init();
    extern void popup_layout_init(void);
    popup_layout_init();
    extern void dialogue_view_init(void);
    dialogue_view_init();
    extern void popup_view_init(void);
    popup_view_init();
    extern void cinematic_view_init(void);
    cinematic_view_init();
    if(argc>1&&!strcmp(argv[1],"--check-compress")){
        int rc=compress_blit_pixel_check(game_base);
        game_log("CHECK-COMPRESS %s\n",rc?"FAIL":"PASS");
        return rc;
    }
    compress_blit_init();
    demo_jni_init();
    jint(*onload)(JavaVM*,void*)=game_symbol("JNI_OnLoad");
    game_log("JNI_OnLoad returned 0x%x\n",onload(demo_vm,NULL));
    if(argc>1&&!strcmp(argv[1],"--probe")){game_log("PROBE PASS: loaded, relocated, constructors, JNI ready\n");return 0;}
    if(load_sdl())return 4;
    /* R36S GPU presentation is the default; launchers may explicitly select
       software for comparison with the preserved previous version. */
    if(!getenv("SDL_RENDER_DRIVER"))setenv("SDL_RENDER_DRIVER","opengles2",1);
    if(S.Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO|SDL_INIT_GAMECONTROLLER|SDL_INIT_EVENTS)){game_log("SDL Init: %s\n",S.GetError());return 4;}
    audio_init();
    SDL_Window*w=S.CreateWindow("泽诺尼亚传奇IV",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,640,480,SDL_WINDOW_FULLSCREEN);
    if(!w){game_log("Window: %s\n",S.GetError());return 5;}
    renderer=S.CreateRenderer(w,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
    if(!renderer){game_log("Renderer: %s\n",S.GetError());return 6;}
    SDL_RendererInfo ri;S.GetRendererInfo(renderer,&ri);game_log("SDL renderer=%s\n",ri.name);
    SDL_Texture*texture=S.CreateTexture(renderer,SDL_PIXELFORMAT_ABGR8888,SDL_TEXTUREACCESS_STREAMING,640,480);
    if(!texture){game_log("Texture: %s\n",S.GetError());return 6;}
    if(!init_upload_colors()){game_log("Upload color table: %s\n",S.GetError());return 6;}
    game_log("UPLOAD fused NEON nearest 800x600 RGB565 -> 640x480 ABGR8888; SDL conversion bypassed\n");
    S.ShowCursor(SDL_ENABLE);
    SDL_RWops*rw=S.RWFromFile("/roms/ports/PortMaster/gamecontrollerdb.txt","rb");
    if(rw)S.GameControllerAddMappingsFromRW(rw,1);
    SDL_GameController*pad=NULL;
    for(int i=0;i<S.NumJoysticks();i++)if(S.IsGameController(i)){pad=S.GameControllerOpen(i);break;}
    game_log("Controller=%s\n",pad?S.GameControllerName(pad):"none");
    void(*initrefs)(JNIEnv*,jobject)=game_symbol("Java_com_gamevil_nexus2_Natives_InitializeJNIGlobalRef");
    void(*initbuf)(JNIEnv*,jobject,jint,jint)=game_symbol("Java_com_gamevil_nexus2_Natives_NativeInitWithBufferSize");
    void(*initdev)(JNIEnv*,jobject,jint,jint)=game_symbol("Java_com_gamevil_nexus2_Natives_NativeInitDeviceInfo");
    void(*resize)(JNIEnv*,jobject,jint,jint)=game_symbol("Java_com_gamevil_nexus2_Natives_NativeResize");
    void(*resume)(JNIEnv*,jobject)=game_symbol("Java_com_gamevil_nexus2_Natives_NativeResumeClet");
    void(*tick)(void)=game_symbol("threadCallback");
    void*(*device_info)(void)=game_symbol("getDeviceInfo");
    input=game_symbol("Java_com_gamevil_nexus2_Natives_handleCletEvent");
    game_log("STAGE initrefs\n");initrefs(demo_env,NULL);
    game_log("STAGE initbuf %dx%d\n",GAME_W,GAME_H);initbuf(demo_env,NULL,GAME_W,GAME_H);
#ifdef UI_OVERRIDE_W
    /*
     * 实验：渲染缓冲用 GAME_W/GAME_H（例如 640x480，1:1 锐利），
     * 但把平台/字体/屏幕的逻辑尺寸改成 UI 层期望的值（例如 960x544）。
     * 这三个函数在 NativeInitWithBufferSize 内部被调用，地址来自反汇编。
     */
    {
        void(*setplat)(int,int)=(void*)(game_base+0x0ce2fc);
        void(*setdev)(int,int)=(void*)(game_base+0x0d68b4);
        void(*setscr)(int,int)=(void*)(game_base+0x0d685c);
        setplat(UI_OVERRIDE_W,UI_OVERRIDE_H);
        setdev(UI_OVERRIDE_W,UI_OVERRIDE_H);
        setscr(UI_OVERRIDE_W,UI_OVERRIDE_H);
        game_log("STAGE ui-override %dx%d (buffer %dx%d)\n",UI_OVERRIDE_W,UI_OVERRIDE_H,GAME_W,GAME_H);
    }
#endif
    game_log("STAGE initdev %dx%d\n",UI_W,UI_H);initdev(demo_env,NULL,UI_W,UI_H);
    game_log("STAGE resize\n");resize(demo_env,NULL,GAME_W,GAME_H);
    game_log("STAGE resume\n");resume(demo_env,NULL);
    input_route_init(input_key);
    game_log("STAGE frame loop\n");
    int running=1,frame=0,previous[BIND_COUNT]={0},mouse_old=0,r3_old=0;unsigned started=S.GetTicks();
    int smoke=argc>1&&!strcmp(argv[1],"--smoke");
    int menu_test=argc>1&&!strcmp(argv[1],"--menu");
    int resume_saved=argc>1&&!strcmp(argv[1],"--resume");
    int keytest=argc>1&&!strcmp(argv[1],"--keytest");
    int keymode=argc>1&&!strcmp(argv[1],"--keys");
    int menu=menu_test||resume_saved||keytest||keymode;
    if(menu_test||keytest||keymode)pad=NULL;
    parse_steps();
    int last_step=step_count?steps[step_count-1].frame:0;
    int game=argc>1&&!strcmp(argv[1],"--game");
    int max_frames=smoke?420:argc>2&&!strcmp(argv[1],"--frames")?atoi(argv[2]):0;
    while(running){
        profile_frame_begin();uint64_t phase=profile_now();
        unsigned start=S.GetTicks();SDL_Event ev;
        while(S.PollEvent(&ev))if(ev.type==SDL_QUIT||(ev.type==SDL_KEYDOWN&&ev.key.keysym.sym==SDLK_ESCAPE))running=0;
        const Uint8*keys=S.GetKeyboardState(NULL);
        for(int i=0;i<BIND_COUNT;i++){
            int down=keys[binds[i].scancode]?1:0;
            if(!down&&pad){
                if(binds[i].button>=0&&S.GameControllerGetButton(pad,binds[i].button))down=1;
                else if(binds[i].axis>=0){
                    int v=S.GameControllerGetAxis(pad,binds[i].axis);
                    down=binds[i].neg?(v<-16000):(v>16000);
                }
            }
            if(down!=previous[i]){input_route_press(binds[i].id,down);previous[i]=down;}
        }
        if(pad){
            int r3=S.GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_RIGHTSTICK);
            if(r3!=r3_old){input_route_r3(r3);r3_old=r3;}
            input_route_stick(S.GameControllerGetAxis(pad,SDL_CONTROLLER_AXIS_RIGHTX),
                              S.GameControllerGetAxis(pad,SDL_CONTROLLER_AXIS_RIGHTY));
        }
        input_route_frame(frame);
        if(input_route_quit())running=0;
        int mx,my;unsigned mouse=S.GetMouseState(&mx,&my)&SDL_BUTTON_LMASK;
        if(mouse||mouse_old){
            static int native_mouse,status_mouse;
            extern int popup_view_active(void);extern void popup_view_touch(int *,int *);
            if(mouse&&!mouse_old){native_mouse=popup_view_active();status_mouse=0;}
            int tx=mx*GAME_W/640,ty=my*GAME_H/480;
            if(native_mouse)popup_view_touch(&tx,&ty);
            else if(mouse&&!mouse_old){extern int cinematic_status_touch(int *,int *);status_mouse=cinematic_status_touch(&tx,&ty);}
            else if(status_mouse){tx=tx*4/5;ty=ty*4/5;}
            input(demo_env,NULL,mouse?(mouse_old?25:23):24,tx,ty,0);
        }
        mouse_old=mouse;
        if(smoke&&frame==45){game_log("SMOKE title confirm press\n");input_key(-5,1);}
        if(smoke&&frame==46)input_key(-5,0);
        if(smoke&&frame==190)input_key(-5,1);
        if(smoke&&frame==191)input_key(-5,0);
        if(smoke&&frame==280)input_key(35,1);
        if(smoke&&frame==281)input_key(35,0);
        /* --menu：只按到主菜单，用来验证 Continue 和槽位，不进游戏 */
        if(menu&&frame==150){game_log("MENU press confirm (dismiss splash)\n");input_key(-5,1);}
        if(menu&&frame==152)input_key(-5,0);
        if(menu&&frame==300){game_log("MENU touch Continue\n");input(demo_env,NULL,23,131,219,0);}
        if(menu&&frame==304)input(demo_env,NULL,24,131,219,0);
        if(menu&&frame==360){game_log("MENU touch NOTICE ok\n");input(demo_env,NULL,23,400,375,0);}
        if(menu&&frame==364)input(demo_env,NULL,24,400,375,0);
        if(menu&&frame==420){game_log("MENU touch Continue #2\n");input(demo_env,NULL,23,131,219,0);}
        if(menu&&frame==424)input(demo_env,NULL,24,131,219,0);
        if(menu&&frame==480){game_log("MENU tap slot1\n");input(demo_env,NULL,23,250,188,0);}
        if(menu&&frame==484)input(demo_env,NULL,24,250,188,0);
        if(menu&&frame==540){game_log("MENU tap load\n");input(demo_env,NULL,23,400,465,0);}
        if(menu&&frame==544)input(demo_env,NULL,24,400,465,0);
        /* --game：点“新游戏”并关掉公告，进到有虚拟控件的场景 */
        if(game&&frame==150){game_log("GAME dismiss splash\n");input_key(-5,1);}
        if(game&&frame==152)input_key(-5,0);
        if(game&&frame==260){game_log("GAME tap NewGame\n");input(demo_env,NULL,23,125,140,0);}
        if(game&&frame==264)input(demo_env,NULL,24,125,140,0);
        if(game&&frame==330){game_log("GAME tap popup ok\n");input(demo_env,NULL,23,400,375,0);}
        if(game&&frame==334)input(demo_env,NULL,24,400,375,0);
        if(game&&frame==400){game_log("GAME tap 2\n");input(demo_env,NULL,23,400,375,0);}
        if(game&&frame==404)input(demo_env,NULL,24,400,375,0);
        if(game&&frame==470){game_log("GAME tap 3\n");input(demo_env,NULL,23,400,375,0);}
        if(game&&frame==474)input(demo_env,NULL,24,400,375,0);
        /* --keytest：走新按键路由，打开暂停菜单并逐页截图，用来核对页序与状态判定 */
        if(keymode){
            run_steps(frame);
            if(step_count&&frame==last_step+120)running=0;
        }
        if(keytest){
            if(frame==300||frame==620)input_route_dump_slots();
            if(frame==600){game_log("KEYTEST START press (open pause)\n");input_route_press(Z4_R_START,1);}
            if(frame==606)input_route_press(Z4_R_START,0);
            for(int n=0;n<7;n++){
                int t=660+n*100;
                if(frame==t){game_log("KEYTEST page%d state menu=%d page=%d\n",n,z4_menu_open(),z4_menu_page());capture_page(n);}
                if(frame==t+40&&n<6){game_log("KEYTEST R1 (tab next)\n");input_route_press(Z4_R_R1,1);}
                if(frame==t+44&&n<6)input_route_press(Z4_R_R1,0);
            }
            if(frame==1300){game_log("KEYTEST START press (leave pause)\n");input_route_press(Z4_R_START,1);}
            if(frame==1306)input_route_press(Z4_R_START,0);
            if(frame==1360){game_log("KEYTEST closed menu=%d\n",z4_menu_open());capture();}
            if(frame==1400)running=0;
        }
        profile_add(P_INPUT,phase);
        {extern void scene_view_begin(void);scene_view_begin();}
        {extern void popup_view_begin(void);popup_view_begin();}
        cinematic_view_begin();
        phase=profile_now();tick();profile_add(P_TICK,phase);
        phase=profile_now();
        if(keymode&&test_view==107){
            unsigned char *map=**(unsigned char ***)(game_base+0x310388);
            void (*draw)(void *,int,int)=(void *)(game_base+0x18c7bc);
            draw(map,map[8],1);
        }else if(keymode&&test_view==108){
            void *info=device_info();memset(*(void **)((char *)info+16),0,GAME_W*GAME_H*2);
            void *ui=*(void **)(game_base+0x314c70);
            void (*draw)(void *,int,int,int,int,const char *,unsigned,int)=(void *)(game_base+0x15d51c);
            draw(ui,80,100,640,300,"黑幕过场文字显示测试。!N!N这段文字应保持清楚、完整，并在黑幕中正常换行。",0xffff,300);
        }
        {extern void popup_view_overlay_begin(void);popup_view_overlay_begin();}
        {extern void ui_layout_overlay(void);ui_layout_overlay();}
        {extern void popup_layout_overlay(void);popup_layout_overlay();}
        {extern void popup_view_overlay_end(void);popup_view_overlay_end();}
        profile_add(P_OVERLAY,phase);
        frame++;
        void *info=device_info();void*pixelbuffer=*(void**)((char*)info+16);
        {extern void *popup_view_present(void *);pixelbuffer=popup_view_present(pixelbuffer);}
        phase=profile_now();
        S.SetRenderDrawColor(renderer,0,0,0,255);S.RenderClear(renderer);
        profile_add(P_COPY,phase);
        if(pixelbuffer){phase=profile_now();upload_rgb565_abgr_640(pixelbuffer,upload_pixels,&upload_colors);profile_add(P_CONVERT,phase);phase=profile_now();S.UpdateTexture(texture,NULL,upload_pixels,640*4);profile_add(P_UPLOAD,phase);SDL_Rect dst={0,0,640,480};phase=profile_now();S.RenderCopy(renderer,texture,NULL,&dst);profile_add(P_COPY,phase);}
        if(menu_test&&frame==660){game_log("MENU hold LEFT\n");input_key(-3,1);}
        if(menu_test&&frame==700){game_log("MENU release LEFT\n");input_key(-3,0);}
        if(menu_test&&frame==705)running=0;
        if(frame==30||frame==120||(menu&&frame==620))capture();
        if(menu&&frame==658)capture_to("logs/pre.ppm");
        if(menu&&frame==662)capture_to("logs/post.ppm");
        if(menu&&frame==690)capture_to("logs/hold.ppm");
        if((game&&frame==560)||(max_frames&&frame==max_frames))capture();
        phase=profile_now();S.RenderPresent(renderer);profile_add(P_PRESENT,phase);
        if(frame%60==0)game_log("FRAME %d UI=%d elapsed_ms=%u\n",frame,demo_ui_status,S.GetTicks()-started);
        if(max_frames&&frame>=max_frames)running=0;
        phase=profile_now();
        unsigned elapsed=S.GetTicks()-start;if(elapsed<33)S.Delay(33-elapsed);
        profile_add(P_WAIT,phase);profile_frame_end();
    }
    game_log("EXIT clean frames=%d\n",frame);
    S.DestroyTexture(texture);S.DestroyRenderer(renderer);S.DestroyWindow(w);S.Quit();return 0;
}
