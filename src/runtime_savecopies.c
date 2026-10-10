/* bbport: copies of the save folders, made and loaded from the game's pause menu,
 * the overlay and keys (gpu/shim/bbport_save_menu.cpp).
 *   <user>/saves/<YYYYMMDD-HHMMSS>-<manual|auto>/<user id>/<dir>[.sce_sys]
 * A copy holds every save directory of the title (SPRJ0005 and its .sce_sys) for every user id.
 * It is written as <name>.part and renamed when complete, so a listed copy is always whole.
 *
 * Copying waits until the game writes no save file and holds the file layer's save gate
 * (runtime_file.c) meanwhile: the folders do not change under the copy.
 *
 * Loading cannot replace the files under a running game: its state is in memory and its next
 * save would write it back. So a load copies the current state first (an automatic copy), freezes
 * the save files (nothing the game writes reaches them) and sends the game to the title screen;
 * the copy goes in when the title screen reads the save (runtime_saves_begin_load). */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/shim/bbport_save_copies.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

/* The state before the last load ("Undo the last load" in the menus): one is kept. While a load is under
 * way two are, so the one being loaded is still there when it goes in. */
#define AUTO_KEEP 1

static HostMutex lock=HOST_MUTEX_INIT;
static char title[16]="UNKNOWN";

static void copies_root(char *out, size_t size) { snprintf(out,size,"%s/saves",runtime_file_user_dir()); }
static int is_dir(const char *path) { struct stat st; return !stat(path,&st) && S_ISDIR(st.st_mode); }
static int ends_with(const char *s, const char *suffix) {
    size_t n=strlen(s), m=strlen(suffix);
    return n>=m && !strcmp(s+n-m,suffix);
}
/* A save directory of the title: a plain name (SPRJ0005) or its metadata (SPRJ0005.sce_sys). */
static int save_dir_name(const char *name) {
    const char *dot=strchr(name,'.');
    return name[0]!='.' && (!dot || !strcmp(dot,".sce_sys"));
}
static int valid_copy_name(const char *name) {
    size_t n=strlen(name);
    if (!n || n>=sizeof(((RuntimeSaveCopy *)0)->name)) return 0;
    for (size_t i=0;i<n;++i) if (!((name[i]>='0' && name[i]<='9') || (name[i]>='a' && name[i]<='z') || name[i]=='-')) return 0;
    return 1;
}

static int remove_tree(const char *path) {
    struct stat st;
    if (stat(path,&st)) return errno==ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode)) return unlink(path);
    DIR *d=opendir(path);
    if (!d) return -1;
    int r=0;
    for (struct dirent *e; (e=readdir(d));) {
        if (!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
        char child[1024];
        snprintf(child,sizeof(child),"%s/%s",path,e->d_name);
        if (remove_tree(child)) r=-1;
    }
    closedir(d);
    return rmdir(path) ? -1 : r;
}
static int copy_file(const char *from, const char *to) {
    FILE *in=fopen(from,"rb");
    if (!in) return -1;
    FILE *out=fopen(to,"wb");
    if (!out) { fclose(in); return -1; }
    char buffer[1<<16];
    int ok=1;
    for (size_t n; (n=fread(buffer,1,sizeof(buffer),in));) if (fwrite(buffer,1,n,out)!=n) { ok=0; break; }
    ok=ok && !ferror(in) && !fflush(out) && !fsync(fileno(out));
    fclose(in);
    return fclose(out) || !ok ? -1 : 0;
}
/* Leaves out the copies the file layer writes before it replaces a file (*.bbtmp). */
static int copy_tree(const char *from, const char *to) {
    if (mkdir(to,0755) && errno!=EEXIST) return -1;
    DIR *d=opendir(from);
    if (!d) return -1;
    int r=0;
    for (struct dirent *e; (e=readdir(d)) && !r;) {
        if (!strcmp(e->d_name,".") || !strcmp(e->d_name,"..") || ends_with(e->d_name,".bbtmp")) continue;
        char a[1024], b[1024];
        snprintf(a,sizeof(a),"%s/%s",from,e->d_name);
        snprintf(b,sizeof(b),"%s/%s",to,e->d_name);
        r=is_dir(a) ? copy_tree(a,b) : copy_file(a,b);
    }
    closedir(d);
    return r;
}

/* The two files have the same bytes. */
static int same_file(const char *a, const char *b) {
    FILE *x=fopen(a,"rb"), *y=x ? fopen(b,"rb") : NULL;
    int same=x && y;
    static char p[1<<16], q[1<<16];
    while (same) {
        size_t n=fread(p,1,sizeof(p),x), m=fread(q,1,sizeof(q),y);
        if (n!=m || memcmp(p,q,n)) same=0;
        else if (!n) break;
    }
    if (x) fclose(x);
    if (y) fclose(y);
    return same;
}
/* Every entry of `a` is in `b` with the same content (`skip`: a name left out at this level). */
static int tree_within(const char *a, const char *b, const char *skip) {
    DIR *d=opendir(a);
    if (!d) return 0;
    int same=1;
    for (struct dirent *e; same && (e=readdir(d));) {
        if (!strcmp(e->d_name,".") || !strcmp(e->d_name,"..") || (skip && !strcmp(e->d_name,skip))) continue;
        char x[1024], y[1024];
        snprintf(x,sizeof(x),"%s/%s",a,e->d_name);
        snprintf(y,sizeof(y),"%s/%s",b,e->d_name);
        same=is_dir(x) ? is_dir(y) && tree_within(x,y,NULL) : !is_dir(y) && same_file(x,y);
    }
    closedir(d);
    return same;
}
static int same_tree(const char *a, const char *b, const char *skip) {
    return tree_within(a,b,skip) && tree_within(b,a,skip);
}

/* Calls fn(user id, path of the title's save root) for each user folder. */
static void each_user(void (*fn)(const char *user, const char *root, void *ctx), void *ctx) {
    char users[700];
    snprintf(users,sizeof(users),"%s/savedata",runtime_file_user_dir());
    DIR *d=opendir(users);
    if (!d) return;
    for (struct dirent *e; (e=readdir(d));) {
        if (e->d_name[0]=='.') continue;
        char root[1024];
        snprintf(root,sizeof(root),"%s/%s/%s",users,e->d_name,title);
        if (is_dir(root)) fn(e->d_name,root,ctx);
    }
    closedir(d);
}
/* Bloodborne: where the character being played is, as the game's Load Game screen shows it. Its
 * file userdata0010 holds a summary of each character slot (0x192 bytes apart, the name from
 * 0x1096, the PlaceName text id 0x32 after it); the slot played is the userdata000N written last. */
static unsigned bloodborne_place(const char *dir) {
    int slot=-1; time_t newest=0;
    for (int i=0;i<10;++i) {
        char path[1100]; struct stat st;
        snprintf(path,sizeof(path),"%s/userdata%04d",dir,i);
        if (!stat(path,&st) && (slot<0 || st.st_mtime>newest)) { slot=i; newest=st.st_mtime; }
    }
    if (slot<0) return 0;
    char path[1100];
    snprintf(path,sizeof(path),"%s/userdata0010",dir);
    FILE *f=fopen(path,"rb");
    if (!f) return 0;
    unsigned char e[0x36]={0};
    int ok=!fseek(f,0x1096+slot*0x192,SEEK_SET) && fread(e,1,sizeof(e),f)==sizeof(e);
    fclose(f);
    unsigned place=e[0x32]|e[0x33]<<8|(unsigned)e[0x34]<<16|(unsigned)e[0x35]<<24;
    return ok && (e[0] || e[1]) && place>=1000 && place<1000000 ? place : 0;
}
/* The save as it is now: where and when the character played was saved last. */
typedef struct { unsigned place; time_t when; } LiveCtx;
static void live_user(const char *user, const char *root, void *p) {
    (void)user;
    LiveCtx *c=p;
    char dir[1100];
    snprintf(dir,sizeof(dir),"%s/SPRJ0005",root);
    for (int i=0;i<10;++i) {
        char path[1200]; struct stat st;
        snprintf(path,sizeof(path),"%s/userdata%04d",dir,i);
        if (!stat(path,&st) && st.st_mtime>c->when) { c->when=st.st_mtime; c->place=bloodborne_place(dir); }
    }
}
int runtime_saves_current(unsigned *place, char *when, size_t when_size) {
    LiveCtx ctx={0,0};
    each_user(live_user,&ctx);
    if (!ctx.when) return -1;
    if (place) *place=ctx.place;
    if (when) strftime(when,when_size,"%Y-%m-%d %H:%M:%S",localtime(&ctx.when));
    return 0;
}
typedef struct { const char *target; int failed, dirs; unsigned place; } CopyCtx;
static void copy_user(const char *user, const char *root, void *p) {
    CopyCtx *c=p;
    char to_user[1024];
    snprintf(to_user,sizeof(to_user),"%s/%s",c->target,user);
    DIR *d=opendir(root);
    if (!d) { c->failed=1; return; }
    for (struct dirent *e; (e=readdir(d));) {
        char from[1024], to[1100];
        snprintf(from,sizeof(from),"%s/%s",root,e->d_name);
        if (!save_dir_name(e->d_name) || !is_dir(from)) continue;
        if (mkdir(to_user,0755) && errno!=EEXIST) { c->failed=1; break; }
        snprintf(to,sizeof(to),"%s/%s",to_user,e->d_name);
        if (!c->place) c->place=bloodborne_place(from);
        if (copy_tree(from,to)) c->failed=1;
        ++c->dirs;
    }
    closedir(d);
}

static int compare_names(const void *a, const void *b) {
    return -strcmp(((const RuntimeSaveCopy *)a)->name,((const RuntimeSaveCopy *)b)->name); /* newest first */
}
static int list_locked(RuntimeSaveCopy *out, int max) {
    char root[700];
    copies_root(root,sizeof(root));
    DIR *d=opendir(root);
    if (!d) return 0;
    int n=0;
    for (struct dirent *e; (e=readdir(d)) && n<max;) {
        const char *kind=strrchr(e->d_name,'-');
        if (!valid_copy_name(e->d_name) || !kind || (strcmp(kind,"-manual") && strcmp(kind,"-auto"))) continue;
        char path[1024];
        snprintf(path,sizeof(path),"%s/%s",root,e->d_name);
        if (!is_dir(path)) continue;
        RuntimeSaveCopy *c=&out[n++];
        memset(c,0,sizeof(*c));
        snprintf(c->name,sizeof(c->name),"%s",e->d_name);
        c->manual=!strcmp(kind,"-manual");
        char info[1100];
        snprintf(info,sizeof(info),"%s/info",path);
        FILE *f=fopen(info,"r");
        if (f) { if (fscanf(f,"place %u",&c->place)!=1) c->place=0; fclose(f); }
        int Y,M,D,h,m,s;
        if (sscanf(e->d_name,"%4d%2d%2d-%2d%2d%2d",&Y,&M,&D,&h,&m,&s)==6)
            snprintf(c->when,sizeof(c->when),"%04d-%02d-%02d %02d:%02d:%02d",Y,M,D,h,m,s);
        else snprintf(c->when,sizeof(c->when),"%s",e->d_name);
    }
    closedir(d);
    qsort(out,(size_t)n,sizeof(*out),compare_names);
    return n;
}
int runtime_saves_list(RuntimeSaveCopy *out, int max) {
    host_lock(&lock);
    int n=list_locked(out,max);
    host_unlock(&lock);
    return n;
}
static void rotate_locked(int manual, int keep) {
    RuntimeSaveCopy copies[256];
    int n=list_locked(copies,256), kept=0;
    char root[700];
    copies_root(root,sizeof(root));
    for (int i=0;i<n;++i) {
        if (copies[i].manual!=manual || ++kept<=keep) continue;
        char path[1024];
        snprintf(path,sizeof(path),"%s/%s",root,copies[i].name);
        if (!remove_tree(path)) printf("Runtime: save copy %s removed (more than %d)\n",copies[i].name,keep);
    }
}

static int copy_locked(int manual, int keep, char *name_out, size_t name_size) {
    char root[700];
    copies_root(root,sizeof(root));
    if (mkdir(root,0755) && errno!=EEXIST) return -1;
    time_t now=time(NULL);
    char stamp[32], name[64], path[1024], part[1100];
    strftime(stamp,sizeof(stamp),"%Y%m%d-%H%M%S",localtime(&now));
    for (int i=1;;++i) {
        if (i==1) snprintf(name,sizeof(name),"%s-%s",stamp,manual ? "manual" : "auto");
        else snprintf(name,sizeof(name),"%s%c-%s",stamp,'a'+(i-2)%26,manual ? "manual" : "auto");
        snprintf(path,sizeof(path),"%s/%s",root,name);
        if (!is_dir(path) || i>27) break;
    }
    snprintf(part,sizeof(part),"%s.part",path);
    remove_tree(part);
    if (mkdir(part,0755)) return -1;
    /* Wait (up to 3 s) for the game to finish a save in progress: a copy then holds one save,
     * not the first files of a new one with the rest of the old. */
    int writers=0;
    for (int tries=0;tries<30;++tries) {
        writers=runtime_file_saves_hold();
        if (!writers) break;
        runtime_file_saves_release();
        usleep(100000);
    }
    if (writers) runtime_file_saves_hold(); /* still writing: copy what is committed */
    CopyCtx ctx={.target=part};
    each_user(copy_user,&ctx);
    runtime_file_saves_release();
    if (ctx.place) {
        char info[1200];
        snprintf(info,sizeof(info),"%s/info",part);
        FILE *f=fopen(info,"w");
        if (f) { fprintf(f,"place %u\n",ctx.place); fclose(f); }
    }
    /* The game has not saved since the newest copy of this kind: that copy is the same save, and
     * another one would only push older copies out. */
    if (!ctx.failed && ctx.dirs) {
        RuntimeSaveCopy copies[64];
        const int n=list_locked(copies,64);
        for (int i=0;i<n;++i) {
            if (copies[i].manual!=manual) continue;
            char newest[1024];
            snprintf(newest,sizeof(newest),"%s/%s",root,copies[i].name);
            if (!same_tree(part,newest,"info")) break;
            remove_tree(part);
            printf("Runtime: save unchanged since copy %s: no new copy\n",copies[i].name);
            if (name_out) snprintf(name_out,name_size,"%s",copies[i].name);
            return 1;
        }
    }
    if (ctx.failed || !ctx.dirs || rename(part,path)) {
        remove_tree(part);
        printf("Runtime: save copy failed (%s)\n",ctx.dirs ? "a file could not be copied" : "no save folders");
        return -1;
    }
    printf("Runtime: save copied to %s\n",path);
    if (name_out) snprintf(name_out,name_size,"%s",name);
    rotate_locked(manual,keep);
    return 0;
}
int runtime_saves_copy(int manual, int keep, char *name_out, size_t name_size) {
    host_lock(&lock);
    int r=copy_locked(manual,keep < 1 ? 1 : keep,name_out,name_size);
    host_unlock(&lock);
    return r;
}

/* Puts every file of the copy over the save folders: each one written beside its target and
 * renamed over it, so a file is either the old one or the copy's. Files the copy does not have
 * stay. Works while the game runs (it keeps no save file open between saves). */
static int overlay_tree(const char *from, const char *to) {
    if (mkdir(to,0755) && errno!=EEXIST) return -1;
    DIR *d=opendir(from);
    if (!d) return -1;
    int r=0;
    for (struct dirent *e; (e=readdir(d));) {
        if (!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
        char a[1024], b[1024], temp[1100];
        snprintf(a,sizeof(a),"%s/%s",from,e->d_name);
        snprintf(b,sizeof(b),"%s/%s",to,e->d_name);
        if (is_dir(a)) { if (overlay_tree(a,b)) r=-1; continue; }
        snprintf(temp,sizeof(temp),"%s.bbrestore",b);
        if (copy_file(a,temp) || rename(temp,b)) { unlink(temp); r=-1; printf("Runtime: save copy: %s not replaced\n",b); }
    }
    closedir(d);
    return r;
}
static int restore_locked(const char *name) {
    char root[700], copy[1024];
    copies_root(root,sizeof(root));
    snprintf(copy,sizeof(copy),"%s/%s",root,name);
    if (!valid_copy_name(name) || !is_dir(copy)) { printf("Runtime: save copy '%s' to load not found\n",name); return -1; }
    int failed=0;
    DIR *d=opendir(copy);
    if (!d) return -1;
    for (struct dirent *e; (e=readdir(d));) {
        if (e->d_name[0]=='.') continue;
        char from[1100], user_dir[800], live_root[1100];
        snprintf(from,sizeof(from),"%s/%s",copy,e->d_name);
        if (!is_dir(from)) continue;
        snprintf(user_dir,sizeof(user_dir),"%s/savedata/%s",runtime_file_user_dir(),e->d_name);
        snprintf(live_root,sizeof(live_root),"%s/%s",user_dir,title);
        mkdir(user_dir,0755);
        mkdir(live_root,0755);
        DIR *u=opendir(from);
        if (!u) { failed=1; continue; }
        for (struct dirent *f; (f=readdir(u));) {
            char a[1300], b[1300];
            snprintf(a,sizeof(a),"%s/%s",from,f->d_name);
            if (!save_dir_name(f->d_name) || !is_dir(a)) continue;
            snprintf(b,sizeof(b),"%s/%s",live_root,f->d_name);
            if (overlay_tree(a,b)) failed=1;
        }
        closedir(u);
    }
    closedir(d);
    printf("Runtime: save copy %s %s\n",name,failed ? "NOT fully loaded (see above)" : "loaded");
    return failed ? -1 : 0;
}

/* A load in the running game: the game leaves for the title screen (bbport_save_menu.cpp calls
 * its "Exit Game"); the save it writes on the way is dropped (frozen), and once the title screen
 * mounts the save to read it, the copy goes in and saving works again. */
static int load_pending;
static char load_name[64];
int runtime_saves_begin_load(const char *name) {
    host_lock(&lock);
    if (name && *name) {
        char root[700], path[1024];
        copies_root(root,sizeof(root));
        snprintf(path,sizeof(path),"%s/%s",root,name);
        if (!valid_copy_name(name) || !is_dir(path)) { host_unlock(&lock); return -1; }
        /* The state being left: loadable again from the list. */
        if (copy_locked(0,AUTO_KEEP+1,NULL,0)<0) printf("Runtime: the current save could not be copied before loading\n");
    }
    snprintf(load_name,sizeof(load_name),"%s",name ? name : "");
    __atomic_store_n(&load_pending,1,__ATOMIC_RELEASE);
    runtime_file_saves_freeze(1);
    host_unlock(&lock);
    printf("Runtime: loading %s: back to the title screen, the save the game writes meanwhile is dropped\n",
           name && *name ? name : "the game's own save");
    return 0;
}
void runtime_saves_mounted(int read_only) {
    if (!read_only || !__atomic_load_n(&load_pending,__ATOMIC_ACQUIRE)) return;
    host_lock(&lock);
    if (load_pending) {
        runtime_file_saves_hold();
        if (load_name[0]) restore_locked(load_name);
        runtime_file_saves_release();
        rotate_locked(0,AUTO_KEEP);
        __atomic_store_n(&load_pending,0,__ATOMIC_RELEASE);
        runtime_file_saves_freeze(0);
        puts("Runtime: save writes back on (title screen); pressing Continue");
        runtime_pad_press_cross(1500,150); /* the title screen's first item: Continue */
    }
    host_unlock(&lock);
}
int runtime_saves_loading(void) { return __atomic_load_n(&load_pending,__ATOMIC_ACQUIRE); }
/* The game did not go to the title screen (it was loading, or dying): saving works again and the
 * game's own save stays as it is. The automatic copy made before stays as "Undo the last load". */
int runtime_saves_cancel_load(void) {
    host_lock(&lock);
    const int was=load_pending;
    if (was) {
        __atomic_store_n(&load_pending,0,__ATOMIC_RELEASE);
        runtime_file_saves_freeze(0);
        rotate_locked(0,AUTO_KEEP);
        printf("Runtime: loading %s cancelled: the game did not go to the title screen; save writes back on\n",
               load_name[0] ? load_name : "the game's own save");
    }
    host_unlock(&lock);
    return was;
}

void runtime_saves_startup(const char *title_id) {
    if (title_id && *title_id) snprintf(title,sizeof(title),"%s",title_id);
    char root[700];
    copies_root(root,sizeof(root));
    /* Copies a crash interrupted. */
    DIR *d=opendir(root);
    if (!d) return;
    for (struct dirent *e; (e=readdir(d));) {
        if (!ends_with(e->d_name,".part")) continue;
        char path[1024];
        snprintf(path,sizeof(path),"%s/%s",root,e->d_name);
        remove_tree(path);
    }
    closedir(d);
    host_lock(&lock);
    rotate_locked(0,AUTO_KEEP); /* older ports kept more */
    host_unlock(&lock);
}
