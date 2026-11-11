#include "demo.h"
static const char *(*get_string)(void *,int,int);
static const char *safe_script_string(void *mgr,int type,int index){
 if(type>=0&&type<=1&&!*(void **)((char *)mgr+0x20+type*8)){
  static int logged;
  if(!logged++)game_log("SCRIPT missing optional string table type=%d index=%d; use native no-text path\n",type,index);
  return NULL;
 }
 return get_string(mgr,type,index);
}
void script_text_init(void){get_string=demo_hook(0x222c70,safe_script_string);}
