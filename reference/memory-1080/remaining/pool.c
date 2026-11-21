#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "scratch.h"
#define BYTES (1920*1080*4)
static void *win[12];
int main(void) {
 for(unsigned i=0;i<12;i++) {
   struct sb_slot *s=acquire_scratch(&win[i],1920,1080);assert(s);
   memset(s->b[s->cur],11+i,BYTES);release_scratch(&s);
 }
 unsigned n=0;
 for(int i=0;i<8;++i)n+=!!scratch_slots[i].b[0]+!!scratch_slots[i].b[1];
 FILE *f=fopen("/proc/self/smaps_rollup","r");assert(f);char line[128];
 while(fgets(line,sizeof line,f))if(!strncmp(line,"Pss_Anon:",9))printf("%s",line);
 fclose(f);printf("retained_kib=%u\n",n*(BYTES/1024));
 struct sb_slot *s=acquire_scratch(&win[11],1920,1080);assert(s);memset(s->b[s->cur],42,BYTES);s->ok[s->cur]=1;s->bw[s->cur]=1920;s->bh[s->cur]=1080;s->cur^=1;
 struct sb_slot *saved=s;release_scratch(&s);
 int marker=0; s=acquire_scratch(&marker,1920,1080);assert(s==saved && s->b[s->cur^1][1234]==42);release_scratch(&s);
 struct sb_slot *held[9]; for(int i=0;i<9;i++){held[i]=acquire_scratch(&win[i],128,64);assert(held[i]);for(int k=0;k<i;k++)assert(held[k]!=held[i]);}
 assert(held[8]->transient);for(int i=0;i<9;i++)release_scratch(&held[i]); puts("history_and_busy_ok");
}
