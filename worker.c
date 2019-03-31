#include "pool.h"
#include "config.h"

static void * worker_routline(void * arg);
static void * worker_manage(void * arg);
static void * distribute_task(void * arg);
static struct Gthread_pool_worker * search_idle_worker(struct Gthread_pool * pool);
static int add_worker(struct Gthread_pool_worker * new_worker, struct Gthread_pool * pool);
static int add_task(struct Gthread_pool_task * task, struct Gthread_pool * pool, void * (*proccess)(void * arg), void * arg);
static void sig_usr1_handler(int signum);
float get_pool_usage(struct Gthread_pool * pool);
static int del_worker(struct Gthread_pool_worker * worker_to_del, struct Gthread_pool * pool);

static struct timeval delay = {0, SLEEP_TIME}; 
static const float LODE_GATE = 0.2;

/*******************************************************
 * name:Gthread_pool_init
 * des:this func will init Gthread_pool struct 
 * para1: the pointer point to a Gthread_pool struct
 * para2:max number of tasks
 * para3:max number of workers
 * para4:min number of workers
 * return: state, SUCCESS or FAILURE
 * ****************************************************/
int Gthread_pool_init(struct Gthread_pool * pool, int max_tasks, int max_workers, int min_workers)
{
	assert(pool);  //assert用于保证满足某个特定条件，如果表达式为假，整个程序将退出，如果为真，则继续执行
	pool->max_tasks = max_tasks;
	pool->max_workers = max_workers;
	pool->min_workers = min_workers;
	pool->mutex_data.worker_num = pool->min_workers;
	pool->mutex_data.task_num = 0;
	
	pthread_mutex_init(&(pool->info_lock), NULL); //线程池成员互斥锁的初始化
#if DEBUG == 1
	pthread_mutex_init(&(pool->IO_lock), NULL);
#endif

	sem_init(&(pool->surplus_task_num), 0, 0);  //初始化任务信号量

	INIT_LIST_HEAD(&(pool->task_list));//任务链表初始化
	INIT_LIST_HEAD(&(pool->workers));//工作线程链表初始化
	
	for(int i = 0; i < pool->min_workers; i++)
	{
		struct Gthread_pool_worker * tmp = (struct Gthread_pool_worker *)malloc(sizeof(struct Gthread_pool_worker));//可能会有内存泄漏
		pthread_mutex_lock(&(pool->info_lock));//在工作线程队列中加成员要先加锁，保证线程安全
		add_worker(tmp, pool);//创建工作线程实例，加入到线程池的工作线程队列中
		pthread_mutex_unlock(&(pool->info_lock));
	}

	pthread_create(&(pool->manage_worker), NULL, worker_manage, (void *)pool);//创建管理线程
	pthread_create(&(pool->task_distribute_worker), NULL, distribute_task, (void *)pool);//创建任务分发线程

	return SUCCESS;
}

/************************************************************************
 * name:add_task
 * des:add a task to the task list in pool
 * para1:a pointer piont to Gthread_pool_task struct'
 * para2:a pointer point to Gthread_pool
 * para3:a pointer point to the proccess function of the task
 * para4:a pointer point to the arg of the process function
 * return :state
 * **********************************************************************/
int add_task(struct Gthread_pool_task * task, struct Gthread_pool * pool, void * (*proccess)(void * arg), void * arg)
{
	assert(task);
	assert(pool);	

	task->proccess = proccess;
	task->arg = arg;
	list_add_tail(&(task->link_node), &(pool->task_list));
	return SUCCESS;
}

/************************************************************************
 * name:add_worker
 * des:add a idle worker to the worker list in pool
 * para1:a pointer piont to Gthread_pool_worker struct'
 * para2:a pointrt point to Gthread_pool
 * return :the pointer piont to new worker
 * **********************************************************************/
int add_worker(struct Gthread_pool_worker * new_worker, struct Gthread_pool * pool)
{
	int err;
	assert(new_worker);
	assert(pool);

	new_worker->state = BOOTING;
	new_worker->routline_args.pool = pool;
	new_worker->routline_args.this_worker = new_worker;
	pthread_mutex_init(&(new_worker->worker_lock), NULL);
	pthread_cond_init(&(new_worker->worker_cond), NULL); 
	pthread_mutex_init(&(new_worker->boot_lock), NULL);
	pthread_cond_init(&(new_worker->boot_cond), NULL); 

	pthread_mutex_lock(&(new_worker->boot_lock));//启动锁

	err = pthread_create(&(new_worker->id), NULL, worker_routline, &(new_worker->routline_args));

	if(err != 0)
	  return FAILURE;
	
	while(new_worker->state == BOOTING)//等待工作线程启动成功后才继续进行
		pthread_cond_wait(&(new_worker->boot_cond), &(new_worker->boot_lock));
	pthread_mutex_unlock(&(new_worker->boot_lock));

	list_add_tail(&(new_worker->link_node), &(pool->workers));//把工作线程加入到线程池的链表队列中
	return SUCCESS;
}

/* ********************************************************************
 *name:distribute_task
 *des: distribute the tasks to idle thread
 *para:a pointer point to Gthread_pool
 *return: void *
 * *******************************************************************/
void * distribute_task(void * arg)
{
	pthread_detach(pthread_self());
	assert(arg);

	struct Gthread_pool * pool = (struct Gthread_pool *)arg;
	struct Gthread_pool_worker * idle_worker;
	struct Gthread_pool_task * task_to_distribute;
	struct list_head * pos;

#if DEBUG == 1
	pthread_mutex_lock(&(pool->IO_lock));
	printf("Now start to schedule!\n");
	pthread_mutex_unlock(&(pool->IO_lock));
#endif
	while(1)
	{
		sem_wait(&(pool->surplus_task_num));//获取一个任务，如果没有任务，就会阻塞在这里
		if(pool->flag == SHUTDOWN)
			pthread_exit(NULL);
		pos = pool->task_list.next;
		task_to_distribute = list_entry(pos, struct Gthread_pool_task, link_node);//获取任务指针
		idle_worker = search_idle_worker(pool);
		if(idle_worker != NULL)//找一个空闲工作线程，分发任务给他
		{
			pthread_mutex_lock(&(pool->info_lock));
			(pool->mutex_data.task_num)--;
			list_del(pos);//从任务队列移除已经分发的任务
			pthread_mutex_unlock(&(pool->info_lock));

			pthread_mutex_lock(&(idle_worker->worker_lock));
			idle_worker->worker_task = task_to_distribute->proccess;
			idle_worker->worker_task_arg = task_to_distribute->arg;
			pthread_cond_signal(&(idle_worker->worker_cond));//唤醒工作线程
			pthread_mutex_unlock(&(idle_worker->worker_lock));
			free(task_to_distribute);
			continue;
		}
		else
		{
			if(pool->mutex_data.worker_num < pool->max_workers)//如果没有空闲线程且线程池工作线程数小于最大阈值，就新建一个工作线程
			{
				idle_worker = (struct Gthread_pool_worker *)malloc(sizeof(struct Gthread_pool_worker));
				if( NULL == idle_worker )
				{
#if DEBUG == 1
					pthread_mutex_lock(&(pool->IO_lock));
					printf("malloc a new worker in distribute_task function failed!");
					pthread_mutex_unlock(&(pool->IO_lock));
#endif
					exit(15);
				}
				pthread_mutex_lock(&(pool->info_lock));
			    if( FAILURE == add_worker(idle_worker, pool) )//如果添加失败就加一个任务信号量 continue
				{
					pthread_mutex_unlock(&(pool->info_lock));
					sem_post(&(pool->surplus_task_num));
					free(idle_worker);
					continue;
				}
				pool->mutex_data.worker_num++;
				pool->mutex_data.task_num--;
				list_del(pos);
				pthread_mutex_unlock(&(pool->info_lock));

				pthread_mutex_lock(&(idle_worker->worker_lock));
				idle_worker->worker_task = task_to_distribute->proccess;
				idle_worker->worker_task_arg = task_to_distribute->arg;
				idle_worker->state = BUSY;
				pthread_cond_signal(&(idle_worker->worker_cond));//发送一个信号唤醒工作线程
				pthread_mutex_unlock(&(idle_worker->worker_lock));
				free(task_to_distribute);
				continue;
			}

			sem_post(&(pool->surplus_task_num)); //不能创建新的工作线程的话，把任务信号量加一，等待下一次处理 
			select(0, NULL, NULL, NULL, &delay);
			continue;
		}
	}	
}

/* ********************************************************************
 *name:search_idle_worker
 *des: search for the worker list, and find a idle worker, the idle worker found will be set busy
 *para:a pointer piont to a Gthread_pool struct
 *return: a pointer piont to a Gthread_pool_worker struct, if none, return NULL 
 * *******************************************************************/
struct Gthread_pool_worker * search_idle_worker(struct Gthread_pool * pool)
{
	assert(pool);
	struct list_head * pos;
    struct Gthread_pool_worker * tmp_worker;
	
	pthread_mutex_lock(&(pool->info_lock));
	list_for_each(pos, &(pool->workers))
	{
		tmp_worker = list_entry(pos, struct Gthread_pool_worker, link_node);
		pthread_mutex_lock(&(tmp_worker->worker_lock));
		if(tmp_worker->state == READY)//如果找到空闲线程 就返回它的指针
		{
			tmp_worker->state = BUSY;//把线程状态改变成BUSY
			pthread_mutex_unlock(&(tmp_worker->worker_lock));
			pthread_mutex_unlock(&(pool->info_lock));
			return tmp_worker;
		}

		else
			pthread_mutex_unlock(&(tmp_worker->worker_lock));
	}
	pthread_mutex_unlock(&(pool->info_lock));
	return NULL;
}

/********************************************************************************
 * name:worker_routline
 * des:the thread funtion for every worker
 * para:a void pointer
 * return: a void pointer
 * *****************************************************************************/
void * worker_routline(void * arg)
{
	pthread_detach(pthread_self());//工作线程分离
	struct Gthread_pool * pool = (*((struct Gthread_pool_worker_routline_args *)arg)).pool;//获取线程池
	struct Gthread_pool_worker * this_worker = (*((struct Gthread_pool_worker_routline_args *)arg)).this_worker;//获取该线程的对象
	
	sigset_t block_set;//设置子线程的信号屏蔽集
	sigemptyset(&block_set);
	sigaddset(&block_set, SIGUSR1);
	signal(SIGUSR1, sig_usr1_handler);//添加用户自定义信号1	的处理函数


#if DEBUG == 1
	pthread_sigmask(SIG_BLOCK, &block_set, NULL);
	pthread_mutex_lock(&(pool->IO_lock));
	printf("Now enter the worker thread: ID %ld\n", pthread_self());
	pthread_mutex_unlock(&(pool->IO_lock));
	pthread_sigmask(SIG_UNBLOCK, &block_set, NULL);
#endif	
	if(this_worker == NULL)
	{
#if DEBUG == 1
		pthread_sigmask(SIG_BLOCK, &block_set, NULL);
		pthread_mutex_lock(&(pool->IO_lock));
		printf("a thread can not get his info by id, his id is: %ld\n",pthread_self());
		pthread_mutex_unlock(&(pool->IO_lock));
		pthread_sigmask(SIG_UNBLOCK, &block_set, NULL);
#endif
		exit(16);
	}
	while(1)
	{
		pthread_mutex_lock(&(this_worker->worker_lock));	

		if(this_worker->state == BOOTING)//确定工作线程已经准备好接受任务
		{
			pthread_mutex_lock(&(this_worker->boot_lock));//启动锁加锁
			this_worker->state = READY;//改变工作线程状态
			pthread_cond_signal(&(this_worker->boot_cond));//唤醒启动条件变量
			pthread_mutex_unlock(&(this_worker->boot_lock));
		}

		pthread_cond_wait(&(this_worker->worker_cond), &(this_worker->worker_lock));//等待信号处理任务
		pthread_mutex_unlock(&(this_worker->worker_lock));

		if(pool->flag == SHUTDOWN)//线程池关闭
		{
#if DEBUG == 1
			pthread_mutex_lock(&(pool->IO_lock));
			printf("the worker thread, id: %ld will eixt!\n",pthread_self()); 
			pthread_mutex_unlock(&(pool->IO_lock));
#endif
			pthread_mutex_lock(&(pool->info_lock));
			pthread_mutex_destroy(&(this_worker->boot_lock));
			pthread_mutex_destroy(&(this_worker->worker_lock));
			pthread_cond_destroy(&(this_worker->boot_cond));
			pthread_cond_destroy(&(this_worker->worker_cond));
			
			list_del(&(this_worker->link_node));
			free(this_worker);
			pool->mutex_data.worker_num--;
			pthread_mutex_unlock(&(pool->info_lock));

			pthread_exit(NULL);
		}

		pthread_sigmask(SIG_BLOCK, &block_set, NULL);
		(*(this_worker->worker_task))(this_worker->worker_task_arg);//处理任务
		pthread_sigmask(SIG_UNBLOCK, &block_set, NULL);
		
		pthread_mutex_lock(&(this_worker->worker_lock));
		this_worker->state = READY;//如果有任务分发到这个线程，会把他弄成BUSY状态，所以要把这里弄回READY状态
		pthread_mutex_unlock(&(this_worker->worker_lock));

#if DEBUG == 1
		pthread_sigmask(SIG_BLOCK, &block_set, NULL);
		pthread_mutex_lock(&(pool->IO_lock));
		printf("The worker %ld has finish a task!\n", pthread_self()); 
		pthread_mutex_unlock(&(pool->IO_lock));
		pthread_sigmask(SIG_UNBLOCK, &block_set, NULL);
#endif
	}
}

/*************************************************************************************
 * name: get_pool_usage
 * des: to get the usage of the workers , when the number of wokers now is not bigger than min_worker_num, this function will return 1;
 * para: a ponter point to a thread pool
 * return: float type, the usage of the workers
 * **********************************************************************************/
float get_pool_usage(struct Gthread_pool * pool)
{
	int total, busy_num = 0;
	struct list_head * pos;
	struct Gthread_pool_worker * a_worker;
	pthread_mutex_lock(&(pool->info_lock));

	total = pool->mutex_data.worker_num;
	if(total <= pool->min_workers)//如果当前工作线程总数小于最少工作线程，返回1
	{
		pthread_mutex_unlock(&(pool->info_lock));
		return	(float)1;
	}

	list_for_each(pos, &(pool->workers))//循环工作线程链表，统计BUSY线程
	{
		a_worker = list_entry(pos, struct Gthread_pool_worker, link_node);
		pthread_mutex_lock(&(a_worker->worker_lock));
		if(a_worker->state == BUSY)
			busy_num++;
		pthread_mutex_unlock(&(a_worker->worker_lock));
	}

	pthread_mutex_unlock(&(pool->info_lock));
	return ((float)busy_num)/((float)total);
}

/********************************************************************************
 * name:del_worker:
 * des:delet a worker in thread pool
 * para:a pointer point to a worker
 * return: SUCCESS or FAILURE
 * *****************************************************************************/
int del_worker(struct Gthread_pool_worker * worker_to_del, struct Gthread_pool * pool)
{
	pthread_kill(worker_to_del->id, SIGUSR1);

	while(0 == pthread_kill(worker_to_del->id, 0) )//发送信号0检测线程是否存在
		select(0, NULL, NULL, NULL, &delay);
	pthread_mutex_destroy(&(worker_to_del->boot_lock));//释放资源
	pthread_mutex_destroy(&(worker_to_del->worker_lock));
	pthread_cond_destroy(&(worker_to_del->boot_cond));
	pthread_cond_destroy(&(worker_to_del->worker_cond));
#if DEBUG == 1
	pthread_mutex_lock(&(pool->IO_lock));
	printf("The worker %ld has been delete!\n", worker_to_del->id); 
	pthread_mutex_unlock(&(pool->IO_lock));
#endif
	
	list_del(&(worker_to_del->link_node));
	return SUCCESS; 
} 
/***************************************************************************
 * name:sig_usr1_handler
 * des: to handle the sigal SIGUSR1. here this signal will made the thread who capture it exit.
 * para:sig number
 * return: void
 * *************************************************************************/
void sig_usr1_handler(int signum)
{
	pthread_exit(NULL);
}

/* *************************************************************************
 *name:worker_manage
 *description: the manager will reduce the number of workers when usage is lower than lode_gate
 *void pointer(this pointer will be translated to a Gthread pool type pointer)
 *return: void pointer(it will be NULL)
 * *************************************************************************/
void * worker_manage(void * arg)
{
	assert(arg);
	pthread_detach(pthread_self());
	struct Gthread_pool * pool = (struct Gthread_pool *)arg;
	struct Gthread_pool_worker * worker_to_del;
	struct Gthread_pool_task * task_to_del;
	struct list_head * pos;
	sleep(1);
	while(1)
	{
		if(pool->flag == SHUTDOWN)//关闭线程池
		{
			sem_post(&(pool->surplus_task_num));//防止任务分发线程因为没有任务阻塞
			while(0 == pthread_kill(pool->task_distribute_worker, 0) )//0一般用来测试线程是否存在，等待任务分发线程退出
				select(0, NULL, NULL, NULL, &delay);

			pthread_mutex_lock(&(pool->info_lock));

			while( (pos = pool->workers.next) != &(pool->workers))
			{
				worker_to_del = list_entry(pos, struct Gthread_pool_worker, link_node);
				del_worker(worker_to_del, pool);
				free(worker_to_del);
				pool->mutex_data.worker_num--;
			}
			
			list_for_each(pos, &(pool->task_list))//删除所有任务
			{
				task_to_del = list_entry(pos, struct Gthread_pool_task, link_node);
				list_del(pos);
				free(task_to_del);
				pool->mutex_data.task_num--;
			}
			pthread_mutex_unlock(&(pool->info_lock));

			pthread_exit(NULL);
		}

		if(get_pool_usage(pool) < LODE_GATE)
		{
			worker_to_del = search_idle_worker(pool);
			if(worker_to_del == NULL)
			{
				sleep(1);
				continue;
			}
			pthread_mutex_lock(&(pool->info_lock));
			del_worker(worker_to_del, pool);
			free(worker_to_del);
			pool->mutex_data.worker_num--;
			pthread_mutex_unlock(&(pool->info_lock));
		}
		sleep(1);
	}
}

/* ***********************************************************************************************************
 *name:close_pool
 *description:close the pool
 *para1: a pointer point to a pool
 *return SUCCESS or FAILURE
 *************************************************************************************************************/
int close_pool(struct Gthread_pool * pool)
{
	assert(pool);
	pool->flag = SHUTDOWN;
#if DEBUG == 1
		pthread_mutex_lock(&(pool->IO_lock));
		printf("The Gthread pool will close!\n"); 
		pthread_mutex_unlock(&(pool->IO_lock));
#endif
	while(0 == pthread_kill(pool->manage_worker, 0) )//管理线程会控制其他所有进程的退出，最后主线程只要等待管理线程
		select(0, NULL, NULL, NULL, &delay);
	sem_destroy(&(pool->surplus_task_num));//释放资源
	pthread_mutex_destroy(&(pool->IO_lock));
	pthread_mutex_destroy(&(pool->info_lock));
	
	return SUCCESS; 
}

/* ***********************************************************************************************************
 *name:add_job
 *description:add a task to this pool 
 *para1: a pointer point to a pool
 *para2:a pointer point to a fucntion like this: void * (* func)(void * arg)
 *return SUCCESS or FAILURE
 *************************************************************************************************************/
int add_job(struct Gthread_pool * pool, void * (* job)(void * arg), void * arg)
{
	assert(pool);
	assert(arg);
	struct Gthread_pool_task * task_to_add; //创建一个任务体
	if(pool->flag == SHUTDOWN)
	{
		return FAILURE;
	}

	pthread_mutex_lock(&(pool->info_lock));
	task_to_add = (struct Gthread_pool_task *)malloc(sizeof(struct Gthread_pool_task)); //开辟一块内存空间
	if(task_to_add == NULL) //创建失败就退出
	{
		exit(34);
	}
	add_task(task_to_add, pool, job, arg);
	pool->mutex_data.task_num++;
	sem_post(&(pool->surplus_task_num));//任务信号量加1
	pthread_mutex_unlock(&(pool->info_lock));

	return SUCCESS;
}
