/*the src is the main part of logcraft
logcraft: a software for process Log from system
*/
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/errno.h>
#include <stddef.h>
#include <sys/un.h>
#include <event.h>
#include <sqlite3.h>
#include <time.h>
#include <signal.h>
#include <semaphore.h>
#include <fcntl.h>
#include <syslog.h>
#include "gnuhash.h"

#define oops(m,x){ perror(m); return; }
#define CACHESIZE 10000
#define MSGLEN 512
#define MAXLINE 1024
#define DEFUPRI		(LOG_USER|LOG_NOTICE)
#define DEFSPRI		(LOG_KERN|LOG_CRIT)
#define SOCKNAME "/var/log/log.socket"
#ifndef _PATH_LOGCONF 
#define _PATH_LOGCONF	"/home/scrooph/craft/logCraft/logcraft.conf"
#endif
#define CONT_LINE	1		/* Allow continuation lines */
#define Debug		1		/* debug flag */
#define DEBUG_MAIN	1
#define DEBUG_TRAN	2
#define DEBUG_CONF	3
/*define log constant parameter*/
#define DATE	0		/* unused entry */
#define HOST		1		/* regular file */
#define PRI		2		/* terminal */
#define FAC	3		/* console terminal */
#define LEFT		4		/* remote machine */

/**/
static int debugging_on = 0;

struct hsearch_data *htab;


static int cache_rflag = 0;
pthread_t transfer_tid;
pthread_attr_t transfer_attr;
pthread_t backup_tid;
pthread_attr_t backup_attr;

char *ConfFile = _PATH_LOGCONF;
char kmark[20];
short int confStatus 	= 0;//config module
char log_line[MSGLEN];
char (* log_cache)[MSGLEN];
char (* log_cache_out)[MSGLEN];
char log_cache_start[CACHESIZE][MSGLEN];
void cfline(char *line);
int unix_time(char *time);
sem_t *sem;
int sock;
char * parts;//the left part of message
/*struct list*/
struct bk_time{
	short int day;
	int time;
	struct bk_time *next;
};

struct bkOption{
	short int bk_style;//0 nature 1 auto only 2 auto+mand
	short int bk_action;
	char bk_dir[256];
	struct bk_time *bkTimeHead;
	struct bk_time *bkTimeTail;
} bkInstant;

struct lc_statistic{
	int lc_receive;
	int lc_drop;
	int lc_bad; //wrong format
	int lc_db_succ;
	int lc_db_fail;
} lc_stat;
/*log module struct*/
struct lc_module{
	sqlite3 *db;
	char name[30];
	char db_location[256];
	long long db_size;
	struct lc_module *next;
} *moduleHead,*moduleTail;
/*the principle of parser and template of sql*/
struct parser_list{
	char letter;
	char field[50];
	struct parser_list *next;
};
struct value_list{
	char field[50];
	int fvalue;
	struct value_list *next;
};
struct lc_subModuleTemp{
	char name[30];
	char table[50];
	struct value_list *vHead;
	struct value_list *vTail;
	struct parser_list *pHead;
	struct parser_list *pTail;
	struct lc_subModuleTemp *next;
} *smodulePlHead,*smodulePlTail;
/*log sub module struct*/
struct lc_subModule{
	char subName[30];
	struct lc_module *pModule;
	struct lc_subModuleTemp *template;
	struct lc_subModule *next;
} *smoduleHead,*smoduleTail;

/*function list*/
void lc_init();
void * lc_accept();
void * lc_read(int fd, short event, struct event *arg);
int setnonblock(int fd);
void updateLog(char *msg,int len);
void printchopped(char*,int);
void printline(char *msg); //insert log into cache

void lc_transfer();
int assemLog(char (*msg)[MSGLEN],char (*sline)[MSGLEN+100],struct lc_subModule **asp);
void sql_insert(struct lc_subModule *sp,char (*msg)[MSGLEN+100]);

static void ldprintf(int mark,char *fmt,...);
void logerror(char *type); //output syslog
/*parser the log*/
void *parser(char symbol,char (*line)[MSGLEN],char (*lineLeft)[MSGLEN],\
             char (*lineRight)[MSGLEN]);

/*the core function of main:
init the arguments and environment
get message from other process (syslog-ng / web)
*/
int main(int argc,char *argv[])
{
	char sockname[]	= SOCKNAME;
	struct 		sockaddr_un addr;
	socklen_t 	addrlen;
	time_t		now;
	int  msgnum	= 0;
	struct event *ev;
	/**/
	if(argc>=2){
		debugging_on = 2;
	}

	/*initial the config*/
	bkInstant.bk_style = 0;
	lc_init();
	//return 1;
	/**/
	log_cache		= log_cache_start;
	log_cache_out	= log_cache_start;
	lc_stat.lc_receive = 0;	
	lc_stat.lc_drop = 0;
	lc_stat.lc_bad = 0;
	lc_stat.lc_db_succ = 0;
	lc_stat.lc_db_fail = 0;
	ev = (struct event *)malloc(sizeof(struct event));
	/**/
	parts = (char *) 0;
	addr.sun_family	= AF_UNIX;
	strcpy(addr.sun_path,sockname);
	
	sock	= socket(AF_UNIX,SOCK_STREAM,0);
	
	addrlen = offsetof(struct sockaddr_un, sun_path)+ strlen(addr.sun_path);
	
	unlink(sockname);
	
	if(bind(sock,(struct sockaddr *) &addr,addrlen) == -1){
		oops("bind",3);
	}
	listen(sock,6);
	/* set nonblock*/
    if (setnonblock(sock) < 0)
            oops("failed to set logcraft socket non-blocking",4);
	/*start the transfer thread*/
	pthread_attr_init(&transfer_attr);
	pthread_create(&transfer_tid,&transfer_attr,lc_transfer,NULL);
	/*Init libevent*/
	event_init();
	event_set(ev,sock,EV_READ|EV_PERSIST,(void *)lc_accept,ev);
	event_add(ev,NULL);
	event_dispatch();
}

/**
 * set this socket be nonblock
 */
int setnonblock(int fd)
{
        int flags;

        flags = fcntl(fd, F_GETFL);
        if (flags < 0)
                return flags;
        flags |= O_NONBLOCK;
        if (fcntl(fd, F_SETFL, flags) < 0)
                return -1;

        return 0;
}

/*
	init the process
*/
void lc_init()
{
	register FILE *cf;
	register char *p;
	struct lc_subModule *sp;
	struct lc_module *moduleTemp;
	int hashlen = 0;
	int rc;
	
#ifdef CONT_LINE
	char cbuf[BUFSIZ];
	char *cline;
#else
	char cline[BUFSIZ];
#endif
	if ((cf = fopen(ConfFile, "r")) == NULL) {
		ldprintf(DEBUG_CONF,"cannot open %s.\n", ConfFile);
		return;
	}
	/*
	 *  Foreach line in the conf table, open that file.
	 */
#if CONT_LINE
	cline = cbuf;
	while (fgets(cline, sizeof(cbuf) - (cline - cbuf), cf) != NULL) {
#else
	while (fgets(cline, sizeof(cline), cf) != NULL) {
#endif
		/*
		 * check for end-of-section, comments, strip off trailing
		 * spaces and newline character.
		 */
		for (p = cline; isspace(*p); ++p);
		if (*p == '\0' || *p == '#')
			continue;
#if CONT_LINE
		strcpy(cline, p);
#endif
		for (p = strchr(cline, '\0'); isspace(*--p););
#if CONT_LINE
		if (*p == '\\') {
			if ((p - cbuf) > BUFSIZ - 30) {
				/* Oops the buffer is full - what now?
				 * there is an error for unfinished config line
				 */
				cline = cbuf;
			} else {
				*p = 0;
				cline = p;
				continue;
			}
		}  else
			cline = cbuf;
#endif
		*++p = '\0';
#if CONT_LINE
		cfline(cbuf);
#else
		cfline(cline);
#endif
	}

	/* close the configuration file */
	(void) fclose(cf);
	/*init the log module sqlite3 conn*/
	moduleTemp = moduleHead;
	while(moduleTemp!=NULL){
		ldprintf(DEBUG_MAIN,"%s\n",moduleTemp->db_location);
		rc = sqlite3_open(moduleTemp->db_location, &(moduleTemp->db));
		if( rc )
		{
			fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(moduleTemp->db));
			sqlite3_close(moduleTemp->db);
		}
		moduleTemp = moduleTemp->next;
	}
	/*init the hash for subModule*/
	sp = smoduleHead;
	while(sp!=NULL){
		ldprintf(DEBUG_CONF,"%s\n",sp->subName);
		hashlen++;
		sp = sp->next;
	}
	/*create log module  hash table*/
	hash_create(hashlen+5,htab);
	sp = smoduleHead;
	while(sp!=NULL){
		hash_add(sp->subName,sp,htab);
		sp = sp->next;
	}
}

void cfline(line)
	char *line;
{
	char *p;//check if module
	char *q;//
	char *r,*needle=NULL;
	char mName[30];
	char pvt[30]; //template paser value
	char bkName[30];
	char dbSize[15];
	char vfvalue[15];
	int module_mark = 0;
	struct lc_module *moduleTemp;
	struct lc_subModule *smoduleTemp;
	struct lc_subModuleTemp *smodulePl;
	struct parser_list *parserTemp;
	struct value_list *valueTemp;
	struct bk_time *bkTime;
	char stamptime[12];
	char tName[50];//template name
	char kName[80];//the mark of a log
	char hName[30];//name of log module | db name 
	//ldprintf(DEBUG_CONF,"%s\n",line);
	for (q = strchr(line, '\0'); isspace(*--q););
	if(*q=='\{'){
		/*Q3 if user locate the { to a new line then ??*/
		for (p = line; !isspace(*p)&&*p!='\{'; ++p);
		strncpy(mName,line,p-line);
		mName[p-line] = '\0';
		//ldprintf(DEBUG_CONF,"%s\n",mName);
		/*check the mName whether illegal*/
		if(strcmp(mName,"kmark")==0){
			confStatus = 1;
		}else if(strcmp(mName,"module")==0){	
			confStatus = 2;
		}else if(strcmp(mName,"template")==0){
			confStatus = 3;
			for (; isspace(*p); ++p);
			needle = p;
			for(;!isspace(*p)&&(*p)!='\{';++p);
			if(needle==p){
				printf ("error config file in %s:missing template name\n",line);
				exit(1);
			}
			smodulePl = (struct lc_subModuleTemp *)malloc(sizeof(struct lc_subModuleTemp));
			strncpy(smodulePl->name,needle,p-needle);
			smodulePl->name[p-needle] = '\0';
			ldprintf(DEBUG_CONF,"%s\n",smodulePl->name);
			if(smodulePlHead==NULL){
				smodulePlHead = smodulePl;
				smodulePlTail = smodulePl;
			}else{
				smodulePlTail->next = smodulePl;
				smodulePlTail = smodulePl;
			}
		}else if(strcmp(mName,"submodule")==0){
			confStatus = 4;
		}else if(strcmp(mName,"backup")==0){
			confStatus = 5;
		}else{
			printf ("error config file at%s in %s\n",mName,line);
			exit(1);
		}
	}else if(*line=='}'){
		/*Q2 we should check the {}*/
		if(q!=line){
			printf ("error config file with %s\n",line);
			exit(1);
		}
		return;
	}else{
		switch(confStatus){
			case 1:
				p = strchr(line,';');
				if(p!=NULL){
					if(strchr(line,':')){
						printf ("error config file with %s\n:not allowed kernel \
						mark embeded with : \n",line);
						exit(1);
					}
					if(p==line){
						kmark[0] = '\0';
					}else{
						--p;
					}
					for (; isspace(*p); --p);
					strncpy(kmark,line,p-line+1);
					kmark[p-line+1] = '\0';
					ldprintf(DEBUG_CONF,"%s\n",kmark);
				}else{
					printf ("error config file with %s\n",line);
					exit(1);
				}
				break;
			case 2:
				p = strchr(line,';');
				if(p==NULL){
					printf ("error config file with %s:missing ; for end\n",line);
					exit(1);
				}
				moduleTemp = (struct lc_module *)malloc(sizeof(struct lc_module));
				for (r = line; !isspace(*r)&&r<p ; ++r);
				
				strncpy(moduleTemp->name,line,r-line);
				moduleTemp->name[r-line] = '\0';
				
				for (; isspace(*r)&&r<p ; ++r);
				
				for (needle = r; !isspace(*r)&&r<p ; ++r);
				
				strncpy(dbSize,needle,r-needle);
				dbSize[r-needle] = '\0';

				for (; isspace(*r)&&r<p ; ++r);
				
				for (needle = r; !isspace(*r)&&r<p ; ++r);

				strncpy(moduleTemp->db_location,needle,r-needle);
				moduleTemp->db_location[r-needle] = '\0';
				if(!strlen(moduleTemp->name)||!strlen(dbSize)||\
				   !strlen(moduleTemp->db_location)){
						printf ("error config file with %s:missing some part\n",line);
						exit(1);
				}
				moduleTemp->db_size = atoll(dbSize);
				/*Q1 here we need to check Legitimacy of dbSize*/
				if(moduleHead==NULL){
					moduleHead = moduleTemp;
					moduleTail = moduleTemp;
				}else{
					moduleTail->next = moduleTemp;
					moduleTail = moduleTemp;
				}
				break;
			case 3:
				p = strchr(line,'\0');
				for (--p; isspace(*r)&&p>line ; --p);
				if(*p!=';'){
					printf ("error config file with %s:missing ; for end \n",line);
					exit(1);
				}
				for (r = line; *r!='\('&&!isspace(*r)&&r<p ; ++r);
				if(r==line){
					printf ("error config file with %s:missing function name \n",line);
					exit(1);
				}
				strncpy(pvt,line,r-line);
				pvt[r-line] = '\0';
				if(strcmp(pvt,"parser")==0){
					for (; isspace(*r)&&r<p ; ++r);
					if(*r!='('){
						printf ("error config file with %s:missing ( \n",line);
						exit(1);
					}
					for (++r; isspace(*r)&&r<p ; ++r);
					if(*r!='\''){
						printf ("error config file with %s:missing \' \n",line);
						exit(1);
					}
					/*initialize a parser*/
					parserTemp = (struct parser_list *)malloc(sizeof(struct parser_list));
					parserTemp->letter = *++r;
					if(*++r!='\''){
						printf ("error config file with %s:missing \' \n",line);
						exit(1);
					}
					for (++r; isspace(*r)&&r<p ; ++r);
					if(*r!=','){
						if(*r!=')'){
							printf ("error config file with %s:missing , \n",line);
							exit(1);
						}else{
							parserTemp->field[0] = '\0';
						}
					}
					for (++r; isspace(*r)&&r<p ; ++r);
				    if(*r!='`'){
						printf ("error config file with %s:missing ` \n",line);
						exit(1);
					}
				    needle = ++r;
				    for (; *r!='`'&&!isspace(*r)&&r<p ; ++r);
					if(*r!='`'){
						printf ("error config file with %s:missing ` \n",line);
						exit(1);
					}
				    strncpy(parserTemp->field,needle,r-needle);
				    parserTemp->field[r-needle] = '\0';
				    for (++r; isspace(*r)&&r<p ; ++r);
					if(*r!=')'){
						printf ("error config file with %s:missing ) \n",line);
						exit(1);
					}
					for (++r; isspace(*r)&&r<p ; ++r);
					if(*r!=';'){
						printf ("error config file with %s:missing ; \n",line);
						exit(1);
					}
					if(r!=p){
						printf ("error config file with %s:extra end of this line\n",line);
						exit(1);
					}
					ldprintf(DEBUG_CONF,"%c : %s\n",parserTemp->letter,parserTemp->field);
					if(!smodulePlTail->pHead){
						smodulePlTail->pHead = parserTemp;
						smodulePlTail->pTail = parserTemp;
					}else{
						smodulePlTail->pTail->next = parserTemp;
						smodulePlTail->pTail = parserTemp;
					}
				}
				if(strcmp(pvt,"value")==0){
					valueTemp = (struct value_list *)malloc(sizeof(struct value_list));
					for (; isspace(*r)&&r<p ; ++r);
					if(*r!='('){
						printf ("error config file with %s:missing ( \n",line);
						exit(1);
					}
					/*get these fields*/
					for (++r; isspace(*r)&&r<p ; ++r);
				    if(*r!='`'){
						printf ("error config file with %s:missing ` \n",line);
						exit(1);
					}
				    needle = ++r;
				    for (; *r!='`'&&!isspace(*r)&&r<p ; ++r);
					if(*r!='`'){
						printf ("error config file with %s:missing ` \n",line);
						exit(1);
					}
					if(needle==r){
						printf ("error config file with %s:wrong field name \n",line);
						exit(1);
					}					   
				    strncpy(valueTemp->field,needle,r-needle);
				    valueTemp->field[r-needle] = '\0';
					/*
					  get the ,
					*/
					for (++r; isspace(*r)&&r<p ; ++r);
					if(*r!=','){
						printf ("error config file with %s:missing , \n",line);
						exit(1);
					}
					/*
					 * get the value of these fields
					*/
					for (++r; isspace(*r)&&r<p ; ++r);
					if(*r!='$'){
						printf ("error config file with %s:parameter missing $ \n",line);
						exit(1);
					}
					/*initialize the field value*/
					for (needle = ++r; *r!=')'&&!isspace(*r)&&r<p ; ++r);
					strncpy(vfvalue,needle,r-needle);
					vfvalue[r-needle] = '\0';
					if(strcmp(vfvalue,"PRI")==0){
						valueTemp->fvalue = PRI;
					}else if(strcmp(vfvalue,"FAC")==0){
						valueTemp->fvalue = FAC;
					}else if(strcmp(vfvalue,"DATE")==0){
						valueTemp->fvalue = DATE;
					}else if(strcmp(vfvalue,"HOST")==0){
						valueTemp->fvalue = HOST;
					}else if(strcmp(vfvalue,"LEFT")==0){
						valueTemp->fvalue = LEFT;
					}else{
						printf ("error config file with %s:wrong parameter \n",line);
						exit(1);
					}
					ldprintf(DEBUG_CONF,"%s : %d\n",valueTemp->field,valueTemp->fvalue);
					/*detect the end of function*/
				    for (; isspace(*r)&&r<p ; ++r);
					if(*r!=')'){
						printf ("error config file with %s:missing ) \n",line);
						exit(1);
					}
					for (++r; isspace(*r)&&r<p ; ++r);
					if(*r!=';'){
						printf ("error config file with %s:missing ; \n",line);
						exit(1);
					}
					if(r!=p){
						printf ("error config file with %s:extra end of \
						this line\n",line);
						exit(1);
					}
					if(!smodulePlTail->vHead){
						smodulePlTail->vHead = valueTemp;
						smodulePlTail->vTail = valueTemp;
					}else{
						smodulePlTail->vTail->next = valueTemp;
						smodulePlTail->vTail = valueTemp;
					}
				}
				if(strcmp(pvt,"table")==0){
					for (; isspace(*r)&&r<p ; ++r);
					if(*r!='('){
						printf ("error config file with %s:missing ( \n",line);
						exit(1);
					}
					/*get table name*/
					for (++r; isspace(*r)&&r<p ; ++r);
				    if(*r!='`'){
						printf ("error config file with %s:missing ` \n",line);
						exit(1);
					}
				    needle = ++r;
				    for (; *r!='`'&&!isspace(*r)&&r<p ; ++r);
					if(*r!='`'){
						printf ("error config file with %s:missing ` \n",line);
						exit(1);
					}
					if(needle==r){
						printf ("error config file with %s:wrong table name \n",line);
						exit(1);
					}					   
				    strncpy(smodulePlTail->table,needle,r-needle);
				    smodulePlTail->table[r-needle] = '\0';
					ldprintf(DEBUG_CONF,"%s\n",smodulePlTail->table);
					/*detect the end of function*/
				    for (++r; isspace(*r)&&r<p ; ++r);
					if(*r!=')'){
						printf ("error config file with %s:missing ) \n",line);
						exit(1);
					}
					for (++r; isspace(*r)&&r<p ; ++r);
					if(*r!=';'){
						printf ("error config file with %s:missing ; \n",line);
						exit(1);
					}
					if(r!=p){
						printf ("error config file with %s:extra end of \
						this line\n",line);
						exit(1);
					}
				}
				break;
			case 4:
				p = strchr(line,';');
				if(p!=NULL){
					/*get the mark*/
					for (r = line; !isspace(*r)&&r<p ; ++r);
					
					strncpy(kName,line,r-line);
					kName[r-line] = '\0';
					
					for (; isspace(*r)&&r<p ; ++r);
					/*get the log module name*/
					for (needle = r; !isspace(*r)&&r<p ; ++r);
					
					strncpy(hName,needle,r-needle);
					hName[r-needle] = '\0';

					for (; isspace(*r)&&r<p ; ++r);
					
					for (needle = r; !isspace(*r)&&r<p ; ++r);

					strncpy(tName,needle,r-needle);
					tName[r-needle] = '\0';
					/*check if the config is legal*/
					if(!strlen(kName)||!strlen(hName)||!strlen(tName)){
							printf ("error config file with %s\n:missing some part",line);
							exit(1);
					}
					smoduleTemp = (struct lc_subModule *)malloc(sizeof(struct lc_subModule));
					strcpy(smoduleTemp->subName,kName);
					/*get the parent module*/
					moduleTemp = moduleHead;
					while(moduleTemp!=NULL){
						if(strcmp(hName,moduleTemp->name)==0){
							smoduleTemp->pModule = moduleTemp;
						}
						moduleTemp = moduleTemp->next;
					}
					if(smoduleTemp->pModule==NULL){
						printf ("error config file with %s:log module %s does not exist!\n",line,hName);
						exit(1);
					}
					/*get the template of submodule*/
					smodulePl = smodulePlHead;
					while(smodulePl!=NULL){
						if(strcmp(tName,smodulePl->name)==0){
							smoduleTemp->template = smodulePl;
						}
						smodulePl = smodulePl->next;
					}
					if(smoduleTemp->pModule==NULL){
						printf ("error config file with %s:log module %s does not exist!\n",line,hName);
						exit(1);
					}
					/*insert into the link*/
					if(smoduleHead==NULL){
						smoduleHead = smoduleTemp;
						smoduleTail = smoduleTemp;
					}else{
						smoduleTail->next = smoduleTemp;
						smoduleTail = smoduleTemp;
					}
				}else{
					printf ("error config file with %s:missing ; for end\n",line);
					exit(1);
				}
				break;
			case 5:
				p = strchr(line,'\0');
				for (--p; isspace(*r)&&p>line ; --p);
				if(*p!=';'){
					printf ("error config file with %s:missing ; for end \n",line);
					exit(1);
				}
				for (r = line; *r!='\('&&!isspace(*r)&&r<p ; ++r);
				if(r==line){
					printf ("error config file with %s:missing function name \n",line);
					exit(1);
				}
				strncpy(bkName,line,r-line);
				bkName[r-line] = '\0';
				if(strcmp(bkName,"set")==0){
					for (; isspace(*r)&&r<p ; ++r);
					if(*r!='('){
						printf ("error config file with %s:missing ( \n",line);
						exit(1);
					}
					for (++r; isspace(*r)&&r<p ; ++r);
					if(!isdigit(*r)){
						printf ("error config file with %s: mand backup option \
						is not digital\n",line);
						exit(1);
					}
					bkInstant.bk_style = *r - 47;
					   
					/*get the bk_dir*/
					for (++r; isspace(*r)&&r<p ; ++r);
					if(*r!=','){
						printf ("error config file with %s:missing ,\n",line);
						exit(1);
					}
					for (++r; isspace(*r)&&r<p ; ++r);
				    if(*r!='\"'){
						printf ("error config file with %s:missing \" \n",line);
						exit(1);
					}
				    needle = ++r;
				    for (; *r!='\"'&&!isspace(*r)&&r<p ; ++r);
					if(*r!='\"'){
						printf ("error config file with %s:missing \" \n",line);
						exit(1);
					}
				    strncpy(bkInstant.bk_dir,needle,r-needle);
				    bkInstant.bk_dir[r-needle] = '\0';
					/*check the end of set*/
				    for (++r; isspace(*r)&&r<p ; ++r);
					if(*r!=')'){
						printf ("error config file with %s:missing ) \n",line);
						exit(1);
					}
					for (++r; isspace(*r)&&r<p ; ++r);
					if(*r!=';'){
						printf ("error config file with %s:missing ; \n",line);
						exit(1);
					}
					if(r!=p){
						printf ("error config file with %s:extra end of this line\n",line);
						exit(1);
					}
					ldprintf(DEBUG_CONF,"%d : %s\n",bkInstant.bk_style,bkInstant.bk_dir);
				}
				if(strcmp(bkName,"time")==0){
					bkTime = (struct bk_time *)malloc(sizeof(struct bk_time));
					for (; isspace(*r)&&r<p ; ++r);
					if(*r!='('){
						printf ("error config file with %s:missing (\n",line);
						exit(1);
					}
					for (++r; isspace(*r)&&r<p ; ++r);
					if(!isdigit(*r)){
						printf ("error config file with %s: mand backup option \
						is not digital\n",line);
						exit(1);
					}
					bkTime->day = *r - 48;
					   
					/*get the backup time*/
					for (++r; isspace(*r)&&r<p ; ++r);
					if(*r!=','){
						printf ("error config file with %s:missing ,\n",line);
						exit(1);
					}
					for (++r; isspace(*r)&&r<p ; ++r);
				    if(*r!='\"'){
						printf ("error config file with %s:missing \"\n",line);
						exit(1);
					}
				    needle = ++r;
				    for (; *r!='\"'&&!isspace(*r)&&r<p ; ++r);
					if(*r!='\"'){
						printf ("error config file with %s:missing \"\n",line);
						exit(1);
					}
				    strncpy(stamptime,needle,r-needle);
				    stamptime[r-needle] = '\0';
					bkTime->time = unix_time (stamptime);
					/*check the end of time*/
				    for (++r; isspace(*r)&&r<p ; ++r);
					if(*r!=')'){
						printf ("error config file with %s:missing )\n",line);
						exit(1);
					}
					for (++r; isspace(*r)&&r<p ; ++r);
					if(*r!=';'){
						printf ("error config file with %s:missing ;\n",line);
						exit(1);
					}
					if(r!=p){
						printf ("error config file with %s:extra end of this line\n",line);
						exit(1);
					}
					ldprintf(DEBUG_CONF,"%d : %d\n",bkTime->day,bkTime->time);
					if(!bkInstant.bkTimeHead){
						bkInstant.bkTimeHead = bkTime;
						bkInstant.bkTimeTail = bkTime;
					}else{
						bkInstant.bkTimeTail->next = bkTime;
						bkInstant.bkTimeTail = bkTime;
					}
				}
				break;
			default:
				printf ("error config file with %s\n",line);
				exit(1);
		}
	}
	return;
}
void *parser(char symbol,char (*line)[MSGLEN],char (*lineLeft)[MSGLEN],\
             char (*lineRight)[MSGLEN])
{
	
}

/*
 *callback function for process the messge from web
 *Example: backup manually 
*/
void lc_ctl()
{
	
}
/*this is the function for receive syslog
callback function
*/
void * lc_accept()
{
	int client_fd;	
	struct 		sockaddr_un addr;
	socklen_t 	addrlen = sizeof(addr);
	client_fd = accept(sock,(struct sockaddr *) &addr,(socklen_t *)&addrlen);
	if (client_fd < 0) {
        oops("accept",10);
        return;
    }
	if (setnonblock(client_fd) < 0)
            oops("failed to set client socket non-blocking\n",11);
	struct event *ev = malloc(sizeof(struct event));
    event_set(ev, client_fd, EV_READ|EV_PERSIST, (void *) lc_read, ev);
    event_add(ev, NULL);
}
/*
read syslog from syslog-ng
*/
void * lc_read(int fd, short event, struct event *arg)
{
    int len=0;
	char	msg[MAXLINE+1];
	memset(msg, 0, sizeof(msg));
	len = read(fd,msg,MAXLINE-2);
	ldprintf(DEBUG_MAIN,"^^^^^^^^^^^^^^^^^^^^^\n%s\n~~~~~~~~~~~~~~~~~~~~~\n",msg);
	updateLog(msg,len);
	printchopped(msg, len);
}

/*chopped the log into line*/
void printchopped(msg, len)
	char *msg;
	int len;
{
	auto int ptlngth;

	auto char *start = msg,*p,*end,tmpline[MAXLINE + 1];

	ldprintf(DEBUG_MAIN,"Message length: %d.\n", len);
	tmpline[0] = '\0';
	if ( parts != (char *) 0 )
	{
		ldprintf(DEBUG_MAIN,"Including part from messages.\n");
		strcpy(tmpline, parts);
		free(parts);
		parts = (char *) 0;
		ldprintf(DEBUG_MAIN,"###########begining#################\n");
		ldprintf(DEBUG_MAIN,"%s\n",msg);
		ldprintf(DEBUG_MAIN,"***********templine list************\n");
		ldprintf(DEBUG_MAIN,"%s\n",tmpline);
		ldprintf(DEBUG_MAIN,"&&&&&&&&&&&msg list&&&&&&&&&&&&&&&&&\n");
		if ( (strlen(msg) + strlen(tmpline)) > MAXLINE )
		{
			logerror("Cannot glue message parts together\n");
			printline(tmpline);
			start = msg;
		}
		else
		{
			ldprintf(DEBUG_MAIN,"Previous: %s\n", tmpline);
			ldprintf(DEBUG_MAIN,"Next: %s\n", msg);
			strcat(tmpline, msg);	/* length checked above */
			printline(tmpline);
			if ( (strlen(msg) + 1) == len )
				return;
			else
				start = strchr(msg, '\0') + 1;
		}
	}

	if ( msg[len-1] != '\0' )
	{
		msg[len] = '\0';
		/*get the left of log,then storing it into parts*/
		for(p= msg+len-1; *p != '\0' && p > msg; )
			--p;
		if(*p == '\0') p++;
		ptlngth = strlen(p);
		if ( (parts = malloc(ptlngth + 1)) == (char *) 0 )
			logerror("Cannot allocate memory for message part.\n");
		else
		{
			strcpy(parts, p);
			ldprintf(DEBUG_MAIN,"Saving partial msg: %s\n", parts);
			memset(p, '\0', ptlngth);
		}
	}

	do {
		end = strchr(start + 1, '\0');
		printline(start);
		start = end + 1;
	} while ( *start != '\0' ); //'\0' because memset(p, '\0', ptlngth); and len+2

	return;
}
/*
 * change \n to \0
 * 
 */

void updateLog(msg,len)
	char *msg;
	int len;
{
	register char *p, *q;
	register unsigned char c;
	int mark;

	p = msg;
	/*add by lizxf*/
	while ((c = *p)&&p<msg+len) {
		if (c == '\n' || c == 127){
			q = p+1;
			if(q==msg+len){
				*p = '\0';
			}else if (*q == '<') {
				mark = 0;
				*p = ' ';
				while (isdigit(*++q)&&q<msg+len)
				{
				   		mark = 2;
				}
				if(q==msg+len||(mark == 2&&*q == '>')){
							*p = '\0';
				}
			}else{
				*p = ' ';
			}
		}
		p++;
	}
	return;
}
/*insert the log into share memory*/
void printline(msg)
	char * msg;
{
	lc_stat.lc_receive++;
	if(log_cache_out-log_cache==1){
		lc_stat.lc_drop++;
		return;
	}else{
		memset(*log_cache,0,sizeof(*log_cache));
		strcpy(*log_cache,msg);
		log_cache = log_cache_start + (++log_cache - log_cache_start)%CACHESIZE;
	}
	ldprintf(DEBUG_MAIN,"%s\n",*log_cache);

	return;
}


/*
thread for process log (main function)
this is the function for process syslog
*/
void lc_transfer(){
	char cline[MSGLEN];
	char sline[MSGLEN+100];
	struct lc_subModule *sp;
	
	while(1){
		/*get msg from cache*/
		if(log_cache-log_cache_out==0){
			continue;
		}
		memset (cline, 0, sizeof(cline));
		memset (sline, 0, sizeof(sline));
		strcpy(cline,*log_cache_out);
		log_cache_out = log_cache_start + (++log_cache_out - log_cache_start)%CACHESIZE;
		ldprintf(DEBUG_TRAN,"%s\n",cline);
		
		/*check and split the message assem it into a sql sentence*/
		if(assemLog(&cline,&sline,&sp)){
			/*insert message into db*/
			sql_insert(sp,&sline);
			lc_stat.lc_db_succ++;
		}else{
			lc_stat.lc_db_fail++;
		}
	}
	return;
}

/*
 * Take a raw input line, decode the message, and print the message
 * on the appropriate log files.
 */

int assemLog(msg,sqlMsg,asp)
	char (*msg)[MSGLEN];
	char (*sqlMsg)[MSGLEN+100];
	struct lc_subModule **asp;
{
	register char *p, *q,*left;
	register unsigned char c;
	int pri;
	int fac, prilev,msglen;
	char sqlTemp[MSGLEN+100];
	char spfield[MSGLEN+100];
	char spvalue[MSGLEN+100];
	char svfield[MSGLEN+100];
	char svvalue[MSGLEN+100];
	char fvalue[MSGLEN+100];
	char hostname[50];
	char nMark[20];
	struct parser_list *parserTemp;
	struct value_list *valueTemp;
	time_t	now;
	ENTRY *ep;
	ENTRY *hConst;
	struct lc_subModule *sp;
	
	p = *msg;

	if (*p == '<') {
		pri = 0;
		while (isdigit(*++p))
		{
		   pri = 10 * pri + (*p - '0');
		}
		if (*p == '>')
			++p;
	}
	/*replace some symbol which has Ctrl*/
	q = sqlTemp;
	while ((c = *p++) && q < (sqlTemp+sizeof(sqlTemp) - 4)) {
		if (c < 040) {
			*q++ = '^';
			*q++ = c ^ 0100;
		} else
			*q++ = c;
	}
	*q = '\0';
	q = sqlTemp;
	msglen = strlen(sqlTemp);
	if (!(msglen < 16 || sqlTemp[3] != ' ' || sqlTemp[6] != ' ' ||
	    sqlTemp[9] != ':' || sqlTemp[12] != ':' || sqlTemp[15] != ' ')) {
		q += 16;
		msglen -= 16;
	}else{
		return 0;
	}

	(void) time(&now); //timestamp
	
	/* extract facility and priority level */
	fac = LOG_FAC(pri);
	prilev = LOG_PRI(pri);
	/*hostname*/
	for(;isspace(*q);q++) msglen--;
	p = q;
	for(;!isspace(*q);q++) msglen--;
	strncpy(hostname,p,q-p);
	hostname[q-p] = '\0';
	/*kernel mark/ submodule mark*/
	for(;isspace(*q);q++) msglen--;
	p = q;
	for(;*q!=':';q++) msglen--;
	strncpy(nMark,p,q-p);
	nMark[q-p] = '\0';
	q++; msglen--;
	if(strcmp(kmark,nMark)==0){
		/*kernel mark/ submodule mark*/
		for(;isspace(*q);q++) msglen--;
		p = q;
		for(;*q!=':'&&!isspace(*q);q++) msglen--;
		strncpy(nMark,p,q-p);
		nMark[q-p] = '\0';
		if(*q==':'){
			q++;msglen--;
		}
	}
	ep = hash_find(nMark,htab);
	if(ep){
		sp = (struct lc_subModule *)ep->data;
		*asp = sp;
		ldprintf(DEBUG_TRAN,"%s\n",sp->subName);
		/*split the log then assemble the sql*/
		for(;isspace(*q);q++) msglen--;
		p = q;
		spvalue[0] = '\0';
		spfield[0] = '\0';
		parserTemp = sp->template->pHead;
		while(!parserTemp){
			q = strchr(parserTemp->letter);
			if(parserTemp->field[0] != '\0'){
				strncpy(fvalue,p,q-p);
				fvalue[q-p] = '\0';
				if(spvalue[0]!='\0'){
					strcat(spvalue,sqlite3_mprintf(", %Q",fvalue));
				}else{
					strcat(spvalue,sqlite3_mprintf("%Q",fvalue));
				}
				strcat(spvalue,fvalue);
				if(spfield[0]=='\0'){
					strcat(spfield,"`");
				}else{
					strcat(spfield,",`");
				}
				strcat(spfield,parserTemp->field);
				strcat(spfield,"`");
			}
			q++;
			parserTemp = parserTemp->next;
		}
		left = q;
		valueTemp = sp->template->vHead;
		while(!valueTemp){
			switch(valueTemp->fvalue){
				case DATE:
					break;
				case HOST:
					break;
				case PRI:
					break;
				case FAC:
					break;
				case LEFT:
					break;
				default:
					
			}
			valueTemp = valueTemp->next;
		}
		return 1;
	}
	return 0;
}

/*
backup thread`s main function
usage:which is used to backup those main database
*/
void lc_backup(){


	
}


/*insert sql insto db*/
void sql_insert(sp,msg)
	struct lc_subModule *sp;
	char (*msg)[MSGLEN+100];
{
	int i;
	char msg_temp[MSGLEN+100];
	char * pErrMsg = 0;

	sprintf(msg_temp,"insert into nat(`content`) values('%s');",*msg);
	sqlite3_exec( sp->pModule->db,msg_temp, 0, 0, &pErrMsg);
	/*check the error message*/
	return;
}
/*log error from logcraft*/
void logerror(type)
	char *type;
{
	char buf[100];

	ldprintf(DEBUG_MAIN,"Called logerr, msg: %s\n", type);
	/*produce the its error syslog we need to edit the time and host*/
	if (errno == 0)
		(void) snprintf(buf, sizeof(buf), "LOG_CRAFT: %s", type);
	else
		(void) snprintf(buf, sizeof(buf), "LOG_CRAFT: %s: %s", type, strerror(errno));
	errno = 0;
	//logmsg(LOG_SYSLOG|LOG_ERR, buf, LocalHostName, ADDDATE);
	printline(buf);
	return;
}
/*print the debug infomation*/
int unix_time(time)
	char *time;
{
	char *p,*q;
	int s;
	char temp[5];
	q = time;
	p = strchr(q,':');
	strncpy(temp,q,p-q);
	temp[p-q] = '\0';
	s = atoi(temp)*3600;
	q = ++p;
	p = strchr(q,':');
	strncpy(temp,q,p-q);
	temp[p-q] = '\0';
	s += atoi(temp)*60;
	strcpy(temp,++p);
	s += atoi(temp);
	return s;
}
static void ldprintf(int mark,char *fmt,...){
	va_list ap;

	if(Debug&&debugging_on==mark){
		va_start(ap, fmt);
		vfprintf(stdout, fmt, ap);
		va_end(ap);

		fflush(stdout);
		return;
	}else{
		return;
	}
}
