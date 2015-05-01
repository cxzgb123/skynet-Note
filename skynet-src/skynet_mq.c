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
	int cap;                             /*¶ÓÁĞ´óĞ¡*/
	int head;                           /*¶ÓÍ·Ë÷Òı*/
	int tail;                             /*¶ÓÎ²Ë÷Òı*/
	int lock;                            /* ¶ÓÁĞËø*/
	int release;
	int in_global;                     /*±êÖ¾¸Ã¶ÓÁĞÊÇ·ñÔÚÈ«¾ÖÏûÏ¢¶ÓÁĞÖĞ*/
	int overload;
	int overload_threshold;       /*¶ÓÁĞÂúÊ±±ê×¢ÎªMQ_OVERLOAD,¿ÕÊ±»Ö¸´*/
	struct skynet_message *queue;   /*¶ÓÁĞÖ¸Õë*/
	struct message_queue *next;     /*ÓÃÓÚÁ´½ÓÈëÈ«¾ÖÏûÏ¢¹ÜÀí½á¹¹*/
};

/**
  * @brief È«¾ÖÏûÏ¢¹ÜÀí½á¹¹
  * @note ¶ÓÁĞÔË×÷£¬Á´±í¹ÜÀí  
  *
  */
struct global_queue {
	struct message_queue *head;     /*¶ÓÍ·*/
	struct message_queue *tail;       /*¶ÓÎ²*/
	int lock;                                 /*Ëø*/
};

static struct global_queue *Q = NULL; /*È«¾Ö¹ÜÀí½á¹¹*/

#define LOCK(q) while (__sync_lock_test_and_set(&(q)->lock,1)) {}    /*Ô­×Ó»ñËø*/
#define UNLOCK(q) __sync_lock_release(&(q)->lock);                     /*Ô­×Ó½âËø*/       

/**
 * @brief È«¾ÖÏûÏ¢¶ÓÁĞÖĞ¹ÒÈëÒ»¸ö
 *    Ä£¿éµÄÏûÏ¢¶ÓÁĞ
 * @param[in|out] queue ´ı¹ÒÈëµÄÏûÏ¢¶ÓÁĞ
 *
 */
void 
skynet_globalmq_push(struct message_queue * queue) {
	struct global_queue *q= Q;
	/*»ñÈ¡Ëø*/
	LOCK(q)
	/*½«ÆäÁ´½ÓÈëÈ«¾Ö¶ÓÁĞÎ²²¿*/
	assert(queue->next == NULL);
	if(q->tail) {
		q->tail->next = queue;
		q->tail = queue;
	} else {
		q->head = q->tail = queue;
	}
	/*ÊÍ·ÅËø*/
	UNLOCK(q)
}

/**
  * @brief ´ÓÈ«¾ÖÏûÏ¢¹ÜÀíÖĞµ¯³öÒ»¸ö
  *   ÊôÓÚÄ³¸öÄ£¿éµÄÏûÏ¢¶ÓÁĞ
  * @return ÏûÏ¢¶ÓÁĞ
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
 * @brief ´´½¨²¢³õÊ¼»¯Ä£¿éµÄÈ«¾Ö¶ÓÁĞ
 * @param[handle] hanedle ¸ÃÄ£¿éµÄË÷Òı
 * return ·µ»Ø¸ÃÄ£¿éµÄ¶ÓÁĞ
 */
struct message_queue * 
skynet_mq_create(uint32_t handle) {
	struct message_queue *q = skynet_malloc(sizeof(*q));
	q->handle = handle;			    /*¶ÓÁĞ¶ÔÓ¦µÄÄ£¿éµÄidºÅ*/
	q->cap = DEFAULT_QUEUE_SIZE;     /*Ã¿¸öÄ£¿éµÄÑ­»·¶ÓÁĞÄ¬ÈÏ´óĞ¡*/
	q->head = 0;					    /*Ñ­»·¶ÓÁĞÍ·Ë÷Òı³õÊ¼»¯*/
	q->tail = 0;					    /*Ñ­»·¶ÓÁĞÎ²°Í³õÊ¼»¯*/	
	q->lock = 0;					    /*Ñ­»·¶ÓÁĞËø*/
	// When the queue is create (always between service create and service init) ,
	// set in_global flag to avoid push it to global queue .
	// If the service init success, skynet_context_new will call skynet_mq_force_push to push it to global queue.
	q->in_global = MQ_IN_GLOBAL;   /*¸Ã¶ÓÁĞÔÚÈ«¾Ö¶ÓÁĞÖĞ*/
	q->release = 0;			        /*±êÖ¾¶ÓÁĞ¿Õ¼äÊÇ·ñµÈ´ıÊÍ·Å*/
	q->overload = 0;				 /*¶ÓÁĞ¸ººÉ*/	
	q->overload_threshold = MQ_OVERLOAD;   /*¶ÓÁĞÔØºÉ¸üĞÂµÄÄ¬ÈÏ·§Öµ*/
	q->queue = skynet_malloc(sizeof(struct skynet_message) * q->cap); /*ÉêÇë¶ÓÁĞ¿Õ¼ä*/
	q->next = NULL;						 /*Á¬½ÓÏÂÒ»¸ö¶ÓÁĞ*/

	return q;								/*·µ»ØÄ£¿é¶ÓÁĞ*/
}

/**
  * @brief ÊÍ·Å¸ÃÄ£¿éµÄÑ­»·¶ÓÁĞ
  * @param[in|out] q Ö¸ÏòµÈ´ıÊÍ·ÅµÄÑ­»·¶ÓÁĞ 
  */
static void 
_release(struct message_queue *q) {
	assert(q->next == NULL);
	skynet_free(q->queue);					/*ÊÍ·ÅÑ­»·Êı×é*/
	skynet_free(q);						/*ÊÍ·Å¶ÓÁĞ¹ÜÀí½á¹¹*/
}

/**
 * @brief ·µ»Ø¸ÃÑ­»·¶ÓÁĞËùÊôµÄÄ£¿é
 * @return Ä£¿éË÷Òı
 */
uint32_t 
skynet_mq_handle(struct message_queue *q) {
	/*·µ»Ø¸ÃÑ­»·¶ÓÁĞ¶ÔÓ¦Ä£¿éµÄID*/
	return q->handle;
}

/**
  * @brief ·µ»ØÄ£¿éÑ­»·¶ÓÁĞµÄ³¤¶È
  * @param[in|out] q Ä£¿éµÄ¶ÓÁĞÖ¸Õë
  * @return Ä£¿éµÄ¶ÓÁĞ³¤¶È
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
  * @brief µ¯³ö¸ººÉÖµ, ²¢½«Ô­À´µÄÖµÖÃÎª 1
  * @param[in] q Ö¸Ïò´ı´¦ÀíµÄÑ­»·¶ÓÁĞ
  * @return ÔØºÉ´óĞ¡
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
  * @brief ÏûÏ¢³ö¶Ó
  * @param[out|out] message_queue Ä£¿éÏûÏ¢¹ÜÀí¶ÓÁĞ
  * @param[out] message ÓÃÓÚ´æ·Å³ö¶ÓµÄÏûÏ¢
  *
  */
int
skynet_mq_pop(struct message_queue *q, struct skynet_message *message) {
	int ret = 1;
	/*»ñÈ¡Ëø*/
	LOCK(q)              
      /*ÅĞ¶Ï¶ÓÁĞÖĞÊÇ·ñ»¹ÓĞÔªËØ*/            
	if (q->head != q->tail) {
		/*µ±¶ÓÁĞÖĞÓĞÊı¾İÊ±, ÏÈÈ¡³öÒ»¸ö*/
		*message = q->queue[q->head++];
		ret = 0;
		int head = q->head;
		int tail = q->tail;
		int cap = q->cap;

	      /*µ±·¢ÏÖÍ·²¿µ½´ïÊı×éÎ²°ÍÊ±ĞèÖØÖÃµ½Í·²¿*/
		if (head >= cap) {
			q->head = head = 0;
		}
		/*¼ÆËãÓĞĞ§ÔØºÉ*/
		int length = tail - head;
		if (length < 0) {
			length += cap;
		}
		/**ÖØÖÃÔØºÉãĞÖµºÍÔØºÉ¼ÇÂ¼*/
		/*TODO ÕâÀï¿ÉÒÔÓÅ»¯*/
		while (length > q->overload_threshold) {
			q->overload = length;		/*ÉèÖÃÔØºÉ*/
			q->overload_threshold *= 2;	/*ÖØÖÃÔØºÉãĞÖµ£¬±¶Ôö*/
		}
	} else {
		// reset overload_threshold when queue is empty
		q->overload_threshold = MQ_OVERLOAD;
	}
      /*µ±·¢ÏÖÄ£¿éµÄ¶ÓÁĞ¿ÕÊ±£¬ÉèÖÃ¶ÓÁĞ±êÖ¾
          Îª²»ÔÚÈ«¾Ö¶ÓÁĞÖĞ*/
	if (ret) {
		q->in_global = 0;
	}
	/*ÊÍ·ÅËø*/
	UNLOCK(q) 

	return ret;
}

/**
  * @brief Ä£¿é¶ÓÁĞÀ©ÕÅ
  * @param[in|out] q Ö¸ÏòÄ£¿é¶ÓÁĞ
  *
  */
static void
expand_queue(struct message_queue *q) {
	/*ÉêÇë±¶Ôö¿Õ¼ä*/
	struct skynet_message *new_queue = skynet_malloc(sizeof(struct skynet_message) * q->cap * 2);
	int i;
	/*TODO bug ÕâÀïÊÇ·ñ¿ÉÒÔÓÅ»¯*/
	/*¿½±´¿Õ¼ä*/
	for (i=0;i<q->cap;i++) {
		new_queue[i] = q->queue[(q->head + i) % q->cap];
	}
	/*ÉèÖÃÊôĞÔ*/
	q->head = 0;
	q->tail = q->cap;
	q->cap *= 2;
	/*ÊÍ·Å¾É¿Õ¼ä*/
	skynet_free(q->queue);
	/*ÖØÉèÖÃ¿Õ¼äÖ¸Õë*/
	q->queue = new_queue;
}
/**
  * @brief ÏòÄ£¿é¶ÓÁĞÖĞÍÆËÍÒ»¸ömessage
  * @param[in|out] Ö¸ÏòÄ£¿é¶ÓÁĞµÄÖ¸Õë
  * @param[in] message ´ıÍÆÈëµÄÏûÏ¢
  *
  */
void 
skynet_mq_push(struct message_queue *q, struct skynet_message *message) {
	assert(message);
	/*¿ªËø*/
	LOCK(q)
      /*ÍÆÈëÏûÏ¢*/
	q->queue[q->tail] = *message;
	/*³¬¹ıÊı×éÎ²°ÍÔòÖØÖÃtailµÄ×ø±êÎª0*/
	if (++ q->tail >= q->cap) {
		q->tail = 0;
	}

	/*·¢ÏÖ¶ÓÁĞÂúÁË£¬Ôò½«¶ÓÁĞ±¶Ôö*/
	if (q->head == q->tail) {
		expand_queue(q);
	}
      /*Ä£¿é¶ÓÁĞÖĞÌî³äÁËĞÂÊı¾İµ«
           ·¢ÏÖ¸Ã¶ÓÁĞÃ»¹ÒÈëÈ«¾Ö¶ÓÁĞĞèÒª½«Æä¹ÒÈëÈ«¾Ö¶ÓÁĞ*/
	if (q->in_global == 0) {
		/*ÉèÖÃ±êÖ¾¹ÒÈëÈ«¾Ö¶ÓÁĞ*/
		q->in_global = MQ_IN_GLOBAL;
		skynet_globalmq_push(q);
	}
	/*½âËø*/
	UNLOCK(q)
}

/**
 * @brief ³õÊ¼»¯È«¾Ö¹ÜÀí¶ÓÁĞ
 *
 */
void 
skynet_mq_init() {
	struct global_queue *q = skynet_malloc(sizeof(*q));
	memset(q,0,sizeof(*q));
	Q=q;
}
/**
  * @brief ½«Ò»¸öÄ£¿é¶ÓÁĞ±êÖ¾Îª´ıÊÍ·Å
  * @param[in] q ´øÊÍ·ÅµÄÄ£¿éÏûÏ¢¶ÓÁĞ
  * 
  */
void 
skynet_mq_mark_release(struct message_queue *q) {
	/*»ñÈ¡Ëø*/
	LOCK(q)
	/**ÊÍ·Å±êÖ¾Î´±»ÉèÖÃÈ´±»ÊÍ·ÅÈÏÎªÊÇ´íÎóµÄ*/
	assert(q->release == 0);

	/*TODO Õâ¸öÉèÖÃ¸Ğ¾õÃ»±ØÒª*/
	/*ÉèÖÃÊÍ·Å±êÖ¾*/
	q->release = 1;
	if (q->in_global != MQ_IN_GLOBAL) {
		skynet_globalmq_push(q);
	}
	/*ÊÍ·ÅËø*/
	UNLOCK(q)
}

/**
  * @brief Çå¿ÕÒ»¸öÄ£¿é¶ÓÁ,²¢½«ÆäÉ¾³ıĞ
  * @param[in] q Ö¸ÏòÄ£¿é¶ÓÁĞ
  * @param[in] drop_func ÓÃÓÚÇå¿Õ¶ÓÁĞµÄº¯Ê
  * @param[in] ud ÇåÀíÊ±ĞèÒªµÄÆäËû²ÎÊı
  */
static void
_drop_queue(struct message_queue *q, message_drop drop_func, void *ud) {
	struct skynet_message msg;
	/*Çå¿Õ¶ÓÁĞÀïÃæµÄÊı¾İ*/
	while(!skynet_mq_pop(q, &msg)) {
		/*µ÷ÓÃÇåÀíº¯ÊıÇåÀí*/
		drop_func(&msg, ud);
	}
	/*ÊÍ·Å¸ÃÏûÏ¢¶ÓÁĞ*/
	_release(q);
}

/**
  * @brief ³¢ÊÔÊÍ·Å¶ÓÁĞ
  * @param[in] q Ä£¿é¶ÓÁĞÖ¸Õë
  * @param[in] drop_func ÏûÏ¢¶ªÆúº¯Êı
  * @param[in] ud ¸ÃÏûÏ¢¶ÔÓ¦µÄÄ£¿éIDµÄÖ¸Õë
  *
  */
void 
skynet_mq_release(struct message_queue *q, message_drop drop_func, void *ud) {
	LOCK(q)
	
	if (q->release) { 
		/*¸ÃÄ£¿é¶ÓÁĞ´ıÇåÀí*/
		UNLOCK(q)
		/*Çå¿Õ¸Ã¶ÓÁĞ*/
		_drop_queue(q, drop_func, ud);
	} else {
		/*Ã»ÓĞ±ê¼ÇÎª´ıÊÍ·Å£¬½«¸Ã¶ÓÁĞ·ÅÈëÈ«¾Ö¶ÓÁĞ*/
		skynet_globalmq_push(q);
		UNLOCK(q)
	}
}
