/*the src is the main part of logCraft
logCraft: a software for process Log from system
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
#define SOCKNAME "/dev/log"

int	Debug;			/* debug flag */
char db_location[512];
static short int bk_style;//0 nature 1 auto only 2 auto+mand
static short int bk_action;
static int debugging_on = 0;
pthread_t transfer_tid;
pthread_attr_t transfer_attr;
pthread_t backup_tid;
pthread_attr_t backup_attr;

char log_line[MSGLEN];
char (* log_cache)[MSGLEN];
char log_cache_start[CACHESIZE][MSGLEN];
sem_t *sem;
static int i; //the mark of cache for accept syslog
int sock;
char * parts;//the left part of message
/*the core function of main:
init the arguments and environment
get message from other process (syslog-ng / web)
*/
void * lc_accept();
void * lc_read(int fd, short event, struct event *arg);
int setnonblock(int fd);
void sql_insert(char msg[512]);
void printchopped(char*,int);
void updateLog(char *msg,int len);
void printline(char msg[512]);
static void lc_dprintf(char *, ...);
void logerror(char *type);
/**/
void main(int argc,char *argv[])
{
	char sockname[]	= SOCKNAME;
	struct 		sockaddr_un addr;
	socklen_t 	addrlen;
	time_t		now;
	int  msgnum	= 0;
	struct event *ev;
	i = 0;
	/**/
	if(argc>=2){
		Debug = 0;
		strcpy(db_location,argv[1]);
	}else{
		Debug = 0;
		printf("please point the db for insert log\n");
		return;
	}
	if(argc>=3){
		Debug = 1;
	}
	/**/
	log_cache		= log_cache_start;
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
	/* 设置客户端socket为非阻塞模式。 */
    if (setnonblock(sock) < 0)
            oops("failed to set logCraft socket non-blocking",4);
	/*Init libevent*/
	event_init();
	event_set(ev,sock,EV_READ|EV_PERSIST,(void *)lc_accept,ev);
	event_add(ev,NULL);
	event_dispatch();
}

/**
 * 将一个socket设置成非阻塞模式
 */
int
setnonblock(int fd)
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
void lc_init(){

}
/*
	callback function for process the messge from web
*/
void lc_ctl(){
	
}
/*this is the function for receive syslog
callback function
*/
void * lc_accept(){
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
	char	msg[MAXLINE];
	len = read(fd,msg,MAXLINE-2);
	//if(Debug){
	//	printf("[sink]: %s\n",msg);
	//	fflush(stdout);
	//}
	updateLog(msg,len);
	printchopped(msg, len+2);
}

/*chopped the log into line*/
void printchopped(msg, len)
	char *msg;
	int len;
{
	auto int ptlngth;

	auto char *start = msg,*p,*end,tmpline[MAXLINE + 1];

	lc_dprintf("Message length: %d.\n", len);
	tmpline[0] = '\0';
	if ( parts != (char *) 0 )
	{
		lc_dprintf("Including part from messages.\n");
		strcpy(tmpline, parts);
		free(parts);
		parts = (char *) 0;
		printf("****************************\n");
		printf("%s",msg);
		printf("****************************\n");
		printf("%s",tmpline);
		printf("&&&&&&&&&&&&&&&&&&&&&&&&&&&&\n");
		if ( (strlen(msg) + strlen(tmpline)) > MAXLINE )
		{
			logerror("Cannot glue message parts together\n");
			printline(tmpline);
			start = msg;
		}
		else
		{
			lc_dprintf("Previous: %s\n", tmpline);
			lc_dprintf("Next: %s\n", msg);
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
			lc_dprintf("Saving partial msg: %s\n", parts);
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
	char msg[512];
{
	int l=0;
	strcpy(*log_cache,msg);
	if(Debug){
		printf("[sink]: %s",*log_cache);
		fflush(stdout);
	}
	//sem_post(sem);  ///here you need to control rollback of cache with accept
	if(i==CACHESIZE){
		log_cache = log_cache_start;
	}else{
		log_cache++;
	}
	i++;
}

/*
 * Take a raw input line, decode the message, and print the message
 * on the appropriate log files.
 */

void anlanyseLog(msg)
	char *msg;
{
	register char *p, *q;
	register unsigned char c;
	char line[MAXLINE + 1];
	int pri;

	/* test for special codes */
	pri = DEFUPRI;
	p = msg;

	if (*p == '<') {
		pri = 0;
		while (isdigit(*++p))
		{
		   pri = 10 * pri + (*p - '0');
		}
		if (*p == '>')
			++p;
	}
	if (pri &~ (LOG_FACMASK|LOG_PRIMASK))
		pri = DEFUPRI;

	memset (line, 0, sizeof(line));
	q = line;
	while ((c = *p++) && q < &line[sizeof(line) - 4]) {
		if (c == '\n' || c == 127)
			*q++ = ' ';
		else if (c < 040) {
			*q++ = '^';
			*q++ = c ^ 0100;
		} else
			*q++ = c;
	}
	*q = '\0';

	//logmsg(pri, line, SYNC_FILE);
	return;
}


/*
thread for process log (main function)
this is the function for process syslog
*/
void lc_transfer(){
	char msg[MSGLEN];
	/*get msg from cache*/
	/*check and split the message*/
	/*insert message into db*/
	//sql_insert(msg);
	/*free the log_cache*/
}
/*
backup thread`s main function
usage:which is uesd to backup those main database
*/
void lc_backup(){


	
}


/*insert sql insto db*/
void sql_insert(msg)
	char msg[512];
{
	int rc,i;
	char msg_temp[600];
	sqlite3 *db;
	rc = sqlite3_open(db_location, &db);
	printf("%s",db_location);
	char * pErrMsg = 0;
	if( rc )
	{
		fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(db));
		sqlite3_close(db);
	}
	else{ 
		//printf("You have opened a sqlite3 database \n");
		sprintf(msg_temp,"insert into nat(`content`) values('%s');",msg);
		sqlite3_exec( db,msg_temp, 0, 0, &pErrMsg);
	}
	sqlite3_close(db); //关闭
}
/*log error from logCraft*/
void logerror(type)
	char *type;
{
	char buf[100];

	lc_dprintf("Called logerr, msg: %s\n", type);
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
static void lc_dprintf(char *fmt, ...)

{
	va_list ap;

	if ( !(Debug && debugging_on) )
		return;
	
	va_start(ap, fmt);
	vfprintf(stdout, fmt, ap);
	va_end(ap);

	fflush(stdout);
	return;
}