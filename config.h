#ifndef CONFIG_H 
#define	CONFIG_H

#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <getopt.h>
#include <semaphore.h>
#include <string.h>
#include <assert.h>
#include "pool.h"

#define DEBUG 0
#define SLEEP_TIME 2
#define SUCCESS 1
#define FAILURE -1 
#define	 DATA_IN 0
#define DATA_OUT 1

/*服务器配置结构体*/ 
struct server_conf{
				char CGIRoot[128];
				char DefaultFile[128];
				char DocumentRoot[128];
				char ConfigFile[128];
				int ListenPort;
				int MaxClient;
				int TimeOut;
				int InitWorkerNum;
				int MaxWoerkerNum;
};


extern struct server_conf server_para;

int DisplayConf();//显示配置

int GetParaFromFile(char * file);//从文件读取配置	

int GetParaFromCmd(int argc, char * argv[]);//从命令行读取配置

int Gthread_pool_init(struct Gthread_pool * pool, int max_tasks, int max_workers, int min_workers);

int close_pool(struct Gthread_pool * pool);//关闭线程池

int add_job(struct Gthread_pool * pool, void * (* job)(void * arg), void * arg);

void add_event(int epoll_fd, int fd, int event_type);//添加epoll监听事件

void del_event(int epoll_fd, int fd, int event_type);//删除epoll监听事件

#endif

