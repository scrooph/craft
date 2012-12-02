#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <signal.h>
#include <semaphore.h>
#include <search.h>

#define CACHESIZE 10000
sem_t * sem;
struct arg_set{
	char * log_p;
	sem_t * sem;
};
struct log_tab{
	char * key;
	char name[20];
	void * p;
} ;
char *log_tab_key[] = {"TRAFNAT","TRAFSESSION","SECURITY","AUDIT","CONFIG"};
struct hsearch_data *htab;
struct log_tab * log_tab_all[5];
struct log_tab * log_tab_temp;
void log_handle (char msg[512]);

void * transfer(void * p){
	time_t		now;
	int  i	= 0,j=0;
	int cf_flag = 0;
	char 	* timestr;
	char 	msg[512];
	char 	* transfer_log_cache;
	char 	* line;
	ENTRY e,*ep;
	log_tab_temp = (struct log_tab *)malloc(5*sizeof(struct log_tab));
	for(j=0;j<5;j++){
		log_tab_temp->key = log_tab_key[j];
		strcpy(log_tab_temp->name,log_tab_key[j]);
		log_tab_temp++;
	}
	printf("****************\n");
	for(j=0;j<5;j++){
		log_tab_all[j]->key = log_tab_key[j];
		strcpy(log_tab_all[j]->name,log_tab_key[j]);
	}
	printf("****************\n");
	hcreate_r(10,htab);
	for(j=0;j<5;j++){
		e.key = log_tab_all[j]->key;
		e.data = log_tab_all[j];
		hsearch_r(e,ENTER,&ep,htab);
	}
	printf("****************\n");
	struct arg_set * args = (struct arg_set *)p;
	transfer_log_cache = args->log_p;
	sem = args->sem;
	line = transfer_log_cache;
	//time(&now);
	//timestr = ctime(&now);
	//timestr[strlen(timestr)-1] = '\0';
	while(1){
		if(cf_flag==1){
			return;
		}
		if(sem_trywait(sem)==0){ //pay attention to the lock of sem
			strcpy(msg,line);
			log_handle(msg);
			i++;
			if(i==CACHESIZE){
				line = transfer_log_cache;
			}else{
				line+=sizeof(msg);
			}
		}
	}
}
/*analyse a line of shmem*/
void cline(){
	
}

void log_handle(msg)
	char msg[512];
{
	int val = 0,i=0,j=0,f=0;
	char temp[20];
	sem_getvalue(sem,&val);
	ENTRY e,*ep;
	struct log_tab * log_temp;
	for(i=0;msg[i]!=':';i++){
		if(msg[i]==' '){
			j++;
		}
		if(j==4){
			temp[f] = msg[i];
			f++;
		}
	}
	temp[f] = '\0';
	e.key = temp;
	e.data = NULL;
	hsearch_r(e,FIND,&ep,htab);
	log_temp = (struct log_tab *) ep->data;
	printf("[tran]: %d $$ %s\n",val,log_temp->name);
	//printf("*******************\n");
	fflush(stdout);
}