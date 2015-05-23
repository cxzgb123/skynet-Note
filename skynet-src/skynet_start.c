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
 * @brief �����̳߳��Բ���ȡ��ģ����Ϣ���н��д���
 * @note ��ȡ��ʧ��ʱ�������Ǹ��ɺܵͣ���Ҫ����˯��������cpu���
 */
static void *
_worker(void *p) {
	/*���̵߳���Ϣ*/
	struct worker_parm *wp = p;
	/*��ȡID*/
	int id = wp->id;
	/*��ȡȨ��*/
	int weight = wp->weight
	/*��ȡ�ܵļ�����*/;
	struct monitor *m = wp->m;
	/*��ȡ���̵߳ļ�����*/
	struct skynet_monitor *sm = m->m[id]
	/*����߳�˽�пռ䴫��THREAD_WORKER*/;
	skynet_initthread(THREAD_WORKER);
	/*q��Ϊnull*/
	struct message_queue * q = NULL;
	for (;;) {
		/*����ȡ��ĳ��ģ��Ĺ�������*/
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
  * @brief ����skynetģ�������̵߳ķ�ʽ����
  * @param[in] thread ���̸߳���
  *
  */
static void
_start(int thread) {
	pthread_t pid[thread+3];
      /*�����ܵļ�������ֻ��һ����
	  �����߳���skynet monitor*/
	struct monitor *m = skynet_malloc(sizeof(*m));
	memset(m, 0, sizeof(*m));
	/*counter ��¼����ҵ���̵߳�����*/
	m->count = thread;
	m->sleep = 0;

	m->m = skynet_malloc(thread * sizeof(struct skynet_monitor *));
	int i;

	/*���������*/
	for (i=0;i<thread;i++) {
		m->m[i] = skynet_monitor_new();
	}

	/*��ʼ����*/
	if (pthread_mutex_init(&m->mutex, NULL)) {
		fprintf(stderr, "Init mutex error");
		exit(1);
	}

	/*��ʼ����������*/
	if (pthread_cond_init(&m->cond, NULL)) {
		fprintf(stderr, "Init cond error");
		exit(1);
	}

	/*����ҵ�����������ʱ�ӣ��׽��ֹ���
	   ��ռ��3�����߳�*/ 
	create_thread(&pid[0], _monitor, m);
	create_thread(&pid[1], _timer, m);
	create_thread(&pid[2], _socket, m);

	 /*����ǰ32���̵߳�Ȩ��*/
	static int weight[] = { 
		-1, -1, -1, -1, 0, 0, 0, 0,
		1, 1, 1, 1, 1, 1, 1, 1, 
		2, 2, 2, 2, 2, 2, 2, 2, 
		3, 3, 3, 3, 3, 3, 3, 3, };
	/*worker_parm �ṹ���ڴ�Ÿ����̵߳�id��Ȩ�ص�����*/
	struct worker_parm wp[thread];

	/*�Ը����߳����ݽ������*/
	for (i=0;i<thread;i++) {
		/*������*/
		wp[i].m = m;
		wp[i].id = i;
		if (i < sizeof(weight)/sizeof(weight[0])) {
			wp[i].weight= weight[i];
		} else {
			/*��Ĭ��Ȩ�ص��̣߳�Ȩ��Ϊ0*/
			wp[i].weight = 0;
		}
		/*����ҵ���߳�*/
		create_thread(&pid[i+3], _worker, &wp[i]);
	}

	/*����,�ȴ�*/
	for (i=0;i<thread+3;i++) {
		pthread_join(pid[i], NULL); 
	}
	/*�ͷż��ӹ���*/
	free_monitor(m);
}

/**
  * @brief ����bootstrapģ��
  * @param[out] logger ���������Ϣ
  * @param[in] ģ��·��
  *
  */
static void
bootstrap(struct skynet_context * logger, const char * cmdline) {
	int sz = strlen(cmdline);
	char name[sz+1];
	char args[sz+1];
	sscanf(cmdline, "%s %s", name, args);
	/*���ظ�ģ��*/
	struct skynet_context *ctx = skynet_context_new(name, args);
	if (ctx == NULL) {
		/*����skynet��δ��ȫ��������
		    ���ֱ��ʹ��error�̵߳�ҵ����ͨ�������*/
		skynet_error(NULL, "Bootstrap error : %s\n", cmdline);
		skynet_context_dispatchall(logger);
		exit(1);
	}
}

/**
  * @brief ��ʼ����Ҫ�ṹ������ģ�飬��������.
  * @param[in] config �����ļ�����ṹ
  *
  */
void 
skynet_start(struct skynet_config * config) {
	if (config->daemon) {
		/*��������ת��Ϊ��̨����*/
		if (daemon_init(config->daemon)) {
			exit(1);
		}
	}
	/*����ȫ�ֽڵ�ID*/
	skynet_harbor_init(config->harbor);

	/*��ʼ��ģ�����ṹ*/
	skynet_handle_init(config->harbor);

	/*��ʼ����Ϣ���й���ṹ����ʽ����*/
	skynet_mq_init();

	/*��ʼ��ģ�����ṹ*/
	skynet_module_init(config->module_path);

	/*��ʼ��ʱ�����*/
	skynet_timer_init();

	/*��ʼ���׽���ȫ�ֹ���ṹ*/
	skynet_socket_init();
	
       /*logΪĬ�ϵ�ģ��*/
	struct skynet_context *ctx = skynet_context_new("logger", config->logger);
	if (ctx == NULL) {
		fprintf(stderr, "Can't launch logger service\n");
		exit(1);
	}
        
        /*����ģ��*/
	bootstrap(ctx, config->bootstrap);

	/*�����������̣߳���ʼ����ҵ��*/
	_start(config->thread);

	// harbor_exit may call socket send, so it should exit before socket_free
	skynet_harbor_exit();
	skynet_socket_free();
	if (config->daemon) {
		daemon_exit(config->daemon);
	}
}
