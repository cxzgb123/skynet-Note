#include "skynet.h"
#include "skynet_server.h"
#include "skynet_imp.h"
#include "skynet_mq.h"
#include "skynet_handle.h"
#include "skynet_module.h"
#include "skynet_timer.h"
#include "skynet_monitor.h"
#include "skynet_socket.h"
#include "skynet_daemon.h"

#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief monitor manager
 *
 */
struct monitor {
	int count;                        /*tot nums of moniter*/
	struct skynet_monitor ** m;       /*storage of monitor*/
	pthread_cond_t cond;              /*condition lock*/ 
	pthread_mutex_t mutex;            /*mutex lock*/
	int sleep;                        /*record thread drop into sleep*/
};

/**
 * @brief warp of monitor
 */
struct worker_parm {
	struct monitor *m;               /*montior hhandle*/       
	int id;                          /*id of this module*/
	int weight;                      /*weight of queue of this module*/
};

#define CHECK_ABORT if (skynet_context_total()==0) break;

/**
 * @brief start a thread
 * @param[in] thread thread handle
 * @param[in] callback for thread
 * @param[in] arg param for the thread
 */
static void
create_thread(pthread_t *thread, void *(*start_routine) (void *), void *arg) {
	if (pthread_create(thread,NULL, start_routine, arg)) {
		fprintf(stderr, "Create thread failed");
		exit(1);
	}
}

/**
 * @brief try to wake up one thread
 * @param[in] m monitor manager
 * @param[in] busy 
 *
 */
static void
wakeup(struct monitor *m, int busy) {
	if (m->sleep >= m->count - busy) {
		// signal sleep worker, "spurious wakeup" is harmless
		pthread_cond_signal(&m->cond);
	}
}

/**
 * @brief socket thread 
 * @param[in] p monitor of this module
 */
static void *
_socket(void *p) {
	struct monitor * m = p;
	skynet_initthread(THREAD_SOCKET);
	for (;;) {
	        /*deal with the event*/
		int r = skynet_socket_poll();
		if (r==0)
			break;
		if (r<0) {
			CHECK_ABORT
			continue;
		}
		/*wakeup one when sleep thread bigger than montior*/
		wakeup(m,0);
	}
	return NULL;
}

/**
 * @brief release the monitor manager
 * @param[in] m monitor manager
 *
 */
static void
free_monitor(struct monitor *m) {
	int i;
	int n = m->count;
	for (i=0;i<n;i++) {
		skynet_monitor_delete(m->m[i]);
	}
	pthread_mutex_destroy(&m->mutex);
	pthread_cond_destroy(&m->cond);
	skynet_free(m->m);
	skynet_free(m);
}

/**
 * @brief watch dog for check if the module exit
 *
 */
static void *
_monitor(void *p) {
	struct monitor * m = p;
	int i;
	int n = m->count;
	skynet_initthread(THREAD_MONITOR);
	for (;;) {
		CHECK_ABORT
		for (i=0;i<n;i++) {
			skynet_monitor_check(m->m[i]);
		}
		for (i=0;i<5;i++) {
			CHECK_ABORT
			sleep(1);
		}
	}

	return NULL;
}

/**
 * @brief watch dog for check if need to wake up more thread when load is too heavy for the server
 *
 */
static void *
_timer(void *p) {
	struct monitor * m = p;
	skynet_initthread(THREAD_TIMER);
	for (;;) {
		skynet_updatetime();
		CHECK_ABORT
		wakeup(m,m->count-1);
		usleep(2500);
	}
	// wakeup socket thread
	skynet_socket_exit();
	// wakeup all worker thread
	pthread_cond_broadcast(&m->cond);
	return NULL;
}

/**
 * @brief 工作线程尝试不断取出模块消息队列进行处理
 * @note 当取出失败时表明这是负荷很低，需要进行睡眠来减少cpu损耗
 */
static void *
_worker(void *p) {
	/*该线程的信息*/
	struct worker_parm *wp = p;
	/*获取ID*/
	int id = wp->id;
	/*获取权重*/
	int weight = wp->weight
	/*获取总的监视器*/;
	struct monitor *m = wp->m;
	/*获取该线程的监视器*/
	struct skynet_monitor *sm = m->m[id]
	/*向该线程私有空间传入THREAD_WORKER*/;
	skynet_initthread(THREAD_WORKER);
	/*q置为null*/
	struct message_queue * q = NULL;
	for (;;) {
		/*尝试取出某个模块的工作队列*/
		q = skynet_context_message_dispatch(sm, q, weight);
		if (q == NULL) {
			if (pthread_mutex_lock(&m->mutex) == 0) {
				++ m->sleep;
				// "spurious wakeup" is harmless,
				// because skynet_context_message_dispatch() can be call at any time.
				pthread_cond_wait(&m->cond, &m->mutex);
				-- m->sleep;
				if (pthread_mutex_unlock(&m->mutex)) {
					fprintf(stderr, "unlock mutex error");
					exit(1);
				}
			}
		} 
		CHECK_ABORT
	}
	return NULL;
}

/**
  * @brief 各个skynet模块以真线程的方式运作
  * @param[in] thread 真线程个数
  *
  */
static void
_start(int thread) {
	pthread_t pid[thread+3];
      /*这是总的监视器，只有一个，
	  各个线程有skynet monitor*/
	struct monitor *m = skynet_malloc(sizeof(*m));
	memset(m, 0, sizeof(*m));
	/*counter 记录的是业务线程的数量*/
	m->count = thread;
	m->sleep = 0;

	m->m = skynet_malloc(thread * sizeof(struct skynet_monitor *));
	int i;

	/*分配监视器*/
	for (i=0;i<thread;i++) {
		m->m[i] = skynet_monitor_new();
	}

	/*初始化锁*/
	if (pthread_mutex_init(&m->mutex, NULL)) {
		fprintf(stderr, "Init mutex error");
		exit(1);
	}

	/*初始化条件管理*/
	if (pthread_cond_init(&m->cond, NULL)) {
		fprintf(stderr, "Init cond error");
		exit(1);
	}

	/*除了业务外监视器，时钟，套接字管理
	   将占用3个真线程*/ 
	create_thread(&pid[0], _monitor, m);
	create_thread(&pid[1], _timer, m);
	create_thread(&pid[2], _socket, m);

	 /*设置前32个线程的权重*/
	static int weight[] = { 
		-1, -1, -1, -1, 0, 0, 0, 0,
		1, 1, 1, 1, 1, 1, 1, 1, 
		2, 2, 2, 2, 2, 2, 2, 2, 
		3, 3, 3, 3, 3, 3, 3, 3, };
	/*worker_parm 结构用于存放各个线程的id，权重等数据*/
	struct worker_parm wp[thread];

	/*对各个线程数据进行填充*/
	for (i=0;i<thread;i++) {
		/*填充参数*/
		wp[i].m = m;
		wp[i].id = i;
		if (i < sizeof(weight)/sizeof(weight[0])) {
			wp[i].weight= weight[i];
		} else {
			/*非默认权重的线程，权重为0*/
			wp[i].weight = 0;
		}
		/*启动业务线程*/
		create_thread(&pid[i+3], _worker, &wp[i]);
	}

	/*阻塞,等待*/
	for (i=0;i<thread+3;i++) {
		pthread_join(pid[i], NULL); 
	}
	/*释放监视管理*/
	free_monitor(m);
}

/**
  * @brief 加载bootstrap模块
  * @param[out] logger 用于输出信息
  * @param[in] 模块路径
  *
  */
static void
bootstrap(struct skynet_context * logger, const char * cmdline) {
	int sz = strlen(cmdline);
	char name[sz+1];
	char args[sz+1];
	sscanf(cmdline, "%s %s", name, args);
	/*加载该模块*/
	struct skynet_context *ctx = skynet_context_new(name, args);
	if (ctx == NULL) {
		/*这里skynet并未完全启动起来
		    因此直接使用error线程的业务处理通输出错误*/
		skynet_error(NULL, "Bootstrap error : %s\n", cmdline);
		skynet_context_dispatchall(logger);
		exit(1);
	}
}

/**
  * @brief 初始化必要结构，加载模块，启动服务.
  * @param[in] config 配置文件管理结构
  *
  */
void 
skynet_start(struct skynet_config * config) {
	if (config->daemon) {
		/*根据配置转换为后台服务*/
		if (daemon_init(config->daemon)) {
			exit(1);
		}
	}
	/*设置全局节点ID*/
	skynet_harbor_init(config->harbor);

	/*初始化模块管理结构*/
	skynet_handle_init(config->harbor);

	/*初始化消息队列管理结构，链式管理*/
	skynet_mq_init();

	/*初始化模块管理结构*/
	skynet_module_init(config->module_path);

	/*初始化时间管理*/
	skynet_timer_init();

	/*初始化套接字全局管理结构*/
	skynet_socket_init();
	
       /*log为默认的模块*/
	struct skynet_context *ctx = skynet_context_new("logger", config->logger);
	if (ctx == NULL) {
		fprintf(stderr, "Can't launch logger service\n");
		exit(1);
	}
        
        /*加载模块*/
	bootstrap(ctx, config->bootstrap);

	/*启动各个真线程，开始处理业务*/
	_start(config->thread);

	// harbor_exit may call socket send, so it should exit before socket_free
	skynet_harbor_exit();
	skynet_socket_free();
	if (config->daemon) {
		daemon_exit(config->daemon);
	}
}
