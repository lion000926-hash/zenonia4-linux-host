#define _GNU_SOURCE
#include "demo.h"
#include "font.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

typedef struct Obj{unsigned magic;int kind,len,persistent;void *data;}Obj;
typedef struct{char *name,*sig;int calls;}Method;
static struct JNINativeInterface_ env_table;
static struct JNIInvokeInterface_ vm_table;
static JNIEnv env_value=&env_table;
static JavaVM vm_value=&vm_table;
JNIEnv *demo_env=&env_value;
JavaVM *demo_vm=&vm_value;
int demo_ui_status=-1;
static Method methods[512];static int method_count;
static Obj dummy={0x5a344f42,0,0,1,NULL};
static int fw=256,fh=256,font_initialized,cur_font;
static int text_log;
static int file_log;
static float font_size=18;
static uint32_t font_color=0xff000000;
static uint32_t text[2048];static int text_count;
static Obj *canvas,*rect;

static Obj *new_obj(int kind,int len,int width){
    Obj *o=calloc(1,sizeof(*o));o->magic=0x5a344f42;o->kind=kind;o->len=len;
    o->data=calloc((size_t)len*width+8,1);return o;
}
static Obj *str_obj(const char *s){Obj *o=new_obj(1,strlen(s),1);memcpy(o->data,s,o->len);return o;}
static const char *str_value(jobject obj){Obj *o=(void*)obj;return o&&o->magic==0x5a344f42?(const char*)o->data:"";}
static Obj *bytes_str(const char *s){Obj *o=new_obj(2,strlen(s),1);memcpy(o->data,s,o->len);return o;}
static void drop(JNIEnv*e,jobject obj){Obj *o=(void*)obj;if(o&&o!=&dummy&&!o->persistent){free(o->data);free(o);}}
static jobject ref(JNIEnv*e,jobject o){if(o)((Obj*)o)->persistent=1;return o;}
static jclass find_class(JNIEnv*e,const char*n){Obj *o=str_obj(n);o->persistent=1;game_log("JNI class %s\n",n);return (void*)o;}
static jclass object_class(JNIEnv*e,jobject o){return (void*)&dummy;}
static jboolean instance(JNIEnv*e,jobject o,jclass c){return 1;}
static jboolean same(JNIEnv*e,jobject a,jobject b){return a==b;}
static jmethodID method_id(JNIEnv*e,jclass c,const char*n,const char*s){
    for(int i=0;i<method_count;i++)if(!strcmp(methods[i].name,n)&&!strcmp(methods[i].sig,s))return (void*)&methods[i];
    Method*m=&methods[method_count++];m->name=strdup(n);m->sig=strdup(s);
    game_log("JNI method %s %s\n",n,s);return (void*)m;
}
static jfieldID field_id(JNIEnv*e,jclass c,const char*n,const char*s){return (void*)method_id(e,c,n,s);}
static jobject object_field(JNIEnv*e,jobject o,jfieldID f){return (void*)&dummy;}
static jint int_field(JNIEnv*e,jobject o,jfieldID f){return 0;}
static jint version(JNIEnv*e){return JNI_VERSION_1_6;}
static jint get_env(JavaVM*v,void**out,jint version){*out=demo_env;return JNI_OK;}
static jint attach(JavaVM*v,void**out,void*a){*out=demo_env;return JNI_OK;}
static jint detach(JavaVM*v){return JNI_OK;}
static jint get_vm(JNIEnv*e,JavaVM**v){*v=demo_vm;return JNI_OK;}
static jboolean no_exception(JNIEnv*e){return JNI_FALSE;}
static jthrowable exception(JNIEnv*e){return NULL;}
static void noargs(JNIEnv*e){}
static jint ensure(JNIEnv*e,jint n){return 0;}
static jobject pop_frame(JNIEnv*e,jobject o){return o;}
static jint monitor(JNIEnv*e,jobject o){return 0;}
static jint regn(JNIEnv*e,jclass c,const JNINativeMethod*m,jint n){return JNI_OK;}
static jstring new_utf(JNIEnv*e,const char*s){return (void*)str_obj(s?s:"");}
static const char *get_utf(JNIEnv*e,jstring s,jboolean*c){if(c)*c=0;return str_value((void*)s);}
static void release_utf(JNIEnv*e,jstring s,const char*c){}
static jsize utf_len(JNIEnv*e,jstring s){return strlen(str_value((void*)s));}
static jstring new_string(JNIEnv*e,const jchar*s,jsize n){
    char *b=calloc(n*3+1,1);int p=0;
    for(int i=0;i<n;i++){uint32_t c=s[i];if(c<128)b[p++]=c;else if(c<2048){b[p++]=192|(c>>6);b[p++]=128|(c&63);}else{b[p++]=224|(c>>12);b[p++]=128|((c>>6)&63);b[p++]=128|(c&63);}}
    jstring result=new_utf(e,b);free(b);return result;
}
static int decode_utf8(const char*s,uint32_t*out,int max){
    const unsigned char*p=(void*)s;int n=0;
    while(*p&&n<max){uint32_t c=*p++;if(c>=0xf0&&p[0]&&p[1]&&p[2]){c=((c&7)<<18)|((p[0]&63)<<12)|((p[1]&63)<<6)|(p[2]&63);p+=3;}
    else if(c>=0xe0&&p[0]&&p[1]){c=((c&15)<<12)|((p[0]&63)<<6)|(p[1]&63);p+=2;}
    else if(c>=0xc0&&*p){c=((c&31)<<6)|(*p++&63);}out[n++]=c;}return n;
}
static jsize string_len(JNIEnv*e,jstring s){uint32_t tmp[2048];return decode_utf8(str_value((void*)s),tmp,2048);}
static const jchar*string_chars(JNIEnv*e,jstring s,jboolean*c){
    uint32_t tmp[2048];int n=decode_utf8(str_value((void*)s),tmp,2048);jchar*b=calloc(n+1,2);for(int i=0;i<n;i++)b[i]=tmp[i];if(c)*c=1;return b;
}
static void release_chars(JNIEnv*e,jstring s,const jchar*c){free((void*)c);}
static jsize arr_len(JNIEnv*e,jarray a){return a?((Obj*)a)->len:0;}
#define ARRAY_FUNCS(Name,Type,Kind) \
static Type##Array new_##Name(JNIEnv*e,jsize n){return (void*)new_obj(Kind,n,sizeof(Type));} \
static Type *get_##Name(JNIEnv*e,Type##Array a,jboolean*c){if(c)*c=0;return a?((Obj*)a)->data:NULL;} \
static void release_##Name(JNIEnv*e,Type##Array a,Type*p,jint mode){} \
static void get_region_##Name(JNIEnv*e,Type##Array a,jsize start,jsize len,Type*b){if(a&&b)memcpy(b,(Type*)((Obj*)a)->data+start,len*sizeof(Type));} \
static void set_region_##Name(JNIEnv*e,Type##Array a,jsize start,jsize len,const Type*b){if(a&&b)memcpy((Type*)((Obj*)a)->data+start,b,len*sizeof(Type));}
ARRAY_FUNCS(Byte,jbyte,2)
ARRAY_FUNCS(Char,jchar,3)
ARRAY_FUNCS(Short,jshort,4)
ARRAY_FUNCS(Int,jint,5)
ARRAY_FUNCS(Long,jlong,6)
ARRAY_FUNCS(Float,jfloat,7)
ARRAY_FUNCS(Double,jdouble,8)
ARRAY_FUNCS(Boolean,jboolean,9)
static jobjectArray new_objects(JNIEnv*e,jsize n,jclass c,jobject val){Obj*o=new_obj(10,n,sizeof(void*));for(int i=0;i<n;i++)((jobject*)o->data)[i]=val;return(void*)o;}
static jobject get_object(JNIEnv*e,jobjectArray a,jsize i){Obj*o=(void*)a;if(!o||o==&dummy)return(void*)&dummy;return i<o->len?((jobject*)o->data)[i]:NULL;}
static void set_object(JNIEnv*e,jobjectArray a,jsize i,jobject v){((jobject*)((Obj*)a)->data)[i]=v;}
static void *critical(JNIEnv*e,jarray a,jboolean*c){if(c)*c=0;return a?((Obj*)a)->data:NULL;}
static void release_critical(JNIEnv*e,jarray a,void*p,jint mode){}

/* 引擎有时会带 "saves/" 或 "assets/" 前缀来问，直接拼会变成 saves/saves/xxx。
   这里统一剥掉前缀，保证写入和读取落在同一个位置。 */
static const char*strip_dir_prefix(const char*name){
    if(!strncmp(name,"saves/",6))return name+6;
    if(!strncmp(name,"assets/",7))return name+7;
    return name;
}
static int is_save_name(const char*name){
    return name&&(strstr(name,"Save")||strstr(name,"option")||strstr(name,".sav"));
}
static int file_path(char*out,size_t cap,const char*name,int save){
    name=strip_dir_prefix(name);
    /* This port's Chinese translation occupies the native Japanese slot.
       Keep that slot working after consolidating its resources into chi. */
    if(!save&&!strcmp(name,"script/z4_jpn_utf8_dlg_120329.zt1"))
        name="script/chi_script_dlg.zt1";
    if(!save&&!strncmp(name,"jpn/",4))
        snprintf(out,cap,"assets/chi/%s",name+4);
    else
        snprintf(out,cap,"%s/%s",save?"saves":"assets",name);
    struct stat st;int ok=!stat(out,&st);
    if(is_save_name(name)){char cw[512];if(!getcwd(cw,sizeof(cw)))cw[0]=0;
        game_log("PATHDBG '%s' -> '%s' exists=%d size=%d errno=%d cwd=%s\n",
                 name,out,ok,ok?(int)st.st_size:0,ok?0:errno,cw);}
    return ok?st.st_size:0;
}
static Obj *read_file(const char*name,int save){
    char p[1024];int length=file_path(p,sizeof(p),name,save);if(!length){game_log("FILE missing %s/%s\n",save?"saves":"assets",name);return NULL;}
    FILE*f=fopen(p,"rb");if(!f)return NULL;Obj*o=new_obj(2,length,1);fread(o->data,1,length,f);fclose(f);
    static int count;if(count++<120)game_log("FILE read %s %d\n",p,length);return o;
}
static void ensure_canvas(void){
    if(canvas&&canvas->len==fw*fh)return;
    if(canvas){free(canvas->data);free(canvas);}
    canvas=new_obj(5,fw*fh,4);canvas->persistent=1;
}
/* 中文排版禁则：标点不能出现在行首，前引号/括号不能留在行尾 */
static int cjk_no_line_start(uint32_t c){
    switch(c){
    case 0x3002:case 0x3001:case 0xFF0C:case 0xFF0E:case 0xFF1A:case 0xFF1B:
    case 0xFF01:case 0xFF1F:case 0x300D:case 0x300F:case 0x3011:case 0x3015:
    case 0xFF09:case 0x2019:case 0x201D:case 0x2026:case 0x30FB:case 0xFF65:
        return 1;
    }
    return 0;
}
static int cjk_no_line_end(uint32_t c){
    switch(c){
    case 0x300C:case 0x300E:case 0x3010:case 0x3014:case 0xFF08:
    case 0x2018:case 0x201C:
        return 1;
    }
    return 0;
}
/* Measure UTF-8 glyphs as rasterised by this bridge; callers remove any
   native BBF formatting commands before passing visible text here. */
void demo_measure_text(const char *s,float *width,int *height){
    uint32_t cps[2048];
    int n=decode_utf8(s,cps,2048);
    float eff=font_size;
    int asc=gfa_font_ascent(eff),des=gfa_font_descent(eff);
    while(asc+des>fh-1&&eff>8.0f){eff-=1.0f;asc=gfa_font_ascent(eff);des=gfa_font_descent(eff);}
    *width=gfa_font_text_width(eff,cps,n);
    *height=asc+des;
}
static int font_dbg;
static Obj *font_rect(int measure,float x,float y,float maxwidth){
    ensure_canvas();if(!rect){rect=new_obj(7,4,4);rect->persistent=1;}
    memset(canvas->data,0,fw*fh*4);float*f=rect->data;
    /* The engine hands us a fixed fw x fh glyph bitmap and expects a rect that
       fits inside it.  ascent+descent grows with the text size, so a size the
       dialogue code forgets to restore can make the glyph box taller than the
       bitmap; the rect would then be taller than the canvas and everything
       drawn afterwards is clipped.  Shrink the rasterised size just enough to
       fit - at the normal size (18) nothing changes, since 16+3 == 19. */
    float eff=font_size;
    int asc=gfa_font_ascent(eff),des=gfa_font_descent(eff);
    while((asc+des)>fh-1&&eff>8.0f){eff-=1.0f;asc=gfa_font_ascent(eff);des=gfa_font_descent(eff);}
    float w=gfa_font_text_width(eff,text,text_count);
    if(!measure&&font_dbg<600){
        int over=(w>fw-1)||(asc+des>fh-1)||(eff!=font_size);
        if(over||font_dbg<80){
            game_log("FONTRECT size=%.1f eff=%.1f chars=%d w=%.1f canvas=%dx%d asc=%d des=%d rect=%dx%d%s\n",
                     font_size,eff,text_count,w,fw,fh,asc,des,(int)w,asc+des,
                     over?" OVERFLOW":"");
            font_dbg++;
        }
    }
    if(!measure)gfa_font_draw_line(eff,text,text_count,canvas->data,fw,fh,x,y+eff-des+1,font_color);
    f[0]=x;f[1]=y;f[2]=w;f[3]=asc+des;
    if(measure){f[0]=w;f[1]=asc+des;}
    return rect;
}
static jvalue dispatch(Method*m,va_list ap){
    jvalue r={0};if(!m)return r;const char*n=m->name;int first=m->calls++==0;
    if(first)game_log("JNI call %s %s\n",n,m->sig);
#define IS(s) (!strcmp(n,s))
    if(IS("readAssete")||IS("readAssets")||IS("loadFile")){const char*nm=str_value(va_arg(ap,jobject));int isSave=IS("loadFile");Obj*o=read_file(nm,isSave);if(file_log<60||is_save_name(nm)){game_log("IO load %s '%s' -> %s\n",isSave?"saves":"assets",nm,o?"ok":"MISSING");file_log++;}r.l=(void*)o;return r;}
    if(IS("isAssetExist")||IS("isFileExist")){const char*nm=str_value(va_arg(ap,jobject));char p[1024];r.i=file_path(p,sizeof(p),nm,IS("isFileExist"));if(file_log<60||is_save_name(nm)){game_log("IO exist '%s' -> %d (%s)\n",nm,r.i,p);file_log++;}return r;}
    if(IS("saveFile")){const char*name=str_value(va_arg(ap,jobject));Obj*o=(void*)va_arg(ap,jobject);char p[1024];file_path(p,sizeof(p),name,1);
        /* 覆盖前先留一份 .bak，防止写入失败或写坏导致存档丢失 */
        {   char bak[1100];snprintf(bak,sizeof(bak),"%s.bak",p);
            FILE*src=fopen(p,"rb");
            if(src){FILE*dst=fopen(bak,"wb");if(dst){char buf[8192];size_t n;while((n=fread(buf,1,sizeof(buf),src))>0)fwrite(buf,1,n,dst);fclose(dst);}fclose(src);} }
        FILE*f=fopen(p,"wb");if(f){r.i=fwrite(o->data,1,o->len,f);int ce=fclose(f);
            struct stat st2;int ex=!stat(p,&st2);
            game_log("SAVE %s bytes=%d close=%d after_exists=%d after_size=%d\n",p,r.i,ce,ex,ex?(int)st2.st_size:0);}else game_log("SAVE FAILED %s errno=%d\n",p,errno);return r;}
    if(IS("deleteFile")){const char*nm=str_value(va_arg(ap,jobject));char p[1024];file_path(p,sizeof(p),nm,1);
        /* 存档保护：游戏会莫名其妙删掉 Save*.dat，这里拒绝删除并保留备份 */
        if(!strncmp(nm,"Save",4)&&strstr(nm,".dat")){
            char bak[1100];snprintf(bak,sizeof(bak),"%s.bak",p);
            FILE*src=fopen(p,"rb");if(src){FILE*dst=fopen(bak,"wb");if(dst){char buf[8192];size_t n;while((n=fread(buf,1,sizeof(buf),src))>0)fwrite(buf,1,n,dst);fclose(dst);}fclose(src);}
            game_log("DELETE BLOCKED '%s' (backup %s)\n",nm,bak);r.i=1;return r;
        }
        r.i=unlink(p)==0;game_log("DELETE '%s' -> %s (%s)\n",nm,r.i?"ok":"false",p);return r;}
    /* 引擎用它拼存档路径：<这里>//Save0.dat、option.sav、SaveFlag.sav。
       必须是真实存档目录，否则槽位全是 EMPTY。
       删除由 loader 的 unlink 钩子拦掉，不会丢档。 */
    if(IS("getAbsolueFilePath")){r.l=(void*)str_obj("saves/");return r;}
    if(IS("getVersion")){r.l=(void*)str_obj("1.2.6");return r;}
    if(IS("getPackageName")){r.l=(void*)str_obj("com.gamevil.zenonia4.global");return r;}
    /* 引擎拿这个字符串下划线前的部分当资源目录名（chi/eng/jpn/...）。
       返回 chi_CN 才会读 assets/chi/。 */
    if(IS("getLanguage")){r.l=(void*)bytes_str("chi_CN");return r;}
    if(IS("getPhoneNumber")||IS("getOldPhoneNumber")){r.l=(void*)bytes_str("01012345678");return r;}
    if(IS("getSimSerialNumber")){r.l=(void*)bytes_str("8982000012345678901");return r;}
    if(IS("getMacAddress")){r.l=(void*)bytes_str("00:00:00:00:00:00");return r;}
    if(IS("getDeviceID")||IS("getAndroidID")){r.l=(void*)bytes_str("0000000000000000");return r;}
    if(IS("getPhoneModel")){r.l=(void*)bytes_str("X35H");return r;}
    if(IS("getOsVersion")){r.l=(void*)bytes_str("6.0");return r;}
    if(IS("getDeviceType")){r.l=(void*)bytes_str("AD Default");return r;}
    if(IS("getCarrierName")){r.l=(void*)bytes_str("ROCKNIX");return r;}
    if(IS("getInetHostAddress")){r.l=(void*)bytes_str("0.0.0.0");return r;}
    if(IS("getPlayerName")){r.l=(void*)str_obj("Regret");return r;}
    if(IS("getPlayerNameByte")){r.l=(void*)bytes_str("Regret");return r;}
    if(IS("getPlayerNumberByte")||IS("getGamevilLiveID")||IS("getGamevilLivePW")){r.l=(void*)bytes_str("");return r;}
    if(IS("getPackageManager")||IS("getPackageInfo")){r.l=(void*)&dummy;return r;}
    /* 语言 ID：1=韩 2=英 3=日 4=中 5=法（来源 Natives.getLocaleID）。
       改成 4 让引擎走 assets/chi/ 资源。 */
    if(IS("getLocaleID")){r.i=4;return r;}
    if(IS("getGLOptionLinear")){r.i=1;return r;}
    if(IS("isNetAvailable")||IS("netConnect")||IS("netSocket")||IS("netBillcomSocketConnect")){r.i=-1;return r;}
    if(IS("getTotalMemory")||IS("getMaxMemory")){r.j=512ll*1024*1024;return r;}
    if(IS("getFreeMemory")){r.j=256ll*1024*1024;return r;}
    if(IS("getUsedMemory")){r.j=128ll*1024*1024;return r;}
    if(IS("OnUIStatusChange")){demo_ui_status=va_arg(ap,int);game_log("UI status=%d\n",demo_ui_status);return r;}
    if(IS("OnEvent")){game_log("UI event=%d\n",va_arg(ap,int));return r;}
    if(IS("OnSoundPlay")){int id=va_arg(ap,int),vol=va_arg(ap,int),loop=va_arg(ap,int);audio_play(id,vol,loop);return r;}
    if(IS("OnStopSound")){audio_stop(-1);return r;}
    if(IS("OnAsyncTimerSet")){int t=va_arg(ap,int),idx=va_arg(ap,int);game_log("TIMER pending ms=%d index=%d\n",t,idx);return r;}
    if(IS("GFA_Init")){int a1=va_arg(ap,int),a2=va_arg(ap,int),a3=va_arg(ap,int),a4=va_arg(ap,int);fw=a1;fh=a2;font_initialized=1;ensure_canvas();r.z=1;game_log("FONT init args=%d,%d,%d,%d canvas=%dx%d\n",a1,a2,a3,a4,fw,fh);return r;}
    if(IS("GFA_IsInitialized")){r.z=font_initialized;return r;}
    if(IS("GFA_CreateFont")){r.i=0;return r;}
    if(IS("GFA_SetFont")){r.i=cur_font;cur_font=va_arg(ap,int);return r;}
    if(IS("GFA_GetCurrentFont")){r.i=cur_font;return r;}
    if(IS("GFA_SetTextSize")){double ns=va_arg(ap,double);
        if((int)(ns*10)!=(int)(font_size*10))
            game_log("FONT size %.1f -> %.1f (asc=%d des=%d cw=%d ch=%d)\n",
                     font_size,ns,gfa_font_ascent(ns),gfa_font_descent(ns),
                     (int)fmaxf(1,gfa_font_advance(ns,0x4e2d)),(int)ns);
        font_size=ns;return r;}
    if(IS("GFA_GetTextSize")){r.f=font_size;return r;}
    if(IS("GFA_SetColor")){font_color=va_arg(ap,int);return r;}
    if(IS("GFA_GetColor")){r.i=font_color;return r;}
    if(IS("GFA_CharWidth")){r.i=fmaxf(1,gfa_font_advance(font_size,0x4e2d));return r;}
    if(IS("GFA_CharHeight")){r.i=font_size;return r;}
    if(IS("GFA_GetAscent")){r.i=gfa_font_ascent(font_size);return r;}
    if(IS("GFA_GetDescent")){r.i=gfa_font_descent(font_size);return r;}
    if(IS("GFA_SetString")){const char*s=str_value(va_arg(ap,jobject));int count=va_arg(ap,int);text_count=decode_utf8(s,text,2048);if(count>0&&count<text_count)text_count=count;return r;}
    if(IS("GFA_SetStringFromUnicode")){Obj*o=(void*)va_arg(ap,jobject);text_count=0;if(o)for(int i=0;i+1<o->len&&text_count<2048;i+=2){uint8_t*b=o->data;uint32_t cp=b[i]|(b[i+1]<<8);if(!cp)break;text[text_count++]=cp;}return r;}
    /* 名字是 KSC5601，但 chi 资源里的文本是 UTF-8（ZT1 解压后 e5 8f 90 = “升”）。
       原来把每个非 ASCII 字节都换成 0x25a1，汉字就全变成 □。这里按 UTF-8 解。 */
    if(IS("GFA_SetStringFromKSC5601")){Obj*o=(void*)va_arg(ap,jobject);text_count=0;
        if(o&&o->len>0){char buf[4096];int n=o->len<4095?o->len:4095;memcpy(buf,o->data,n);buf[n]=0;
            text_count=decode_utf8(buf,text,2048);}
        return r;}
    if(IS("GFA_GetStringLength")){r.i=text_count;return r;}
    if(IS("GFA_GetTextWidths")){const char*s=str_value(va_arg(ap,jobject));int count=va_arg(ap,int);uint32_t tmp[2048];int n=decode_utf8(s,tmp,2048);if(count>0&&count<n)n=count;r.f=gfa_font_text_width(font_size,tmp,n);return r;}
    if(IS("GFA_DrawFont")){r.l=(void*)font_rect(0,0,0,fw);
        static int n;if(n++<25)game_log("DRAWFONT chars=%d size=%.1f canvas=%dx%d rect=%.0f,%.0f %.0fx%.0f\n",text_count,font_size,fw,fh,((float*)rect->data)[0],((float*)rect->data)[1],((float*)rect->data)[2],((float*)rect->data)[3]);
        return r;}
    if(IS("GFA_DrawText")){float x=va_arg(ap,double),y=va_arg(ap,double);int mode=va_arg(ap,int);float w=va_arg(ap,double);if(text_log<40){game_log("TEXT draw x=%.1f y=%.1f maxw=%.1f mode=%d chars=%d\n",x,y,w,mode,text_count);text_log++;}r.l=(void*)font_rect(0,x,y,w);return r;}
    if(IS("GFA_MeasureText")){(void)va_arg(ap,int);float w=va_arg(ap,double);r.l=(void*)font_rect(1,0,0,w);return r;}
    if(IS("GFA_GetPixels32")){ensure_canvas();r.l=(void*)canvas;return r;}
    if(IS("GFA_GetPixels16")){ensure_canvas();Obj*o=new_obj(4,fw*fh,2);uint32_t*s=canvas->data;uint16_t*d=o->data;for(int i=0;i<fw*fh;i++)d[i]=((s[i]>>8)&0xf800)|((s[i]>>5)&0x7e0)|((s[i]>>3)&31);r.l=(void*)o;return r;}
    /* 换行位置：按宽度断行，并做中文标点避头尾。
       原来只是按宽度硬切，标点会掉到行首。int 数组长度是 o->len/4，不是 o->len。 */
    if(IS("GFA_GetWordwrapPositionEx")){float max=va_arg(ap,double);Obj*o=(void*)va_arg(ap,jobject);
        int cap=o?o->len/4:0;float w=0;int lines=0;
        for(int i=0;i<text_count;){
            float adv=gfa_font_advance(font_size,text[i]);
            if(w+adv>max&&i>0){
                int brk=i;
                if(cjk_no_line_start(text[i])&&i>1)brk=i-1;
                while(brk>1&&cjk_no_line_end(text[brk-1]))brk--;
                if(o&&lines<cap)((int*)o->data)[lines]=brk;
                lines++;w=0;
                for(int k=brk;k<=i;k++)w+=gfa_font_advance(font_size,text[k]);
                i++;continue;
            }
            w+=adv;i++;
        }
        r.i=lines;return r;}
    if(IS("GFA_GetWordwrapPosition")){r.i=text_count;return r;}
    /* Optional Android UI, analytics, sensors, online store and vibration are inactive in this demo. */
    return r;
}
#define CALLS(Name,Type,Member) \
static Type call_##Name##_v(JNIEnv*e,jobject o,jmethodID id,va_list ap){va_list cp;va_copy(cp,ap);jvalue r=dispatch((void*)id,cp);va_end(cp);return r.Member;} \
static Type call_##Name(JNIEnv*e,jobject o,jmethodID id,...){va_list ap;va_start(ap,id);Type r=call_##Name##_v(e,o,id,ap);va_end(ap);return r;}
CALLS(Object,jobject,l)
CALLS(Boolean,jboolean,z)
CALLS(Int,jint,i)
CALLS(Long,jlong,j)
CALLS(Float,jfloat,f)
CALLS(Double,jdouble,d)
static void call_Void_v(JNIEnv*e,jobject o,jmethodID id,va_list ap){va_list cp;va_copy(cp,ap);dispatch((void*)id,cp);va_end(cp);}
static void call_Void(JNIEnv*e,jobject o,jmethodID id,...){va_list ap;va_start(ap,id);call_Void_v(e,o,id,ap);va_end(ap);}

void demo_jni_init(void){
    env_table.GetVersion=version;env_table.FindClass=find_class;env_table.GetObjectClass=object_class;
    env_table.GetMethodID=method_id;env_table.GetStaticMethodID=method_id;env_table.GetFieldID=field_id;env_table.GetStaticFieldID=field_id;
    env_table.GetObjectField=object_field;env_table.GetStaticObjectField=object_field;env_table.GetIntField=int_field;env_table.GetStaticIntField=int_field;
    env_table.IsInstanceOf=instance;env_table.IsSameObject=same;env_table.NewGlobalRef=ref;env_table.NewLocalRef=ref;
    env_table.DeleteLocalRef=drop;env_table.DeleteGlobalRef=drop;env_table.ExceptionCheck=no_exception;env_table.ExceptionOccurred=exception;
    env_table.ExceptionClear=noargs;env_table.ExceptionDescribe=noargs;env_table.EnsureLocalCapacity=ensure;env_table.PushLocalFrame=ensure;env_table.PopLocalFrame=pop_frame;
    env_table.GetJavaVM=get_vm;env_table.MonitorEnter=monitor;env_table.MonitorExit=monitor;env_table.RegisterNatives=regn;
    env_table.NewStringUTF=new_utf;env_table.GetStringUTFChars=get_utf;env_table.ReleaseStringUTFChars=release_utf;env_table.GetStringUTFLength=utf_len;
    env_table.NewString=new_string;env_table.GetStringLength=string_len;env_table.GetStringChars=string_chars;env_table.ReleaseStringChars=release_chars;
    env_table.GetArrayLength=arr_len;env_table.NewObjectArray=new_objects;env_table.GetObjectArrayElement=get_object;env_table.SetObjectArrayElement=set_object;
    env_table.GetPrimitiveArrayCritical=critical;env_table.ReleasePrimitiveArrayCritical=release_critical;
#define ASSIGN_ARRAY(N) env_table.New##N##Array=new_##N;env_table.Get##N##ArrayElements=get_##N;env_table.Release##N##ArrayElements=release_##N;env_table.Get##N##ArrayRegion=get_region_##N;env_table.Set##N##ArrayRegion=set_region_##N;
    ASSIGN_ARRAY(Byte) ASSIGN_ARRAY(Char) ASSIGN_ARRAY(Short) ASSIGN_ARRAY(Int) ASSIGN_ARRAY(Long) ASSIGN_ARRAY(Float) ASSIGN_ARRAY(Double) ASSIGN_ARRAY(Boolean)
#define ASSIGN_CALL(N) env_table.Call##N##Method=call_##N;env_table.Call##N##MethodV=call_##N##_v;env_table.CallStatic##N##Method=call_##N;env_table.CallStatic##N##MethodV=call_##N##_v;
    ASSIGN_CALL(Object) ASSIGN_CALL(Boolean) ASSIGN_CALL(Int) ASSIGN_CALL(Long) ASSIGN_CALL(Float) ASSIGN_CALL(Double) ASSIGN_CALL(Void)
    vm_table.GetEnv=get_env;vm_table.AttachCurrentThread=attach;vm_table.AttachCurrentThreadAsDaemon=attach;vm_table.DetachCurrentThread=detach;
    gfa_font_init("font.ttf");
}
