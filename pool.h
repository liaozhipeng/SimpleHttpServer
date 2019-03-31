#ifndef POOL_H
#define POOL_H

#include <stdlib.h>
#include <stdio.h>
#include <semaphore.h>
#include <pthread.h>
#include <setjmp.h>
#include "Glist.h"
#include <signal.h>
				

/*线程池状态*/
enum Gthread_pool_flag {RUN, SHUTDOWN}; 

/*工作线程状态*/
enum Gthread_pool_worker_state {BUSY, READY, BOOTING};

/*线程池工作线程和任务计数*/
struct mutex_pool_data{
						int worker_num;
						int task_num;
};

/*线程池*/
struct Gthread_pool{
					struct list_head task_list;
					struct list_head workers;
					enum Gthread_pool_flag flag;
					sem_t surplus_task_num;//信号量 大于0表示有任务待处理
					pthread_mutex_t info_lock; //互斥量
					pthread_mutex_t IO_lock;   //互斥量
					int max_tasks;   
					int max_workers;
					int min_workers;
					pthread_t manage_worker;
					pthread_t task_distribute_worker;
					struct mutex_pool_data mutex_data;
};

/*线程池任务*/
struct Gthread_pool_task{
						 void * (*proccess)(void * arg); 
						 void * arg;
						 struct list_head link_node;
};


struct Gthread_pool_worker;
/**/
struct Gthread_pool_worker_routline_args{
										 struct Gthread_pool * pool;
										 struct Gthread_pool_worker * this_worker;
};
/*工作线程*/
struct Gthread_pool_worker{
							pthread_t id;
							pthread_mutex_t worker_lock;//工作锁
							pthread_cond_t worker_cond;
							pthread_mutex_t boot_lock;//启动锁
							pthread_cond_t boot_cond;
							enum Gthread_pool_worker_state state;
							struct Gthread_pool_worker_routline_args routline_args;
							void * (*worker_task)(void * );
							void * worker_task_arg;
							struct list_head link_node;
};

#endif
