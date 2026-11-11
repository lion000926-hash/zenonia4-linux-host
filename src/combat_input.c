#include "demo.h"

/* Preserve native interaction checks and the 500ms charge timer. Only the
   normal-attack branch is postponed until physical A is released. */
static int a_held,directions;
static void *key_scope,*pending_player,*walk_scope;
static int charged;
static long long pressed_at;
static long long (*current_time)(void);
static int (*can_move)(void *,int,int,int,int,int,int);
static void (*player_key)(void *),(*player_release)(void *,int);
static int (*player_action)(void *,int,int);
static int (*player_move)(void *,int,int,int,int,int);
static int (*find_direction)(void *,int,int,int,int);
static int field_controls(void){
    return demo_ui_status==14&&!z4_mouse_active()&&z4_scene()==0;
}
void combat_input_button(int id,int down){
    if(id>=Z4_R_UP&&id<=Z4_R_RIGHT){
        if(down)directions|=1<<id;else directions&=~(1<<id);
    }
    if(id==Z4_R_A){
        a_held=down;
        if(down){pending_player=NULL;charged=0;}
    }
}
void combat_input_frame(void){
    if(!pending_player)return;
    if(!field_controls()){
        pending_player=NULL;charged=0;return;
    }
    if(*(int *)((char *)pending_player+0x183c)>=2)charged=1;
}
static void on_key(void *player){
    void *previous=key_scope;
    key_scope=a_held&&field_controls()?player:NULL;
    player_key(player);
    key_scope=previous;
}
static int on_action(void *player,int attack,int interact){
    if(key_scope!=player||!attack)return player_action(player,attack,interact);
    /* All native item/event/collection checks still run on press. Passing
       attack=false skips both immediate and reserved normal attacks. A zero
       return lets OnKeyPress arm its original charge state and timestamp. */
    int handled=player_action(player,0,interact);
    if(!handled){
        if(pending_player!=player)pressed_at=current_time();
        pending_player=player;
    }
    else{pending_player=NULL;charged=0;}
    return handled;
}
static void on_release(void *player,int key){
    int tap=0;
    if(key==-5&&pending_player==player){
        int state=*(int *)((char *)player+0x183c);
        tap=!charged&&(state==0||state==1)&&field_controls()&&current_time()-pressed_at<=500;
        pending_player=NULL;charged=0;
    }
    /* This cancels the waiting state on a short tap, or releases the actual
       charge through native code on a long hold. Never add a second attack. */
    player_release(player,key);
    if(tap){
        player_action(player,1,0); /* retain native combo reservation */
    }
}
static int on_move(void *player,int facing,int direction,int distance,int frames,int flag){
    void *previous=walk_scope;
    /* MoveKeyProc's ordinary walk uses (dir,dir,-1,48,true). Scripted moves,
       skills and dodge calls retain their own movement path. */
    walk_scope=directions&&field_controls()&&facing==direction&&distance==-1&&frames==48&&flag
        ?player:NULL;
    int result=player_move(player,facing,direction,distance,frames,flag);
    walk_scope=previous;
    return result;
}
static int direct_direction(void *character,int dir,int distance,int facing,int search){
    if(character!=walk_scope)return find_direction(character,dir,distance,facing,search);
    /* Match the native first collision check. Do not enter its sideways
       search or world-position nudges when a physical walk is blocked. */
    return can_move(character,dir,distance,-1,-1,0,0)?dir:-1;
}
void combat_input_init(void){
    current_time=(void *)(game_base+0x151f24);
    can_move=(void *)(game_base+0x1b1dbc);
    player_key=demo_hook(0x1d2448,on_key);
    player_action=demo_hook(0x1cb07c,on_action);
    player_release=demo_hook(0x1cebb0,on_release);
    player_move=demo_hook(0x1d6db0,on_move);
    find_direction=demo_hook(0x1b4738,direct_direction);
    game_log("COMBATINPUT physical A tap-release attack/native hold-charge; physical walk has no obstacle detour\n");
}
