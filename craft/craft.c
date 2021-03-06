#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <stddef.h>
#include <sys/un.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <semaphore.h>
#include "sink.h"
#include "transfer.h"
#include "backup.h"

#define oops(m,x){ perror(m); return 0; }
#define CACHESIZE 10000
static int bk_style;//0 nature 1 auto only 2 auto+mand
static int bk_action;
pthread_t sink_tid;
pthread_attr_t sink_attr;
pthread_t transfer_tid;
pthread_attr_t transfer_attr;
pthread_t backup_tid;
pthread_attr_t backup_attr;

char log_line[512];
char * log_cache;
sem_t p_sem;
struct arg_set{
	char * log_p;
	sem_t *sem;
};

struct arg_set sink_args,transfer_args,backup_args;

void init();
void moniter();
int main(int ac, char * av[])
{
	bk_action = 0;
	log_cache = (char *)malloc(CACHESIZE*sizeof(log_line));
	sink_args.sem = &p_sem;
	transfer_args.sem = &p_sem;
	sink_args.log_p = log_cache;
	transfer_args.log_p = log_cache;
	init();
	pthread_attr_init(&sink_attr);
	pthread_create(&sink_tid,&sink_attr,sink,(void *)&sink_args);
	pthread_attr_init(&transfer_attr);
	pthread_create(&transfer_tid,&transfer_attr,transfer,(void *)&transfer_args);
	moniter();
	pthread_join(sink_tid,NULL);
	pthread_join(transfer_tid,NULL);
	return 0;
}
/*initialize the config of program*/
void init(){
	
}
/*moniter the msg outside*/
void moniter(){
	while(1){
		if(bk_action==1){
			pthread_attr_init(&backup_attr);
			pthread_create(&backup_tid,&backup_attr,backup("/home/scrooph/"),NULL);
		}
	}
}