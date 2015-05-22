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

/**
 * @brief manager of queue for module
 * @note circular Queue
 */
struct message_queue {
	uint32_t handle;
	int cap;                             /*size of the queue*/
	int head;                            /*index of head*/
	int tail;                            /*index of tail*/
	int lock;                            /*mutex lock*/
	int release;                         /*mark the queue as release*/
	int in_global;                       /*check if the queue is in global*/
	int overload;                        /*overload record*/
	int overload_threshold;              /*trigger val for overload record*/ 
	struct skynet_message *queue;        /*pointer to the array*/
	struct message_queue *next;          /*used to link all queue for module together*/
};

/**
  * @brief global queue
  * @note all queue for module store in it as a list
  *
  */
struct global_queue {
	struct message_queue *head;       /*index for head */
	struct message_queue *tail;       /*index for tail*/
	int lock;                         /*mutex lock*/
};

static struct global_queue *Q = NULL;     /*one local example for global queue*/

#define LOCK(q) while (__sync_lock_test_and_set(&(q)->lock,1)) {}      /*get the mutex lock*/
#define UNLOCK(q) __sync_lock_release(&(q)->lock);                     /*release the mutex lock*/       

/**
 * @brief push one module queue into the global queue
 * @param[in|out] queue pointer to the module queue
 *
 */
void 
skynet_globalmq_push(struct message_queue * queue) {
	struct global_queue *q= Q;
	LOCK(q)
	assert(queue->next == NULL);
	if(q->tail) {
		q->tail->next = queue;
		q->tail = queue;
	} else {
		q->head = q->tail = queue;
	}
	UNLOCK(q)
}

/**
 * @brief pop one module queue from the global queue
 * @param[in|out] queue pointer for the module queue
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
 * @brief init the queue for the module
 * @param[in] handle index for the module
 * return success ? module : NUll
 */
struct message_queue * 
skynet_mq_create(uint32_t handle) {
	struct message_queue *q = skynet_malloc(sizeof(*q));/*alloc handle*/
	q->handle = handle;			            /*store the module index which the queue is belong to*/
	q->cap = DEFAULT_QUEUE_SIZE;                        /*the size of the queue*/
	q->head = 0;					    /*head index*/
	q->tail = 0;					    /*tail index*/	
	q->lock = 0;					    /*mutex lock*/
	// When the queue is create (always between service create and service init) ,
	// set in_global flag to avoid push it to global queue .
	// If the service init success, skynet_context_new will call skynet_mq_force_push to push it to global queue.
	q->in_global = MQ_IN_GLOBAL;                        /*all queue push into global queue at first*/
	q->release = 0;			                    /*mark the queue as wait to release*/
	q->overload = 0;					
	q->overload_threshold = MQ_OVERLOAD;                
	q->queue = skynet_malloc(sizeof(struct skynet_message) * q->cap); /*alloc the array fot the queue*/
	q->next = NULL;						 

	return q;								
}

/**
  * @brief just release the queue
  * @param[in|out] q handle of the queue
  * @note ignore the message in the queue
  */
static void 
_release(struct message_queue *q) {
	assert(q->next == NULL);
	skynet_free(q->queue);					
	skynet_free(q);					
}

/**
 * @brief get the index of module the queue belong to
 * @return module index
 */
uint32_t 
skynet_mq_handle(struct message_queue *q) {
	return q->handle;
}

/**
  * @brief get the nums of messages store in the module queue
  * @param[in|out] q handle of thq queue
  * @return nums of message
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
	return tail + cap - head;
}

/**
  * @brief try to get the overloead of the queue and reset the overload
  * @param[in] q handle of the queue
  * @return overload record
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
  * @brief try to pop one message from the module queue
  * @param[out|out] q hande of the message queue
  * @param[out] message used to get the message 
  *
  */
int
skynet_mq_pop(struct message_queue *q, struct skynet_message *message) {
	int ret = 1;
	LOCK(q)  
	/*message exist in the queue*/
	if (q->head != q->tail) {
		/*pop the message out*/
		*message = q->queue[q->head++];
		ret = 0;
		int head = q->head;
		int tail = q->tail;
		int cap = q->cap;


		if (head >= cap) {
			q->head = head = 0;
		}

		/*get the tot message in the queue*/
		int length = tail - head;
		if (length < 0) {
			length += cap;
		}
		
		/*try to set the overload mark*/
		/*overload_thread set as double when lenght is bigger than the overload_thershold*/
		while (length > q->overload_threshold) {
			q->overload = length;		
			q->overload_threshold *= 2;	
		}
	} else {
	        /*no message exis*/
		// reset overload_threshold when queue is empty
		/*TODO why not set the size of module queue as size/2*/
		q->overload_threshold = MQ_OVERLOAD;
	}
	/*no message in the module queue*/
	if (ret) {
	        /*the module queue should remove from the global queue*/
		q->in_global = 0; 
	}
	UNLOCK(q) 

	return ret;
}

/**
  * @brief expand the size of the module queue
  * @param[in|out] q hande of the queue
  *
  */
static void
expand_queue(struct message_queue *q) {
	/*alloc the new queue*/
	struct skynet_message *new_queue = skynet_malloc(sizeof(struct skynet_message) * q->cap * 2);
	int i;
	/*TODO could be better?*/
	/*restore the message from the origin queue*/
	for (i=0;i<q->cap;i++) {
		new_queue[i] = q->queue[(q->head + i) % q->cap];
	}
	q->head = 0;
	q->tail = q->cap;
	/*size set as double*/
	q->cap *= 2;
	skynet_free(q->queue);
	q->queue = new_queue;
}

/**
  * @brief push one message into the module queue
  * @param[in|out] q queue for the module
  * @param[in] message message wait to push into the module queue
  *
  */
void 
skynet_mq_push(struct message_queue *q, struct skynet_message *message) {
	assert(message);
	LOCK(q)
	q->queue[q->tail] = *message;
	if (++ q->tail >= q->cap) {
		q->tail = 0;
	}
        /*no more space , so we should expand the queue*/
	if (q->head == q->tail) {
		expand_queue(q);
	}
	/*try to push the module_queue into global queue after push new message into it*/
	if (q->in_global == 0) {
		q->in_global = MQ_IN_GLOBAL;
		skynet_globalmq_push(q);
	}
	UNLOCK(q)
}

/**
 * @brief alloc the global queue
 *
 */
void 
skynet_mq_init() {
	struct global_queue *q = skynet_malloc(sizeof(*q));
	memset(q,0,sizeof(*q));
	Q=q;
}

/**
  * @brief mark the queue as wait to release
  * @param[in] q handle of the queue
  * 
  */
void 
skynet_mq_mark_release(struct message_queue *q) {
	LOCK(q)
	assert(q->release == 0);
	/*mark release flag*/
	q->release = 1;
	/*push the module queue into the global queue, so can delete the queue when module queue active*/
	if (q->in_global != MQ_IN_GLOBAL) {
		skynet_globalmq_push(q);
	}
	UNLOCK(q)
}

/**
  * @brief drop message in the queue
  * @param[in] q handle of the queue
  * @param[in] drop_func function used to drop the message
  * @param[in] ud  module index for the queue
  */
static void
_drop_queue(struct message_queue *q, message_drop drop_func, void *ud) {
	struct skynet_message msg;
	/*drop all message*/
	while(!skynet_mq_pop(q, &msg)) {
		drop_func(&msg, ud);
	}
	/*release queue*/
	_release(q);
}

/**
  * @brief try to release the queue
  * @param[in] q handle of the module queue
  * @param[in] drop_func function used to drop the messsage
  * @param[in] ud module id for the queue
  *
  */
void 
skynet_mq_release(struct message_queue *q, message_drop drop_func, void *ud) {
	LOCK(q)
	/*if release flag marked*/
	if (q->release) { 
		UNLOCK(q)
		/*destory the queue*/
		_drop_queue(q, drop_func, ud);
	} else {
	        /*push it into global queue again*/
		skynet_globalmq_push(q);
		UNLOCK(q)
	}
}
