#include "skynet.h"
#include "skynet_mq.h"
#include "skynet_handle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

#define DEFAULT_QUEUE_SIZE 64
#define MAX_GLOBAL_MQ 0x10000

// 0 means mq is not in global mq.
// 1 means mq is in global mq , or the message is dispatching.

#define MQ_IN_GLOBAL 1
#define MQ_OVERLOAD 1024

struct message_queue {
	uint32_t handle;
	int cap;                             /*���д�С*/
	int head;                           /*��ͷ����*/
	int tail;                             /*��β����*/
	int lock;                            /* ������*/
	int release;
	int in_global;                     /*��־�ö����Ƿ���ȫ����Ϣ������*/
	int overload;
	int overload_threshold;       /*������ʱ��עΪMQ_OVERLOAD,��ʱ�ָ�*/
	struct skynet_message *queue;   /*����ָ��*/
	struct message_queue *next;     /*����������ȫ����Ϣ����ṹ*/
};

/**
  * @brief ȫ����Ϣ����ṹ
  * @note �����������������  
  *
  */
struct global_queue {
	struct message_queue *head;     /*��ͷ*/
	struct message_queue *tail;       /*��β*/
	int lock;                                 /*��*/
};

static struct global_queue *Q = NULL; /*ȫ�ֹ���ṹ*/

#define LOCK(q) while (__sync_lock_test_and_set(&(q)->lock,1)) {}    /*ԭ�ӻ���*/
#define UNLOCK(q) __sync_lock_release(&(q)->lock);                     /*ԭ�ӽ���*/       

/**
 * @brief ȫ����Ϣ�����й���һ��
 *    ģ�����Ϣ����
 * @param[in|out] queue ���������Ϣ����
 *
 */
void 
skynet_globalmq_push(struct message_queue * queue) {
	struct global_queue *q= Q;
	/*��ȡ��*/
	LOCK(q)
	/*����������ȫ�ֶ���β��*/
	assert(queue->next == NULL);
	if(q->tail) {
		q->tail->next = queue;
		q->tail = queue;
	} else {
		q->head = q->tail = queue;
	}
	/*�ͷ���*/
	UNLOCK(q)
}

/**
  * @brief ��ȫ����Ϣ�����е���һ��
  *   ����ĳ��ģ�����Ϣ����
  * @return ��Ϣ����
  *
  */
struct message_queue * 
skynet_globalmq_pop() {
	struct global_queue *q = Q;

	LOCK(q)
	struct message_queue *mq = q->head;
	if(mq) {
		q->head = mq->next;
		if(q->head == NULL) {
			assert(mq == q->tail);
			q->tail = NULL;
		}
		mq->next = NULL;
	}
	UNLOCK(q)

	return mq;
}

/**
 * @brief ��������ʼ��ģ���ȫ�ֶ���
 * @param[handle] hanedle ��ģ�������
 * return ���ظ�ģ��Ķ���
 */
struct message_queue * 
skynet_mq_create(uint32_t handle) {
	struct message_queue *q = skynet_malloc(sizeof(*q));
	q->handle = handle;			    /*���ж�Ӧ��ģ���id��*/
	q->cap = DEFAULT_QUEUE_SIZE;     /*ÿ��ģ���ѭ������Ĭ�ϴ�С*/
	q->head = 0;					    /*ѭ������ͷ������ʼ��*/
	q->tail = 0;					    /*ѭ������β�ͳ�ʼ��*/	
	q->lock = 0;					    /*ѭ��������*/
	// When the queue is create (always between service create and service init) ,
	// set in_global flag to avoid push it to global queue .
	// If the service init success, skynet_context_new will call skynet_mq_force_push to push it to global queue.
	q->in_global = MQ_IN_GLOBAL;   /*�ö�����ȫ�ֶ�����*/
	q->release = 0;			        /*��־���пռ��Ƿ�ȴ��ͷ�*/
	q->overload = 0;				 /*���и���*/	
	q->overload_threshold = MQ_OVERLOAD;   /*�����غɸ��µ�Ĭ�Ϸ�ֵ*/
	q->queue = skynet_malloc(sizeof(struct skynet_message) * q->cap); /*������пռ�*/
	q->next = NULL;						 /*������һ������*/

	return q;								/*����ģ�����*/
}

/**
  * @brief �ͷŸ�ģ���ѭ������
  * @param[in|out] q ָ��ȴ��ͷŵ�ѭ������ 
  */
static void 
_release(struct message_queue *q) {
	assert(q->next == NULL);
	skynet_free(q->queue);					/*�ͷ�ѭ������*/
	skynet_free(q);						/*�ͷŶ��й���ṹ*/
}

/**
 * @brief ���ظ�ѭ������������ģ��
 * @return ģ������
 */
uint32_t 
skynet_mq_handle(struct message_queue *q) {
	/*���ظ�ѭ�����ж�Ӧģ���ID*/
	return q->handle;
}

/**
  * @brief ����ģ��ѭ�����еĳ���
  * @param[in|out] q ģ��Ķ���ָ��
  * @return ģ��Ķ��г���
  */
int
skynet_mq_length(struct message_queue *q) {
	int head, tail,cap;

	LOCK(q)
	head = q->head;
	tail = q->tail;
	cap = q->cap;
	UNLOCK(q)
	
	if (head <= tail) {
		return tail - head;
	}
	/*TODO bug ? overflow cap - head + tail better*/
	return tail + cap - head;
}

/**
  * @brief ��������ֵ, ����ԭ����ֵ��Ϊ 1
  * @param[in] q ָ��������ѭ������
  * @return �غɴ�С
  *
  */
int
skynet_mq_overload(struct message_queue *q) {
	if (q->overload) {
		int overload = q->overload;
		q->overload = 0;
		return overload;
	} 
	return 0;
}

/**
  * @brief ��Ϣ����
  * @param[out|out] message_queue ģ����Ϣ�������
  * @param[out] message ���ڴ�ų��ӵ���Ϣ
  *
  */
int
skynet_mq_pop(struct message_queue *q, struct skynet_message *message) {
	int ret = 1;
	/*��ȡ��*/
	LOCK(q)              
      /*�ж϶������Ƿ���Ԫ��*/            
	if (q->head != q->tail) {
		/*��������������ʱ, ��ȡ��һ��*/
		*message = q->queue[q->head++];
		ret = 0;
		int head = q->head;
		int tail = q->tail;
		int cap = q->cap;

	      /*������ͷ����������β��ʱ�����õ�ͷ��*/
		if (head >= cap) {
			q->head = head = 0;
		}
		/*������Ч�غ�*/
		int length = tail - head;
		if (length < 0) {
			length += cap;
		}
		/**�����غ���ֵ���غɼ�¼*/
		/*TODO ��������Ż�*/
		while (length > q->overload_threshold) {
			q->overload = length;		/*�����غ�*/
			q->overload_threshold *= 2;	/*�����غ���ֵ������*/
		}
	} else {
		// reset overload_threshold when queue is empty
		q->overload_threshold = MQ_OVERLOAD;
	}
      /*������ģ��Ķ��п�ʱ�����ö��б�־
          Ϊ����ȫ�ֶ�����*/
	if (ret) {
		q->in_global = 0;
	}
	/*�ͷ���*/
	UNLOCK(q) 

	return ret;
}

/**
  * @brief ģ���������
  * @param[in|out] q ָ��ģ�����
  *
  */
static void
expand_queue(struct message_queue *q) {
	/*���뱶���ռ�*/
	struct skynet_message *new_queue = skynet_malloc(sizeof(struct skynet_message) * q->cap * 2);
	int i;
	/*TODO bug �����Ƿ�����Ż�*/
	/*�����ռ�*/
	for (i=0;i<q->cap;i++) {
		new_queue[i] = q->queue[(q->head + i) % q->cap];
	}
	/*��������*/
	q->head = 0;
	q->tail = q->cap;
	q->cap *= 2;
	/*�ͷžɿռ�*/
	skynet_free(q->queue);
	/*�����ÿռ�ָ��*/
	q->queue = new_queue;
}
/**
  * @brief ��ģ�����������һ��message
  * @param[in|out] ָ��ģ����е�ָ��
  * @param[in] message ���������Ϣ
  *
  */
void 
skynet_mq_push(struct message_queue *q, struct skynet_message *message) {
	assert(message);
	/*����*/
	LOCK(q)
      /*������Ϣ*/
	q->queue[q->tail] = *message;
	/*��������β��������tail������Ϊ0*/
	if (++ q->tail >= q->cap) {
		q->tail = 0;
	}

	/*���ֶ������ˣ��򽫶��б���*/
	if (q->head == q->tail) {
		expand_queue(q);
	}
      /*ģ�����������������ݵ�
           ���ָö���û����ȫ�ֶ�����Ҫ�������ȫ�ֶ���*/
	if (q->in_global == 0) {
		/*���ñ�־����ȫ�ֶ���*/
		q->in_global = MQ_IN_GLOBAL;
		skynet_globalmq_push(q);
	}
	/*����*/
	UNLOCK(q)
}

/**
 * @brief ��ʼ��ȫ�ֹ������
 *
 */
void 
skynet_mq_init() {
	struct global_queue *q = skynet_malloc(sizeof(*q));
	memset(q,0,sizeof(*q));
	Q=q;
}
/**
  * @brief ��һ��ģ����б�־Ϊ���ͷ�
  * @param[in] q ���ͷŵ�ģ����Ϣ����
  * 
  */
void 
skynet_mq_mark_release(struct message_queue *q) {
	/*��ȡ��*/
	LOCK(q)
	/**�ͷű�־δ������ȴ���ͷ���Ϊ�Ǵ����*/
	assert(q->release == 0);

	/*TODO ������øо�û��Ҫ*/
	/*�����ͷű�־*/
	q->release = 1;
	if (q->in_global != MQ_IN_GLOBAL) {
		skynet_globalmq_push(q);
	}
	/*�ͷ���*/
	UNLOCK(q)
}

/**
  * @brief ���һ��ģ����,������ɾ���
  * @param[in] q ָ��ģ�����
  * @param[in] drop_func ������ն��еĺ��
  * @param[in] ud ����ʱ��Ҫ����������
  */
static void
_drop_queue(struct message_queue *q, message_drop drop_func, void *ud) {
	struct skynet_message msg;
	/*��ն������������*/
	while(!skynet_mq_pop(q, &msg)) {
		/*��������������*/
		drop_func(&msg, ud);
	}
	/*�ͷŸ���Ϣ����*/
	_release(q);
}

/**
  * @brief �����ͷŶ���
  * @param[in] q ģ�����ָ��
  * @param[in] drop_func ��Ϣ��������
  * @param[in] ud ����Ϣ��Ӧ��ģ��ID��ָ��
  *
  */
void 
skynet_mq_release(struct message_queue *q, message_drop drop_func, void *ud) {
	LOCK(q)
	
	if (q->release) { 
		/*��ģ����д�����*/
		UNLOCK(q)
		/*��ոö���*/
		_drop_queue(q, drop_func, ud);
	} else {
		/*û�б��Ϊ���ͷţ����ö��з���ȫ�ֶ���*/
		skynet_globalmq_push(q);
		UNLOCK(q)
	}
}
