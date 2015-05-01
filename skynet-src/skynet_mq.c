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
	int cap;                             /*队列大小*/
	int head;                           /*队头索引*/
	int tail;                             /*队尾索引*/
	int lock;                            /* 队列锁*/
	int release;
	int in_global;                     /*标志该队列是否在全局消息队列中*/
	int overload;
	int overload_threshold;       /*队列满时标注为MQ_OVERLOAD,空时恢复*/
	struct skynet_message *queue;   /*队列指针*/
	struct message_queue *next;     /*用于链接入全局消息管理结构*/
};

/**
  * @brief 全局消息管理结构
  * @note 队列运作，链表管理  
  *
  */
struct global_queue {
	struct message_queue *head;     /*队头*/
	struct message_queue *tail;       /*队尾*/
	int lock;                                 /*锁*/
};

static struct global_queue *Q = NULL; /*全局管理结构*/

#define LOCK(q) while (__sync_lock_test_and_set(&(q)->lock,1)) {}    /*原子获锁*/
#define UNLOCK(q) __sync_lock_release(&(q)->lock);                     /*原子解锁*/       

/**
 * @brief 全局消息队列中挂入一个
 *    模块的消息队列
 * @param[in|out] queue 待挂入的消息队列
 *
 */
void 
skynet_globalmq_push(struct message_queue * queue) {
	struct global_queue *q= Q;
	/*获取锁*/
	LOCK(q)
	/*将其链接入全局队列尾部*/
	assert(queue->next == NULL);
	if(q->tail) {
		q->tail->next = queue;
		q->tail = queue;
	} else {
		q->head = q->tail = queue;
	}
	/*释放锁*/
	UNLOCK(q)
}

/**
  * @brief 从全局消息管理中弹出一个
  *   属于某个模块的消息队列
  * @return 消息队列
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
 * @brief 创建并初始化模块的全局队列
 * @param[handle] hanedle 该模块的索引
 * return 返回该模块的队列
 */
struct message_queue * 
skynet_mq_create(uint32_t handle) {
	struct message_queue *q = skynet_malloc(sizeof(*q));
	q->handle = handle;
	q->cap = DEFAULT_QUEUE_SIZE;     /*每个模块的循环队列默认大小*/
	q->head = 0;					    /*循环队列头索引初始化*/
	q->tail = 0;					    /*循环队列尾巴初始化*/	
	q->lock = 0;					    /*循环队列锁*/
	// When the queue is create (always between service create and service init) ,
	// set in_global flag to avoid push it to global queue .
	// If the service init success, skynet_context_new will call skynet_mq_force_push to push it to global queue.
	q->in_global = MQ_IN_GLOBAL;   /*该队列在全局队列中*/
	q->release = 0;			
	q->overload = 0;
	q->overload_threshold = MQ_OVERLOAD;
	q->queue = skynet_malloc(sizeof(struct skynet_message) * q->cap);
	q->next = NULL;

	return q;
}

/**
  * @brief 释放该模块的循环队列
  * @param[in|out] q 指向等待释放的循环队列 
  */
static void 
_release(struct message_queue *q) {
	assert(q->next == NULL);
	skynet_free(q->queue);
	skynet_free(q);
}

/**
 * @brief 返回该循环队列所属的模块
 * @return 模块索引
 */
uint32_t 
skynet_mq_handle(struct message_queue *q) {
	return q->handle;
}

/**
  * @brief 返回模块循环队列的长度
  * @param[in|out] q 模块的队列指针
  * @return 模块的队列长度
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
  * @brief 弹出负荷值, 并将原来的值置为 1
  * @param[in] q 指向待处理的循环队列
  * @return 载荷大小
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
  * @brief 消息出队
  * @param[out|out] message_queue 模块消息管理队列
  * @param[out] message 用于存放出队的消息
  *
  */
int
skynet_mq_pop(struct message_queue *q, struct skynet_message *message) {
	int ret = 1;
	/*获取锁*/
	LOCK(q)              
      /*判断队列中是否还有元素*/            
	if (q->head != q->tail) {
		/*当队列中有数据时, 先取出一个*/
		*message = q->queue[q->head++];
		ret = 0;
		int head = q->head;
		int tail = q->tail;
		int cap = q->cap;

	      /*当发现头部到达数组尾巴时需重置到头部*/
		if (head >= cap) {
			q->head = head = 0;
		}
		/*计算有效载荷*/
		int length = tail - head;
		if (length < 0) {
			length += cap;
		}
		/**重置载荷阈值和载荷记录*/
		while (length > q->overload_threshold) {
			q->overload = length;
			q->overload_threshold *= 2;
		}
	} else {
		// reset overload_threshold when queue is empty
		q->overload_threshold = MQ_OVERLOAD;
	}
      /*当发现模块的队列空时，设置队列标志
          为不在全局队列中*/
	if (ret) {
		q->in_global = 0;
	}
	/*释放锁*/
	UNLOCK(q) 

	return ret;
}

/**
  * @brief 模块队列扩张
  * @param[in|out] q 指向模块队列
  *
  */
static void
expand_queue(struct message_queue *q) {
	/*申请倍增空间*/
	struct skynet_message *new_queue = skynet_malloc(sizeof(struct skynet_message) * q->cap * 2);
	int i;
	/*TODO bug 这里是否可以优化*/
	/*拷贝空间*/
	for (i=0;i<q->cap;i++) {
		new_queue[i] = q->queue[(q->head + i) % q->cap];
	}
	/*设置属性*/
	q->head = 0;
	q->tail = q->cap;
	q->cap *= 2;
	/*释放旧空间*/
	skynet_free(q->queue);
	/*重设置空间指针*/
	q->queue = new_queue;
}
/**
  * @brief 向模块队列中推送一个message
  * @param[in|out] 指向模块队列的指针
  * @param[in] message 待推入的消息
  *
  */
void 
skynet_mq_push(struct message_queue *q, struct skynet_message *message) {
	assert(message);
	/*开锁*/
	LOCK(q)
      /*推入消息*/
	q->queue[q->tail] = *message;
	/*超过数组尾巴则重置tail的坐标为0*/
	if (++ q->tail >= q->cap) {
		q->tail = 0;
	}

	/*发现队列满了，则将队列倍增*/
	if (q->head == q->tail) {
		expand_queue(q);
	}
      /*模块队列中填充了新数据但
           发现该队列没挂入全局队列需要将其挂入全局队列*/
	if (q->in_global == 0) {
		/*设置标志挂入全局队列*/
		q->in_global = MQ_IN_GLOBAL;
		skynet_globalmq_push(q);
	}
	/*解锁*/
	UNLOCK(q)
}

/**
 * @brief 初始化全局管理队列
 *
 */
void 
skynet_mq_init() {
	struct global_queue *q = skynet_malloc(sizeof(*q));
	memset(q,0,sizeof(*q));
	Q=q;
}

void 
skynet_mq_mark_release(struct message_queue *q) {
	LOCK(q)
	assert(q->release == 0);
	q->release = 1;
	if (q->in_global != MQ_IN_GLOBAL) {
		skynet_globalmq_push(q);
	}
	UNLOCK(q)
}

/**
  * @brief 清空一个模块队列
  * @param[in] q 指向模块队列
  * @param[in] drop_func 用于清空队列的函数
  */
static void
_drop_queue(struct message_queue *q, message_drop drop_func, void *ud) {
	struct skynet_message msg;
	while(!skynet_mq_pop(q, &msg)) {
		drop_func(&msg, ud);
	}
	_release(q);
}

void 
skynet_mq_release(struct message_queue *q, message_drop drop_func, void *ud) {
	LOCK(q)
	
	if (q->release) {
		UNLOCK(q)
		_drop_queue(q, drop_func, ud);
	} else {
		skynet_globalmq_push(q);
		UNLOCK(q)
	}
}
