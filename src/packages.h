/* packages.list is supplied by Android's PackageManager. Refresh socket ACL
 * and the process-name/uid table together when the file is replaced. */
struct package {uid_t uid;char name[256];};
static struct package *packages;
static size_t package_count;
static struct stat package_stat;
static const char *package_path,*socket_path;

static int refresh_packages(void)
{
    struct stat st;
    if(stat(package_path,&st)) return 0;
    if(packages && st.st_dev==package_stat.st_dev && st.st_ino==package_stat.st_ino &&
       st.st_mtim.tv_sec==package_stat.st_mtim.tv_sec && st.st_mtim.tv_nsec==package_stat.st_mtim.tv_nsec)
        return 1;
    FILE *f=fopen(package_path,"re");
    if(!f) return 0;
    struct package *list=NULL;
    size_t count=0,cap=0,linesize=0;
    char *line=NULL,*acltext=NULL;
    size_t aclsize=0;
    FILE *spec=open_memstream(&acltext,&aclsize);
    if(!spec) {fclose(f);return 0;}
    fputs("u::rw,g::---,m::rw,o::---",spec);
    unsigned char seen[10000]={0};
    int ok=!fstat(fileno(f),&st);
    while(ok && getline(&line,&linesize,f)>=0) {
        struct package pkg;
        if(sscanf(line,"%255s %u",&pkg.name[0],&pkg.uid)!=2) {ok=0;break;}
        if(pkg.uid!=1000 && (pkg.uid<10000 || pkg.uid>19999)) continue;
        if(pkg.uid==1000 && strcmp(pkg.name,system_names[SETTINGS])) continue;
        if(count==cap) {
            cap=cap?cap*2:64;
            struct package *more=realloc(list,cap*sizeof(*list));
            if(!more) {ok=0;break;}
            list=more;
        }
        list[count++]=pkg;
        if(pkg.uid>=10000 && !seen[pkg.uid-10000]) {
            seen[pkg.uid-10000]=1;
            fprintf(spec,",u:%u:rw",pkg.uid);
        }
    }
    ok=ok && !ferror(f) && count;
    fclose(f);free(line);
    if(fclose(spec)) ok=0;
    static void *lib;
    if(!lib) lib=dlopen("libacl.so.1",RTLD_NOW);
    void *(*parse)(const char*)=lib?dlsym(lib,"acl_from_text"):NULL;
    int (*apply)(const char*,int,void*)=lib?dlsym(lib,"acl_set_file"):NULL;
    int (*release)(void*)=lib?dlsym(lib,"acl_free"):NULL;
    void *acl=ok && parse && apply && release ? parse(acltext) : NULL;
    ok=acl && apply(socket_path,0x8000,acl)==0;
    if(acl) release(acl);
    free(acltext);
    if(!ok) {free(list);return 0;}
    free(packages);packages=list;package_count=count;package_stat=st;
    fprintf(stderr,"layer-input loaded %zu package identities\n",count);
    return 1;
}

static int peer_role(pid_t pid,uid_t uid,char name[256])
{
    char path[64],group[256]={0};
    snprintf(path,sizeof(path),"/proc/%d/cgroup",pid);
    FILE *f=fopen(path,"re");
    if(!f) return -1;
    int android=fgets(group,sizeof(group),f) && !strncmp(group,"0::/lxc.payload.waydroid/",25);
    fclose(f);
    if(!android) return -1;
    snprintf(path,sizeof(path),"/proc/%d/cmdline",pid);
    int fd=open(path,O_RDONLY|O_CLOEXEC);
    if(fd<0) return -1;
    ssize_t n=read(fd,name,255);
    close(fd);
    if(n<=0 || !memchr(name,0,n)) return -1;
    if(uid==1000 && !strcmp(name,"/system/bin/surfaceflinger")) return PRODUCERS;
    if(uid==1000 && !strcmp(name,"/vendor/bin/hw/android.hardware.graphics.composer@2.1-service")) return PRODUCERS+1;
    if(!refresh_packages()) return -1;
    /* Private app processes may append :suffix. The uid still has to match
     * the package owning the prefix. Isolated and SDK-sandbox UIDs differ. */
    char *sep=strchr(name,':');
    if(sep) *sep=0;
    for(size_t i=0;i<package_count;i++) if(packages[i].uid==uid && !strcmp(packages[i].name,name)) {
        for(int r=0;r<ORDINARY;r++) if(!strcmp(system_names[r],name)) return r;
        return ORDINARY;
    }
    return -1;
}
