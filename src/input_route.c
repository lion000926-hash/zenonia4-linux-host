/*
 * Scene-aware physical button routing.
 *
 * The engine already has a complete internal key map; what it lacks is a
 * single physical layout.  This module decides, per press, which native key
 * code (or which menu call) a physical button produces, based on the engine's
 * own state:
 *
 *   SCENE_MENU   pause menu / any main-UI page is showing
 *   SCENE_STORY  a script is running (cutscene)
 *   SCENE_GAME   normal play, including combat
 *
 * Every address below is pinned to the deployed libgameDSO.so build
 * (hide05b, sha256 f78bd00b...ddfc75).  Nothing here is generic.
 */
#define _GNU_SOURCE
#include "demo.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* ---- engine layout of this build ------------------------------------- */
enum{
    SLOT_GAMEUI     = 0x314c70, /* CGsSingleton<CMvGameUI>::ms_pSingleton       */
    SLOT_SCRIPT     = 0x31df40, /* CGsSingleton<CMvGameScriptMgr>::ms_pSingleton */
    SLOT_KEYPAD     = 0x31df38, /* CGsSingleton<CZnTouchKeypad>::ms_pSingleton  */
};
enum{
    ADDR_ISMENU     = 0x15e6cc, /* int CMvGameUI::IsMainMenu()                 */
    ADDR_CHANGETAB  = 0x167798, /* void CMvGameUI::ChangeTabMenu(int)          */
    ADDR_OPENMINI   = 0x160538, /* void CMvGameUI::OpenMiniMap()               */
    ADDR_OPENWORLD  = 0x1606bc, /* void CMvGameUI::OpenWorldMap(int)           */
    ADDR_CLOSEMINI  = 0x15e714, /* void CMvGameUI::CloseMiniMap()              */
    ADDR_CLOSEWORLD = 0x15e720, /* void CMvGameUI::CloseWorldMap()             */
    ADDR_KEYPADMODE = 0x2474bc, /* void CZnTouchKeypad::ChangeKeypadMode(int)  */
    ADDR_CANTUSE    = 0x163670, /* int CMvGameUI::CheckUseFunctionOnCanNotSaveArea(bool) */
    SLOT_UIMGR      = 0x314c88, /* CGsSingleton<CGsUIMgr>::ms_pSingleton        */
    ADDR_DELPOPUPS  = 0x14f0a4, /* void CGsUIMgr::DeletePopupAll()              */
    ADDR_TOUCHRECT  = 0x24f3a4, /* bool CZnCharaterSelectMenu::DoCheckTouchRect(int) */
    ADDR_DOCHANGE   = 0x24f46c, /* void CZnCharaterSelectMenu::DoChangeClass()  */
};
enum{
    UI_DEPTH = 0x23a4,          /* 0..1 menu layers, >1 world            */
    UI_TYPE  = 0x23ac,          /* current menu page 0..6                */
    UI_MENU_OBJ = 0x23b0,       /* current page object                   */
    UI_CHARSEL  = 0x23b8,       /* -> CZnCharaterSelectMenu while it is open */
    UI_MINI  = 0x23a0,          /* mini map showing (byte)               */
    UI_WORLD = 0x23a1,          /* world map showing (byte)              */
    SCRIPT_RUNNING = 0x18,      /* int script depth                      */
};
enum{ PAGE_MAX = 6 };
/* The class-select menu lives inline in CMvGameUI; UI_CHARSEL points at it. */
enum{ CHARSEL_OFFSET = 0x1ed8 };
/* DoCheckTouchRect() indices: 0..3 class slots, 4 = confirm, 5 = cancel. */
enum{ CHARSEL_CONFIRM = 4, CHARSEL_CANCEL = 5 };

/* native Gx key codes the engine understands */
enum{
    K_UP=-1, K_DOWN=-2, K_LEFT=-3, K_RIGHT=-4, K_OK=-5,
    K_MAP=-6, K_SAVE=-10, K_BACK=-16,
    K_SHOP=42, K_SKILL5=49, K_SKILL4=51, K_SKILL3=55, K_SKILL2=57,
    K_TAB=35, K_SKIP=35,
};
/* pseudo codes: resolved to a menu action instead of a key event */
enum{ ACT_TAB_PREV=1000, ACT_TAB_NEXT=1001 };

enum{ SCENE_GAME, SCENE_MENU, SCENE_STORY };

/* grace frames before a lone SELECT/START fires, so a combo can win */
enum{ COMBO_GRACE = 4 };

static void (*emit_key)(int key, int pressed);
static void *(*fn_ismenu)(void *);
static void (*fn_changetab)(void *, int);
static void (*fn_openmini)(void *);
static void (*fn_openworld)(void *, int);
static void (*fn_closemini)(void *);
static void (*fn_closeworld)(void *);
static void (*fn_keypadmode)(void *, int);
static int (*fn_cantuse)(void *, int);
static void (*fn_delpopups)(void *);
static int (*fn_touchrect)(void *, int);
static void (*fn_dochange)(void *);
static void *gameui;
static int resolved = -1;           /* -1 unknown, 0 failed, 1 ok */
static int quit_requested;
static int last_scene = -1;
static int trace = -1;

static int sent[Z4_PAD_COUNT];      /* native code held down per button */
static unsigned frame_no;
static unsigned sel_frame, start_frame;
static int sel_fired, start_fired;
static int pending_up;
static int held_start, held_select, both_held_frames;
/* Virtual mouse: R3 toggles, right stick moves, A clicks, B leaves. */
static int mouse_mode, mouse_x=400, mouse_y=300, mouse_release;
static unsigned mini_restore_frame;   /* rebuild the HUD mini map after BACK */

static uintptr_t addr(uintptr_t off){ return game_base + off; }

/* CGsSingleton<T>::ms_pSingleton holds the instance pointer directly */
static void *singleton(uintptr_t off){
    void *p = *(void **)addr(off);
    return p;
}
static int readable(uintptr_t a, size_t n);

static int plausible(void *obj){
    if(!obj) return 0;
    if(!readable((uintptr_t)obj, 8)) return 0;
    void *vt = *(void **)obj;
    return vt >= (void *)game_base && vt < (void *)(game_base + game_size);
}

/* Engine entry points are pure code addresses: resolve them unconditionally so
   every path below works no matter which state probe succeeded first. */
static void resolve_fns(void){
    if(fn_touchrect) return;
    fn_ismenu      = (void *)addr(ADDR_ISMENU);
    fn_changetab   = (void *)addr(ADDR_CHANGETAB);
    fn_openmini    = (void *)addr(ADDR_OPENMINI);
    fn_openworld   = (void *)addr(ADDR_OPENWORLD);
    fn_closemini   = (void *)addr(ADDR_CLOSEMINI);
    fn_closeworld  = (void *)addr(ADDR_CLOSEWORLD);
    fn_keypadmode  = (void *)addr(ADDR_KEYPADMODE);
    fn_cantuse     = (void *)addr(ADDR_CANTUSE);
    fn_delpopups   = (void *)addr(ADDR_DELPOPUPS);
    fn_touchrect   = (void *)addr(ADDR_TOUCHRECT);
    fn_dochange    = (void *)addr(ADDR_DOCHANGE);
}

static int state_ready(void){
    if(resolved == 1) return 1;
    /* Failure is not cached: the singleton only exists after the engine boots. */
    static int attempts;
    resolve_fns();
    void *ui = singleton(SLOT_GAMEUI);
    if(plausible(ui)){
        int depth = *(int *)((char *)ui + UI_DEPTH);
        int type  = *(int *)((char *)ui + UI_TYPE);
        /* UI_TYPE is -1 whenever no pause page is open, which is the normal
           state in play, so it must not gate the very first resolution. */
        if(depth >= 0 && depth < 64 && type >= -1 && type < 64){
            gameui = ui;
            resolved = 1;
            game_log("ROUTE gameui=%p depth=%d type=%d\n", ui, depth, type);
            return 1;
        }
        if(attempts++ < 20)
            game_log("ROUTE gameui %p rejected depth=%d type=%d\n", ui, depth, type);
    }else if(attempts++ < 20){
        game_log("ROUTE gameui slot not ready (%p)\n", ui);
    }
    return 0;
}

int z4_menu_open(void){
    if(!state_ready()) return 0;
    /* Depth 0..1 is a UI layer (pause pages, shop). 2 and up is the world.
       IsMainMenu() only covers pause pages 0..6 and reports false for the
       shop page (type 7), so the depth field is the reliable signal. */
    int depth = *(int *)((char *)gameui + UI_DEPTH);
    return depth >= 0 && depth <= 1;
}
static int story_active(void){
    void *mgr = singleton(SLOT_SCRIPT);
    if(!plausible(mgr)) return 0;
    return *(int *)((char *)mgr + SCRIPT_RUNNING) > 0;
}
int z4_story(void){ return story_active(); }
static int scene_is_menu(void){ return z4_scene() == SCENE_MENU; }
/* CGsUIMgr keeps its popup stack in a CGsArray: {T* data; int size; int cap} */
static void *ui_manager(void){
    void *mgr = singleton(SLOT_UIMGR);
    return (mgr && readable((uintptr_t)mgr, 0x38)) ? mgr : NULL;
}
static int popup_count(void){
    void *mgr = ui_manager();
    if(!mgr) return 0;
    int n = *(int *)((char *)mgr + 0x30);
    return (n >= 0 && n < 64) ? n : 0;
}
/* The class-select menu is embedded in CMvGameUI; while it is open the game
   keeps a pointer to it. It is a touch-only screen (four class cells plus a
   confirm button), which a handheld without a touch panel cannot drive.
   Resolved directly from the singleton so it still works before any pause
   page has ever been opened. */
static void *char_select_menu(void){
    resolve_fns();
    if(!gameui){
        void *ui = singleton(SLOT_GAMEUI);
        if(plausible(ui)) gameui = ui;
    }
    if(!gameui) return NULL;
    void *m = *(void **)((char *)gameui + UI_CHARSEL);
    return m == (char *)gameui + CHARSEL_OFFSET ? m : NULL;
}
static int char_select_open(void){ return char_select_menu() != NULL; }
int z4_char_select_open(void){ return char_select_open(); }

/* A on this screen commits the highlighted class. The engine's own "通知 /
   是否转变到选择的级别?" dialog is deliberately not raised: it is a touch-only
   widget that cannot be answered without a touch panel, and once it is up the
   screen cannot be left again. */
static void char_select_confirm(void){
    void *menu = char_select_menu();
    if(!menu) return;
    int n = popup_count();
    if(n > 0){
        void *mgr = ui_manager();
        if(fn_delpopups && mgr) fn_delpopups(mgr);
    }
    game_log("ROUTE class-select commit (popups=%d)\n", n);
    if(fn_dochange) fn_dochange(menu);
}
/* B on this screen: the confirmation popup answers "no" by simply going away,
   which returns to the class list so another class can be picked. */
static void char_select_cancel(void){
    void *menu = char_select_menu();
    if(!menu) return;
    if(popup_count() > 0){
        void *mgr = ui_manager();
        game_log("ROUTE class-select dismiss confirm popup\n");
        if(fn_delpopups && mgr) fn_delpopups(mgr);
    }else{
        game_log("ROUTE class-select cancel\n");
        if(fn_touchrect) fn_touchrect(menu, CHARSEL_CANCEL);
    }
}
static int world_map_open(void){
    if(!state_ready()) return 0;
    return ((unsigned char *)gameui)[UI_WORLD]?1:0;
}
int z4_scene(void){
    if(z4_menu_open()) return SCENE_MENU;
    if(story_active()) return SCENE_STORY;
    return SCENE_GAME;
}
int z4_menu_page(void){
    if(!state_ready()) return -1;
    return *(int *)((char *)gameui + UI_TYPE);
}
int z4_menu_depth(void){
    if(!state_ready()) return -1;
    return *(int *)((char *)gameui + UI_DEPTH);
}
/* State field of the currently open menu object (CMvItemMenu+0x24 on page 2). */
int z4_menu_state(void){
    if(!state_ready()) return -1;
    void *menu = *(void **)((char *)gameui + UI_MENU_OBJ);
    if(!menu || !readable((uintptr_t)menu, 0x28)) return -1;
    return *(int *)((char *)menu + 0x24);
}

static void *current_menu(void){
    if(!state_ready())return NULL;
    void *menu=*(void **)((char *)gameui+UI_MENU_OBJ);
    return menu&&readable((uintptr_t)menu,0x238)?menu:NULL;
}
static int shop_popup(void *menu){
    int kind=*(int *)((char *)menu+0x190);
    return popup_count()>0||(kind>=0x27&&kind<=0x2b);
}
int z4_status_apply_ready(void *menu){
    int (*main_points)(void *)=(void *)addr(0x169d4c);
    int (*sub_points)(void *)=(void *)addr(0x169da4);
    return menu&&(main_points(menu)>0||sub_points(menu)>0);
}
/* Shoulders select only the shop's primary row. Category rows and item
   grids retain the engine's directional navigation. */
static int shop_tab(int page,int dir){
    void *menu=current_menu();
    if(!menu||shop_popup(menu))return 0;
    int old=*(int *)((char *)menu+0x24);
    if(page==7&&*(int *)((char *)menu+0x228)==4){
        void (*open_bank)(void *)=(void *)addr(0x166918);
        open_bank(gameui);
        game_log("ROUTE network shop -> bank (shoulder)\n");
        return 1;
    }
    void **vt=*(void ***)menu;
    int (*key)(void *,int)=vt[0xb0/8];
    *(int *)((char *)menu+0x24)=0;
    key(menu,dir<0?14:15); /* GsVKey LEFT/RIGHT, only on the chosen tab row */
    game_log("ROUTE shop shoulder primary row page=%d state=%d dir=%d\n",page,old,dir);
    return 1;
}

int input_route_tab(int dir){
    if(!state_ready() || !z4_menu_open()) return 0;
    int cur = *(int *)((char *)gameui + UI_TYPE);
    if(cur==7||cur==11)return shop_tab(cur,dir);
    /* Only the seven pause pages are tabbable; shop (7) and others are not. */
    if(cur < 0 || cur > PAGE_MAX){ game_log("ROUTE tab skipped, page=%d\n", cur); return 0; }
    int next = cur + dir;
    if(next < 0) next = PAGE_MAX;
    if(next > PAGE_MAX) next = 0;
    if(next == cur) return 0;
    game_log("ROUTE tab %d -> %d\n", cur, next);
    fn_changetab(gameui, next);
    return 1;
}

static const char *scene_name(int s){
    return s == SCENE_MENU ? "MENU" : s == SCENE_STORY ? "STORY" : "GAME";
}

/* Native code a button produces in a scene; 0 = deliberately inert.
   Values >= ACT_TAB_PREV are menu actions rather than key codes. */
static int code_for(int id, int scene){
    switch(id){
    case Z4_R_UP:    return K_UP;
    case Z4_R_DOWN:  return K_DOWN;
    /* LEFT/RIGHT always reach the engine: pages use them for their own cursor,
       and a yes/no popup must receive them too (stealing them for page
       switching made the system-page confirm impossible to answer). */
    case Z4_R_LEFT:  return K_LEFT;
    case Z4_R_RIGHT: return K_RIGHT;
    case Z4_R_A:     return K_OK;
    /* Use START's native action during dialogue/script screens too: those
       consume SKIP (35), not BACK (-16). Popup/title/loading interfaces must
       never receive the combat quick-slot key merely because no pause page
       is open. Keep quick-slot 4 only in unobstructed field play. */
    case Z4_R_B:
        if(scene == SCENE_STORY) return K_SKIP;
        if(scene == SCENE_MENU || world_map_open() || popup_count() > 0 ||
           !state_ready() || demo_ui_status != 14) return K_BACK;
        return K_SKILL4;
    case Z4_R_X:     return scene == SCENE_GAME ? K_SKILL2 : 0;
    case Z4_R_Y:     return scene == SCENE_GAME ? K_SKILL3 : 0;
    case Z4_R_R2:    return scene == SCENE_GAME ? K_SKILL5 : 0;
    case Z4_R_L2:    return scene == SCENE_GAME ? K_SHOP : 0;
    case Z4_R_L1:    return (scene == SCENE_GAME && !world_map_open()) ? K_SAVE : ACT_TAB_PREV;
    case Z4_R_R1:    return scene == SCENE_GAME ? K_TAB : ACT_TAB_NEXT;
    }
    return 0;
}
static const char *button_name(int id){
    static const char *n[Z4_PAD_COUNT] = {
        "UP", "DOWN", "LEFT", "RIGHT", "A", "B", "X", "Y",
        "L1", "R1", "L2", "R2", "START", "SELECT"
    };
    return id >= 0 && id < Z4_PAD_COUNT ? n[id] : "?";
}

/* BACK also closes the world map through the engine's own path, which leaves
   a "map" shortcut instead of the HUD mini map; rebuild it a few frames later. */
static void emit_down(int code){
    if(code == K_BACK && world_map_open()) mini_restore_frame = frame_no + 4;
    emit_key(code, 1);
}
static void tap(int code, const char *why){
    if(code == 0) return;
    if(trace != 0) game_log("ROUTE tap %d (%s)\n", code, why);
    emit_down(code);
    pending_up = code;
}
static void start_action(void){
    int scene = z4_scene();
    /* START opens/leaves the pause layer in play, skips a script in a cutscene. */
    tap(scene == SCENE_STORY ? K_SKIP : K_BACK, scene == SCENE_STORY ? "start/skip" : "start/pause");
}
static void select_action(void){
    /* The map is a field action; inside menus SELECT stays inert.
       Key code -6 never reaches the game (CMvApp::EvProceKey drops -6/-7),
       so drive the engine's own mini-map / world-map cycle directly. */
    if(z4_scene() != SCENE_GAME){
        if(trace != 0) game_log("ROUTE SELECT inert in %s\n", scene_name(z4_scene()));
        return;
    }
    if(!state_ready()) return;
    unsigned char *field = (unsigned char *)gameui;
    /* SELECT toggles the world map.  The mini-map flag is on by default in
       the field, so it is not used to decide the state. */
    if(field[UI_WORLD]){
        game_log("MAP close\n");
        fn_closeworld(gameui);
        /* Closing the world map leaves only a "map" shortcut where the HUD
           mini map used to be, so rebuild the mini map right away. */
        fn_openmini(gameui);
    }else{
        game_log("MAP open\n");
        fn_openworld(gameui, 0);
    }
}

int z4_mouse_active(void){ return mouse_mode; }
void z4_mouse_pos(int *x,int *y){ if(x)*x=mouse_x; if(y)*y=mouse_y; }

void input_route_r3(int down){
    if(!down) return;
    mouse_mode=!mouse_mode;
    game_log("ROUTE mouse mode %s at %d,%d\n",mouse_mode?"on":"off",mouse_x,mouse_y);
}
void input_route_stick(int rx,int ry){
    if(!mouse_mode) return;
    if(rx>-4000&&rx<4000) rx=0;
    if(ry>-4000&&ry<4000) ry=0;
    if(!rx&&!ry) return;
    mouse_x+=(int)((long long)rx*10/32767);
    mouse_y+=(int)((long long)ry*10/32767);
    if(mouse_x<0)mouse_x=0; if(mouse_x>799)mouse_x=799;
    if(mouse_y<0)mouse_y=0; if(mouse_y>599)mouse_y=599;
}

void input_route_press(int id, int down){
    combat_input_button(id,down);
    extern int popup_layout_press(int,int);
    if(popup_layout_press(id,down)){
        if(sent[id]){emit_key(sent[id],0);sent[id]=0;}
        return;
    }
    if(mouse_mode){
        if(id==Z4_R_A){
            if(down){ demo_touch_tap(mouse_x,mouse_y); mouse_release=(int)frame_no+2;
                      if(trace!=0) game_log("ROUTE mouse click %d,%d\n",mouse_x,mouse_y); }
            sent[id]=0; return;
        }
        if(id==Z4_R_B){
            if(down){ mouse_mode=0; game_log("ROUTE mouse mode off\n"); }
            sent[id]=0; return;
        }
    }
    int scene = z4_scene();
    if(scene != last_scene){
        if(trace != 0) game_log("ROUTE scene %s (depth page=%d)\n", scene_name(scene), z4_menu_page());
        last_scene = scene;
    }
    if(trace < 0){
        const char *v = getenv("Z4_KEYLOG");
        trace = v && *v && *v != '0';
        if(trace) game_log("ROUTE key logging on\n");
    }

    if(id == Z4_R_START || id == Z4_R_SELECT){
        /* Remember the held state: the quit combo also fires while both keys
           are held, not only when they are pressed within a few frames. */
        if(id == Z4_R_START) held_start = down; else held_select = down;
        if(down){
            if(id == Z4_R_START){
                if(sel_frame && !sel_fired){ quit_requested = 1; sel_frame = start_frame = 0; game_log("ROUTE combo SELECT+START -> quit\n"); return; }
                start_frame = frame_no; start_fired = 0;
            }else{
                if(start_frame && !start_fired){ quit_requested = 1; sel_frame = start_frame = 0; game_log("ROUTE combo START+SELECT -> quit\n"); return; }
                sel_frame = frame_no; sel_fired = 0;
            }
        }else{
            /* A quick tap fires on release, so it feels immediate. */
            if(id == Z4_R_START && start_frame && !start_fired){
                start_frame = 0; start_fired = 1; start_action();
            }else if(id == Z4_R_SELECT && sel_frame && !sel_fired){
                sel_frame = 0; sel_fired = 1; select_action();
            }
        }
        return;
    }

    if(down && id == Z4_R_A && char_select_open()){
        char_select_confirm();
        sent[id] = 0;
        return;
    }
    if(down && id == Z4_R_B && char_select_open()){
        char_select_cancel();
        sent[id] = 0;
        return;
    }
    if(scene==SCENE_MENU&&z4_menu_page()==0&&id==Z4_R_Y){
        void *menu=current_menu();
        if(menu&&!shop_popup(menu)){
            if(down&&z4_status_apply_ready(menu)){
                void (*apply)(void *,int)=(void *)addr(0x16a6ac);
                apply(menu,0); /* same confirmation path as the blue button */
                game_log("ROUTE status Y -> apply pending points\n");
            }
            sent[id]=0;return;
        }
    }
    if(scene==SCENE_MENU&&z4_menu_page()==4&&id==Z4_R_Y){
        void *menu=current_menu();
        if(menu&&!shop_popup(menu)){
            if(down){
                unsigned char *player_mgr=**(unsigned char ***)(game_base+0x311668);
                void *fairy=*(void **)(player_mgr+0x18);
                void **vt=*(void ***)fairy;
                int (*available)(void *)=vt[0x58/8];
                if(available(fairy)){
                    void (*extract)(void *)=(void *)addr(0x178248);
                    extract(menu);
                    game_log("ROUTE fairy Y -> extract\n");
                }
            }
            sent[id]=0;return;
        }
    }
    if(scene==SCENE_MENU&&z4_menu_page()==5&&id==Z4_R_Y){
        void *menu=current_menu();
        if(menu&&!shop_popup(menu)){
            if(down){
                void **vt=*(void ***)menu;
                int (*key)(void *,int)=vt[0xb0/8];
                key(menu,11); /* native quest detail/summary toggle */
                game_log("ROUTE quest Y -> detail toggle\n");
            }
            sent[id]=0;return;
        }
    }
    if(down&&scene==SCENE_MENU&&z4_menu_page()==1){
        void *menu=current_menu();
        if(menu&&!shop_popup(menu)){
            int state=*(int *)((char *)menu+0x24);
            void (*change_keymap)(void *,int,int)=(void *)addr(0x22fb04);
            if(id==Z4_R_A&&state>=0&&state<=3){
                /* IsMainMenu otherwise consumes the next A merely to enable
                   page navigation, instead of forwarding it to the list. */
                if(!((unsigned char *)menu)[0x28]){
                    void **vt=*(void ***)menu;
                    void (*activate)(void *,int,int,int)=vt[0xf0/8];
                    activate(menu,1,0,1);
                    state=*(int *)((char *)menu+0x24);
                }
                /* The engine's OK handler builds the same-slot candidates
                   and remembers the body slot, but never focuses that list. */
                void (*prepare)(void *)=(void *)addr(0x22ec14);
                prepare(menu);
                int count=*(int *)((char *)menu+0x90);
                if(count>0){
                    change_keymap(menu,5,0);
                    game_log("ROUTE equip candidates opened from=%d count=%d\n",state,count);
                }
                sent[id]=0;return;
            }
            if(id==Z4_R_B&&state==5){
                int previous=*(int *)((char *)menu+0x1e8);
                if(previous>=0&&previous<=3){
                    change_keymap(menu,previous,-1);
                    game_log("ROUTE equip candidates cancelled -> body=%d\n",previous);
                    sent[id]=0;return;
                }
            }
        }
    }
    if(down&&scene==SCENE_MENU&&(id==Z4_R_LEFT||id==Z4_R_RIGHT)){
        int page=z4_menu_page();
        if(page==7||page==11){
            void *menu=current_menu();
            if(menu&&!shop_popup(menu)){
                int st=*(int *)((char *)menu+0x24);
                if(st==0){
                    sent[id]=0;
                    game_log("ROUTE direction stays on shop/bank tab row (use L1/R1)\n");
                    return;
                }
            }
        }
    }
    if(down){
        int code = code_for(id, scene);
        if(code >= ACT_TAB_PREV){
            int dir = code == ACT_TAB_NEXT ? 1 : -1;
            if(trace != 0) game_log("ROUTE %s -> tab%+d (page %d)\n", button_name(id), dir, z4_menu_page());
            input_route_tab(dir);
            sent[id] = 0;
            return;
        }
        if(!code){
            if(trace != 0) game_log("ROUTE %s -> inert in %s\n", button_name(id), scene_name(scene));
            sent[id] = 0;
            return;
        }
        if(trace != 0) game_log("ROUTE %s down -> %d (%s) page=%d state=%d\n", button_name(id), code, scene_name(scene), z4_menu_page(), z4_menu_state());
        sent[id] = code;
        emit_down(code);
    }else{
        /* Release the exact code that was pressed, even if the scene changed. */
        if(sent[id]){ if(trace != 0) game_log("ROUTE %s up -> %d\n", button_name(id), sent[id]); emit_key(sent[id], 0); sent[id] = 0; }
    }
}

void input_route_frame(unsigned frame){
    frame_no = frame;
    combat_input_frame();
    /* Holding START and SELECT together quits, however far apart the two keys
       were pressed; this is the reliable path on real hardware. */
    if(held_start && held_select){
        if(++both_held_frames >= 2 && !quit_requested){
            quit_requested = 1;
            game_log("ROUTE combo START+SELECT held -> quit\n");
        }
    }else{
        both_held_frames = 0;
    }
    if(mouse_release&&(int)frame>=mouse_release){ demo_touch_release(mouse_x,mouse_y); mouse_release=0; }
    if(pending_up){ emit_key(pending_up, 0); pending_up = 0; }
    if(mini_restore_frame && frame >= mini_restore_frame){
        mini_restore_frame = 0;
        if(state_ready() && !world_map_open()) fn_openmini(gameui);
    }
    if(start_frame && !start_fired && frame - start_frame >= COMBO_GRACE){
        start_frame = 0; start_fired = 1; start_action();
    }
    if(sel_frame && !sel_fired && frame - sel_frame >= COMBO_GRACE){
        sel_frame = 0; sel_fired = 1; select_action();
    }
}

int input_route_quit(void){ return quit_requested; }

/* ---------------------------------------------------------------------
 * Item-menu UP fix.
 *
 * CMvItemMenu::KeyPress, internal state 2 (cursor on the category row),
 * rewrites UP into a synthetic BACK press at 0x171794..0x1717b8, which then
 * closes the whole pause layer.  UP has nowhere to go on that row, but it
 * must not unwind the menu.  Widening the compare makes UP fall through to
 * the normal forwarding path instead of entering that branch; every other
 * key keeps the original behaviour.
 * ------------------------------------------------------------------- */
enum{
    ADDR_ITEMUP_CMP = 0x171794,
    ITEMUP_BEFORE   = 0x710032df,   /* cmp w22, #0xc  (UP) */
    ITEMUP_AFTER    = 0x7101fedf,   /* cmp w22, #0x7f (never UP) */
};
/* ---------------------------------------------------------------------
 * CMvShopMenu::Open(NULL) guard.
 *
 * The shop can be opened from a path where the menu object was never
 * created; Open then dereferences NULL at 0x1806e0 and the game dies.
 * A 16-byte trampoline at the entry checks "this" first and otherwise
 * re-runs the relocated prologue before continuing.
 * ------------------------------------------------------------------- */
enum{ ADDR_SHOPOPEN = 0x18069c };
static void (*shop_tramp)(void *, int, int, int);

static void shop_open_guard(void *self, int a, int b, int c){
    if(!self){ game_log("MENUFIX shop Open(NULL) skipped\n"); return; }
    shop_tramp(self, a, b, c);
}

static int patch_shop_open(void){
    const uint32_t src[4]={0xa9bd7bfd,0x53001c63,0x910003fd,0xa90153f3};
    uint32_t *at=(uint32_t *)addr(ADDR_SHOPOPEN);
    for(int i=0;i<4;i++)
        if(at[i]!=src[i]){ game_log("MENUFIX skip shop guard: 0x%08x\n",at[i]); return 0; }
    void *mem=mmap(NULL,4096,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(mem==MAP_FAILED){ game_log("MENUFIX shop guard: mmap failed\n"); return 0; }
    uint32_t *t=(uint32_t *)mem;
    for(int i=0;i<4;i++) t[i]=src[i];
    uintptr_t resume=(uintptr_t)(game_base+ADDR_SHOPOPEN+16);
    t[4]=0x58000050; t[5]=0xd61f0200;           /* ldr x16,#8 ; br x16 */
    t[6]=(uint32_t)resume; t[7]=(uint32_t)(resume>>32);
    __builtin___clear_cache((char *)mem,(char *)mem+32);
    shop_tramp=(void *)mem;
    uintptr_t hook=(uintptr_t)shop_open_guard;
    at[0]=0x58000050; at[1]=0xd61f0200;
    at[2]=(uint32_t)hook; at[3]=(uint32_t)(hook>>32);
    __builtin___clear_cache((char *)at,(char *)at+16);
    game_log("MENUFIX shop Open NULL guard installed\n");
    return 1;
}

/* The pause pages draw their own circular touch "back" button; physical B
   already does that job, so the drawing routine is disabled. */
static void patch_back_button(void){
    const uint32_t before=0xb0000d61u;   /* adrp x1, page */
    const uint32_t after =0xd65f03c0u;   /* ret */
    uint32_t *at=(uint32_t *)addr(0x163190);   /* CMvGameUI::DrawBackKey */
    if(*at!=before){game_log("MENUFIX skip back-button: 0x%08x\n",*at);return;}
    uint32_t v=after;
    memcpy(at,&v,4);
    __builtin___clear_cache((char *)at,(char *)at+4);
    game_log("MENUFIX on-screen menu back button hidden\n");
}

static int patch_item_up(void){
    uint32_t *at = (uint32_t *)addr(ADDR_ITEMUP_CMP);
    if(*at != ITEMUP_BEFORE){
        game_log("MENUFIX skip item-up: 0x%x holds 0x%08x\n", ADDR_ITEMUP_CMP, *at);
        return 0;
    }
    uint32_t v = ITEMUP_AFTER;
    memcpy(at, &v, 4);
    __builtin___clear_cache((char *)at, (char *)at + 4);
    game_log("MENUFIX item-menu UP no longer synthesises BACK\n");
    return 1;
}

/* The native pause handler falls back to switching tabs on LEFT/RIGHT when
   the page does not consume the key. Keep page-specific navigation intact,
   but disable those two fallback branches. L1/R1 call ChangeTabMenu directly. */
static void patch_direction_tab_fallback(void){
    const unsigned offsets[]={0x167c70,0x167cb4};
    for(int i=0;i<2;i++){
        uint32_t *at=(void *)addr(offsets[i]);
        if(*at!=0xaa1403e0u){game_log("MENUFIX skip direction tab %x\n",offsets[i]);continue;}
        int32_t delta=0x1679f8-(int32_t)offsets[i];
        *at=0x14000000u|(((uint32_t)(delta/4))&0x3ffffffu);
        __builtin___clear_cache((char *)at,(char *)at+4);
    }
    game_log("MENUFIX tabs use L1/R1 only; directions stay inside page\n");
}

void input_route_init(void (*emit)(int, int)){
    emit_key = emit;
    /* Key tracing on by default: the log is the only evidence after a crash. */
    { const char *v = getenv("Z4_KEYLOG"); trace = (v && *v == '0') ? 0 : 1; }
    { const char *v = getenv("Z4_MOUSE"); if(v && *v && *v != '0'){ mouse_mode = 1; game_log("ROUTE mouse mode on (init)\n"); } }
    patch_item_up();
    patch_direction_tab_fallback();
    patch_shop_open();
    /* Physical walk and tap/hold changes are scoped to their player calls. */
    combat_input_init();
    /* resolve eagerly so problems show up in the log before the first press */
    state_ready();
    game_log("ROUTE init menu=%d story=%d page=%d\n", z4_menu_open(), story_active(), z4_menu_page());
}

/* Diagnostics only: list pointer slots in the engine's static area that really
   hold an object of the game image, validated against the process mem map. */
typedef struct{ uintptr_t lo, hi; } Mapping;
static Mapping maps[256];
static int map_count, maps_loaded;

static void load_maps(void){
    if(maps_loaded) return;
    maps_loaded = 1;
    FILE *f = fopen("/proc/self/maps", "r");
    if(!f) return;
    char line[512];
    while(fgets(line, sizeof(line), f) && map_count < 256){
        unsigned long lo, hi; char perm[8] = {0};
        if(sscanf(line, "%lx-%lx %7s", &lo, &hi, perm) == 3 && perm[0] == 'r'){
            maps[map_count].lo = lo; maps[map_count].hi = hi; map_count++;
        }
    }
    fclose(f);
}
static int readable(uintptr_t a, size_t n){
    load_maps();
    for(int i = 0; i < map_count; i++)
        if(a >= maps[i].lo && a <= maps[i].hi && n <= maps[i].hi - a) return 1;
    return 0;
}

void input_route_dump_slots(void){
    uintptr_t lo = 0x310000, hi = 0x320000;
    if(hi + 8 > game_size) hi = game_size - 8;
    game_log("SLOTS base=%p size=%zu maps=%d\n", (void *)game_base, game_size, map_count);
    {
        static const uintptr_t named[] = {0x314c70,0x31df40,0x312c18,0x31df78,0x310d40,0x3109b8};
        for(unsigned i = 0; i < sizeof(named)/sizeof(named[0]); i++){
            uintptr_t off = named[i];
            if(off + 8 > game_size) continue;
            uint64_t v = *(uint64_t *)(game_base + off);
            game_log("  raw +0x%lx = %p", (unsigned long)off, (void *)(uintptr_t)v);
            if(v && readable((uintptr_t)v, 8)){
                uint64_t w = *(uint64_t *)(uintptr_t)v;
                game_log(" -> %p", (void *)(uintptr_t)w);
                if(w && readable((uintptr_t)w, 8))
                    game_log(" -> %p", *(void **)(uintptr_t)w);
            }
            game_log("\n");
        }
    }
    for(uintptr_t off = lo; off < hi; off += 8){
        uint64_t v = *(uint64_t *)(game_base + off);
        if(!v || v < 0x1000) continue;
        if(!readable((uintptr_t)v, 8)) continue;
        void *vt = *(void **)(uintptr_t)v;
        if(vt < (void *)game_base || vt >= (void *)(game_base + game_size)) continue;
        int depth = readable((uintptr_t)v + UI_DEPTH, 4) ? *(int *)((char *)(uintptr_t)v + UI_DEPTH) : -1;
        int type  = readable((uintptr_t)v + UI_TYPE, 4) ? *(int *)((char *)(uintptr_t)v + UI_TYPE) : -1;
        game_log("  +0x%lx obj=%p vtable=+0x%lx depth=%d type=%d\n",
                 (unsigned long)off, (void *)(uintptr_t)v,
                 (unsigned long)((uintptr_t)vt - game_base), depth, type);
    }
}
