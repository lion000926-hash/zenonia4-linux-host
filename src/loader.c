#define _GNU_SOURCE
#include "demo.h"
#include <elf.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

uintptr_t game_base;
size_t game_size;
static unsigned char *file_image;
static Elf64_Ehdr *eh;
static Elf64_Shdr *sections;
static Elf64_Sym *symbols;
static size_t symbol_count;
static const char *names;
static uint64_t stack_guard=0x71ab84c29d31f600ull;
static unsigned char android_stdio[3*152];
static void noop(void){}
static int *android_errno(void){return &errno;}
static int ignore_atexit(void *a,void *b,void *c){return 0;}
typedef struct {void *key;pthread_mutex_t value;} Mutex;
static Mutex mutexes[256];
static pthread_mutex_t table_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t *get_mutex(void *key){
    pthread_mutex_lock(&table_lock);
    Mutex *found=NULL;
    for(int i=0;i<256;i++){
        if(mutexes[i].key==key){found=&mutexes[i];break;}
        if(!mutexes[i].key){
            found=&mutexes[i];found->key=key;
            pthread_mutexattr_t attr;pthread_mutexattr_init(&attr);
            pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE);
            pthread_mutex_init(&found->value,&attr);pthread_mutexattr_destroy(&attr);break;
        }
    }
    pthread_mutex_unlock(&table_lock);
    if(!found){game_log("FATAL: Bionic mutex table exhausted\n");abort();}
    return &found->value;
}
static int android_mutex_lock(void *m){return pthread_mutex_lock(get_mutex(m));}
static int android_mutex_unlock(void *m){return pthread_mutex_unlock(get_mutex(m));}
static FILE *stdio_file(FILE *f){
    uintptr_t p=(uintptr_t)f;
    if(p>=(uintptr_t)android_stdio&&p<(uintptr_t)android_stdio+sizeof(android_stdio))
        return p==(uintptr_t)android_stdio?stdin:stderr;
    return f;
}
static int android_fputs(const char*s,FILE*f){return fputs(s,stdio_file(f));}
static int android_fputc(int c,FILE*f){return fputc(c,stdio_file(f));}
/* 引擎自己会直接调 libc 的 unlink/rename/fopen 管存档，绕开了 JNI。
   这里拦一层：全部记日志，删除前留副本，避免存档无声消失。 */
static int is_save_path(const char*p){return p&&(strstr(p,"Save")||strstr(p,"option"));}
static void keep_copy(const char*p){
    char bak[1200];snprintf(bak,sizeof(bak),"%s.deleted",p);
    FILE*src=fopen(p,"rb");if(!src)return;
    FILE*dst=fopen(bak,"wb");if(!dst){fclose(src);return;}
    char buf[8192];size_t n;while((n=fread(buf,1,sizeof(buf),src))>0)fwrite(buf,1,n,dst);
    fclose(dst);fclose(src);
    game_log("NATIVE keep '%s' -> '%s'\n",p,bak);
}
static int traced_unlink(const char*p){
    /* 引擎的“旧档迁移”是：读旧文件 -> JNI 写新文件 -> unlink 旧文件。
       在这套环境里新旧是同一个目录，删了就等于把刚写的档抹掉。
       这里直接拒绝删除存档，函数返回 0 让引擎以为删成功了。 */
    if(is_save_path(p)){keep_copy(p);game_log("NATIVE unlink BLOCKED '%s'\n",p);return 0;}
    return unlink(p);
}
static int traced_rename(const char*a,const char*b){
    if(is_save_path(a)||is_save_path(b))game_log("NATIVE rename '%s' -> '%s'\n",a?a:"(null)",b?b:"(null)");
    return rename(a,b);
}
static FILE *traced_fopen(const char*p,const char*m){
    FILE*f=fopen(p,m);
    if(is_save_path(p))game_log("NATIVE fopen '%s' mode=%s -> %s\n",p,m?m:"",f?"ok":"FAIL");
    return f;
}
/* 槽位判定用的是 access/stat，不走 fopen，这里补上才能看到它在探哪个文件 */
static int traced_access(const char*p,int mode){
    int r=access(p,mode);
    if(is_save_path(p))game_log("NATIVE access '%s' -> %d\n",p,r);
    return r;
}
static int traced_stat(const char*p,struct stat*b){
    int r=stat(p,b);
    if(is_save_path(p))game_log("NATIVE stat '%s' -> %d size=%ld\n",p,r,r?0:(long)b->st_size);
    return r;
}
static void *resolve(const char *s){
    /* The engine's CPU framebuffer is presented by SDL; its legacy GL1 presentation calls are inactive. */
    if(!strncmp(s,"gl",2))return noop;
    if(!strcmp(s,"__sF"))return android_stdio;
    if(!strcmp(s,"__errno"))return android_errno;
    if(!strcmp(s,"__stack_chk_guard"))return &stack_guard;
    if(!strcmp(s,"__google_potentially_blocking_region_begin")||!strcmp(s,"__google_potentially_blocking_region_end"))return noop;
    if(!strcmp(s,"__cxa_atexit"))return ignore_atexit;
    if(!strcmp(s,"__cxa_finalize"))return noop;
    if(!strcmp(s,"pthread_mutex_lock"))return android_mutex_lock;
    if(!strcmp(s,"pthread_mutex_unlock"))return android_mutex_unlock;
    if(!strcmp(s,"fputs"))return android_fputs;
    if(!strcmp(s,"fputc"))return android_fputc;
    if(!strcmp(s,"unlink"))return traced_unlink;
    if(!strcmp(s,"rename"))return traced_rename;
    if(!strcmp(s,"fopen"))return traced_fopen;
    if(!strcmp(s,"access"))return traced_access;
    if(!strcmp(s,"stat"))return traced_stat;
    return dlsym(RTLD_DEFAULT,s);
}
void *game_symbol(const char *name){
    for(size_t i=0;i<symbol_count;i++)
        if(symbols[i].st_shndx!=SHN_UNDEF&&!strcmp(names+symbols[i].st_name,name))
            return (void*)(game_base+symbols[i].st_value);
    return NULL;
}
int game_load(const char *path){
    FILE *f=fopen(path,"rb");if(!f){perror(path);return -1;}
    fseek(f,0,SEEK_END);size_t length=ftell(f);rewind(f);
    file_image=malloc(length);fread(file_image,1,length,f);fclose(f);
    eh=(void*)file_image;
    if(memcmp(eh->e_ident,ELFMAG,4)||eh->e_machine!=EM_AARCH64||eh->e_ident[EI_CLASS]!=ELFCLASS64)return -1;
    Elf64_Phdr *ph=(void*)(file_image+eh->e_phoff);
    for(int i=0;i<eh->e_phnum;i++)if(ph[i].p_type==PT_LOAD){
        size_t end=ph[i].p_vaddr+ph[i].p_memsz;
        if(end>game_size)game_size=end;
    }
    game_size=(game_size+4095)&~4095ull;
    void *mem=mmap(NULL,game_size,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(mem==MAP_FAILED){perror("mmap");return -1;}game_base=(uintptr_t)mem;
    for(int i=0;i<eh->e_phnum;i++)if(ph[i].p_type==PT_LOAD)
        memcpy(mem+ph[i].p_vaddr,file_image+ph[i].p_offset,ph[i].p_filesz);
    sections=(void*)(file_image+eh->e_shoff);
    for(int i=0;i<eh->e_shnum;i++)if(sections[i].sh_type==SHT_DYNSYM){
        symbols=(void*)(file_image+sections[i].sh_offset);symbol_count=sections[i].sh_size/sizeof(*symbols);
        names=(void*)(file_image+sections[sections[i].sh_link].sh_offset);
    }
    int missing=0,relocs=0;
    for(int i=0;i<eh->e_shnum;i++)if(sections[i].sh_type==SHT_RELA){
        Elf64_Rela *r=(void*)(file_image+sections[i].sh_offset);
        for(size_t j=0;j<sections[i].sh_size/sizeof(*r);j++){
            uint64_t *dst=(void*)(game_base+r[j].r_offset);
            unsigned typ=ELF64_R_TYPE(r[j].r_info),idx=ELF64_R_SYM(r[j].r_info);
            if(typ==R_AARCH64_RELATIVE)*dst=game_base+r[j].r_addend;
            else if(typ==R_AARCH64_GLOB_DAT||typ==R_AARCH64_JUMP_SLOT||typ==R_AARCH64_ABS64){
                void *target=symbols[idx].st_shndx==SHN_UNDEF?resolve(names+symbols[idx].st_name):(void*)(game_base+symbols[idx].st_value);
                if(!target){game_log("UNRESOLVED %s\n",names+symbols[idx].st_name);missing++;}
                *dst=(uintptr_t)target+r[j].r_addend;
            }else{game_log("Unsupported relocation %u\n",typ);return -1;}
            relocs++;
        }
    }
    game_log("ELF mapped base=%p size=%zu symbols=%zu relocations=%d missing=%d\n",mem,game_size,symbol_count,relocs,missing);
    if(missing)return -1;
    __builtin___clear_cache(mem,mem+game_size);
    for(int i=0;i<eh->e_shnum;i++)if(sections[i].sh_type==SHT_INIT_ARRAY){
        void (**ctors)(void)=(void*)(game_base+sections[i].sh_addr);
        for(size_t j=0;j<sections[i].sh_size/8;j++)if(ctors[j])ctors[j]();
    }
    game_log("ELF constructors complete\n");return 0;
}
