#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <signal.h>
#include <semaphore.h>
#include <sqlite3.h>
#include "gnuhash.h"

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
//日志模块属性
struct logModule{
	char logdir[30];//cache目录
	char moduleName[10];//模块名称
	int rollMark; //回滚选项
	int rollStatus; //当前回滚状态
	int logsize;//日志大小
	int format;//日志格式类型
};
//日志子模块属性
struct logSubModule{
	char subName[20];
	struct logModule *p;
	int maxNum;//当前表存储最大条目数
	int needle;//数据表当前已回滚条目数
};

char *log_tab_key[] = {"TRAFNAT","TRAFSESSION","SECURITY","AUDIT","CONFIG"};
struct hsearch_data *htab;
//struct log_tab * log_tab_all[5];
struct log_tab * log_tab_temp,* log_tab_start;
void log_handle (char msg[512]);
void sql_insert (char msg[512]);


void * transfer(void * p){
	time_t		now;
	int  i	= 0,j=0;
	int cf_flag = 0;
	char 	* timestr;
	char 	msg[512];
	char 	* transfer_log_cache;
	char 	* line;
	ENTRY *ep;
	log_tab_start = (struct log_tab *)malloc(5*sizeof(struct log_tab));
	log_tab_temp = log_tab_start;
	for(j=0;j<5;j++){
		log_tab_temp->key = log_tab_key[j];
		strcpy(log_tab_temp->name,log_tab_key[j]);
		log_tab_temp++;
	}
	log_tab_temp = log_tab_start;
	/*for(j=0;j<5;j++){
		log_tab_all[j]->key = log_tab_key[j];
		strcpy(log_tab_all[j]->name,log_tab_key[j]);
	}*/
	hash_create(10,htab);
	for(j=0;j<5;j++){
		hash_add(log_tab_temp->key,log_tab_temp,htab);
		log_tab_temp++;
	}
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
			//printf("%s",msg);
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
	ENTRY *ep;
	struct log_tab * log_temp;
	printf("%s",msg);
	/*get the module name from msg to temp*/
	for(i=0;msg[i]!='\0';i++){
		if(msg[i]==':'&&msg[i+1]==' '){
			for(j=0;;j++){
				if(msg[i-j]==' ')
				{
					for(f=0;f<j-1;f++){
						temp[f] = msg[i-j+f+1];
					}
					break;
				}
			}
		}
	}
	temp[f] = '\0';
	//printf("%s\n",temp);
	ep = hash_find(temp,htab);
	if(ep){
		log_temp = (struct log_tab *) ep->data;
		if(strcmp(log_temp->name,"TRAFNAT")==0){
			sql_insert(msg);
		}else{
			printf("[tran]: %d $$ %s\n",val,log_temp->name);
		}
	}else{
		printf("it is not attached!\n");
	}
	fflush(stdout);
}

void sql_insert(msg)
	char msg[512];
{
	int rc,i;
	char msg_temp[600];
	sqlite3 *db;
	rc = sqlite3_open("/home/scrooph/craft/hash/traffic.db", &db);
	char * pErrMsg = 0;
	if( rc )
	{
		fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(db));
		sqlite3_close(db);
	}
	else{ 
		//printf("You have opened a sqlite3 database named zieckey.db successfully!nCongratulations! Have fun ! ^-^ n");
		sprintf(msg_temp,"insert into nat(`content`) values('%s');",msg);
		sqlite3_exec( db,msg_temp, 0, 0, &pErrMsg);
	}
	sqlite3_close(db); //关闭
}
