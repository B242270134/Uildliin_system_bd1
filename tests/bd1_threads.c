#include <pthread.h>
#include <unistd.h>
#include <sys/prctl.h>
static void *f(void *a){ (void)a; sleep(900); return 0; }
int main(void){ pthread_t t[3]; prctl(PR_SET_NAME,"bd1-threads"); for(int i=0;i<3;i++) pthread_create(&t[i],0,f,0); sleep(900); return 0; }
