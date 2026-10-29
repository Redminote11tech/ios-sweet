#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <assert.h>
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL 1
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT 2
struct ssync { int live, signaled; uint32_t seq; };
static struct ssync table[3];
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static struct ssync *ssync(uint32_t h){return h<3 && table[h].live ? &table[h] : NULL;}
static int fence_passed(uint32_t seq){return seq==9;}
#include "sync-wait.inc"
static void *signal_later(void *arg){
 (void)arg; struct timespec pause={0,10000000};nanosleep(&pause,NULL);
 uint32_t h=1;assert(drmSyncobjSignal(0,&h,1)==0);return NULL;
}
int main(void){
 table[0]=(struct ssync){1,0,8};table[1]=(struct ssync){1,1,0};
 uint32_t h[]={0,1},first=99;
 assert(drmSyncobjWait(0,h,2,0,0,&first)==0 && first==1);
 assert(drmSyncobjWait(0,h,2,0,DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL,NULL)==-ETIME);
 uint32_t bad[]={1,7};assert(drmSyncobjWait(0,bad,2,0,0,NULL)==-EINVAL);
 table[1].signaled=0;
 assert(drmSyncobjWait(0,h+1,1,0,0,NULL)==-EINVAL);
 assert(drmSyncobjWait(0,h+1,1,0,DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,NULL)==-ETIME);
 pthread_t thread;assert(pthread_create(&thread,NULL,signal_later,NULL)==0);
 assert(drmSyncobjWait(0,h+1,1,s9_monotonic_ns()+1000000000LL,DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,NULL)==0);
 pthread_join(thread,NULL);
 table[0].seq=9;assert(drmSyncobjWait(0,h,2,0,DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL,NULL)==0);
 table[1].signaled=0;
 assert(drmSyncobjSignal(0,bad,2)==-EINVAL && table[1].signaled==0);
 assert(drmSyncobjTimelineSignal(0,NULL,NULL,0)==-ENOTSUP);
 table[1].signaled=1;
 assert(drmSyncobjReset(0,bad,2)==-EINVAL && table[1].signaled==1);
 assert(drmSyncobjReset(0,h,2)==0 && !table[0].seq && !table[1].signaled);
 puts("PASS: wait-any, wait-all, timeout, invalid handles, delayed CPU signal, atomic rejection, native timeline refusal");
}
