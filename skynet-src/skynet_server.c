#include "skynet.h"

#include "skynet_server.h"
#include "skynet_module.h"
#include "skynet_handle.h"
#include "skynet_mq.h"
#include "skynet_timer.h"
#include "skynet_harbor.h"
#include "skynet_env.h"
#include "skynet_monitor.h"
#include "skynet_imp.h"
#include "skynet_log.h"

#include <pthread.h>

#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

#ifdef CALLING_CHECK

#define CHECKCALLING_BEGIN(ctx) assert(__sync_lock_test_and_set(&ctx->calling,1) == 0);
#define CHECKCALLING_END(ctx) __sync_lock_release(&ctx->calling);
#define CHECKCALLING_INIT(ctx) ctx->calling = 0;
#define CHECKCALLING_DECL int calling;

#else

#define CHECKCALLING_BEGIN(ctx)
#define CHECKCALLING_END(ctx)
#define CHECKCALLING_INIT(ctx)
#define CHECKCALLING_DECL

#endif

/**
 *  @brief manager for module
 *
 */
struct skynet_context {
	void * instance;                 /*hold the structure init by module*/
	struct skynet_module * mod;      /*handle of the module*/
	void * cb_ud;
	skynet_cb cb;
	struct message_queue *queue;    /*message queue for this module*/
	FILE * logfile;                 /*logfile*/
	char result[32];
	uint32_t handle;                /*module id for this queue*/  
	int session_id;                 /*curr session id*/
	int ref;                        /*ref for this module*/
	bool init;                      /*flag if the handle init finished*/
	bool endless;

	CHECKCALLING_DECL
};

struct skynet_node {
	int total;                          
	int init;
	uint32_t monitor_exit;
	pthread_key_t handle_key;       /*shared data in the thread*/   
};

static struct skynet_node G_NODE;

/**
 * @brief tot 
 */
int 
skynet_context_total() {
	return G_NODE.total;
}

/**
 * @brief atom add tot 
 */
static void
context_inc() {
	__sync_fetch_and_add(&G_NODE.total,1);
}

/**
 * @brief atod dec tot 
 */
static void
context_dec() {
	__sync_fetch_and_sub(&G_NODE.total,1);
}

/**
 * @brief get id of this thread
 * @@note main thread id before init finished
 *
 */
uint32_t 
skynet_current_handle(void) {
	if (G_NODE.init) {
		void * handle = pthread_getspecific(G_NODE.handle_key);
		return (uint32_t)(uintptr_t)handle;
	} else {
		uintptr_t v = (uint32_t)(-THREAD_MAIN);
		return v;
	}
}

/**
 * @brief change id to hex val
 *
 */
static void
id_to_hex(char * str, uint32_t id) {
	int i;
	static char hex[16] = { '0','1','2','3','4','5','6','7','8','9','A','B','C','D','E','F' };
	str[0] = ':';
	for (i=0;i<8;i++) {
		str[i+1] = hex[(id >> ((7-i) * 4))&0xf];
	}
	str[9] = '\0';
}

/**
 * @brief hold the module id when drop message
 */
struct drop_t {
	uint32_t handle;    /*id of the module*/
};

/**
 * @brief drop msg
 * @param[in] msg msg wait to drop
 * @param[in] ud the module id this msg belong to
 */
static void
drop_message(struct skynet_message *msg, void *ud) {
	struct drop_t *d = ud;
	skynet_free(msg->data);
	uint32_t source = d->handle;
	assert(source);
	// report error to the message source
	skynet_send(NULL, source, msg->source, PTYPE_ERROR, 0, NULL, 0);
}

/* *
  * @brief create handle for module
  * @param[in] name module name
  * @param[in] param other param
  * @return handle for module
  */
struct skynet_context * 
skynet_context_new(const char * name, const char *param) {
	/*load module*/
	struct skynet_module * mod = skynet_module_query(name);

	if (mod == NULL)
		return NULL;
	/*init the module*/
	void *inst = skynet_module_instance_create(mod);
	if (inst == NULL)
		return NULL;

	struct skynet_context * ctx = skynet_malloc(sizeof(*ctx));

	CHECKCALLING_INIT(ctx)

        /*warp the base handle as context*/
	ctx->mod = mod;				/*manager of module*/
	ctx->instance = inst;			/*data from init*/
	ctx->ref = 2;			        /*ref as c*/
	ctx->cb = NULL;				/*callback for recive msg*/
	ctx->cb_ud = NULL;
	ctx->session_id = 0;
	ctx->logfile = NULL;                    /*logfile*/

	ctx->init = false;
	ctx->endless = false;
	// Should set to 0 first to avoid skynet_handle_retireall get an uninitialized handle
	ctx->handle = 0;	
	ctx->handle = skynet_handle_register(ctx);
	struct message_queue * queue = ctx->queue = skynet_mq_create(ctx->handle);
	// init function maybe use ctx->handle, so it must init at last
	context_inc();

	CHECKCALLING_BEGIN(ctx)
	/*init with the inst*/
	int r = skynet_module_instance_init(mod, inst, ctx, param);
	CHECKCALLING_END(ctx)
	if (r == 0) {
		struct skynet_context * ret = skynet_context_release(ctx);
		if (ret) {
			ctx->init = true;
		}
		/*push the queue of this module into global queue in default*/
		skynet_globalmq_push(queue);
		if (ret) {
			skynet_error(ret, "LAUNCH %s %s", name, param ? param : "");
		}
		return ret;
	} else {
		skynet_error(ctx, "FAILED launch %s", name);
		uint32_t handle = ctx->handle;
		skynet_context_release(ctx);
		skynet_handle_retire(handle);
		struct drop_t d = { handle };
		skynet_mq_release(queue, drop_message, &d);
		return NULL;
	}
}

/**
 * @brief alloc a new session id for this socket
 * @param[in] ctx handle for the module
 * return session id 
 */
int
skynet_context_newsession(struct skynet_context *ctx) {
	// session always be a positive number
	int session = ++ctx->session_id;
	if (session <= 0) {
		ctx->session_id = 1;
		return 1;
	}
	return session;
}

/**
 * @brief add ref of the context
 * @param[in] ctx handle for the module
 *
 */
void 
skynet_context_grab(struct skynet_context *ctx) {
	__sync_add_and_fetch(&ctx->ref,1);
}

/**
 *
 *
 */
void
skynet_context_reserve(struct skynet_context *ctx) {
	skynet_context_grab(ctx);
	// don't count the context reserved, because skynet abort (the worker threads terminate) only when the total context is 0 .
	// the reserved context will be release at last.
	context_dec();
}

/**
  * @brief release the handle for module
  * @param[in|out] ָ���ɾ����ģ��
  */
static void 
delete_context(struct skynet_context *ctx) {
	/*close the lofgile for this module*/
	if (ctx->logfile) {
		fclose(ctx->logfile);
	}
	/*release inst*/
	skynet_module_instance_release(ctx->mod, ctx->instance);
	/*mark the queue of the module as release*/
	skynet_mq_mark_release(ctx->queue);
	/*free handle but the queue left*/
	skynet_free(ctx);
	context_dec();
}

/**
 * @brief sub ref of the handle for module
 * @param[in] ctx handle for module
 * return (release ?  null : handle)
 */
struct skynet_context * 
skynet_context_release(struct skynet_context *ctx) {
	/*ref sub*/
	if (__sync_sub_and_fetch(&ctx->ref,1) == 0) {
		delete_context(ctx);
		return NULL;
	}
	return ctx;
}

/**
 * @brief push the queue of the module into global queue
 * @param[in] handle id of the module
 *
 */
int
skynet_context_push(uint32_t handle, struct skynet_message *message) {
	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL) {
		return -1;
	}
	skynet_mq_push(ctx->queue, message);
	skynet_context_release(ctx);

	return 0;
}

/**
 * @brief mark the module exit if context marked with endless
 * @param[in] handle of the module
 */
void 
skynet_context_endless(uint32_t handle) {
	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL) {
		return;
	}
	ctx->endless = true;
	skynet_context_release(ctx);
}

/**
 * @brief check if the handle is remote
 * @param[in] ctx handle of the module
 * @param[in] handle id of the module
 * @param[in] harbor used to get harbor id
 *
 */
int 
skynet_isremote(struct skynet_context * ctx, uint32_t handle, int * harbor) {
	int ret = skynet_harbor_message_isremote(handle);
	if (harbor) {
		*harbor = (int)(handle >> HANDLE_REMOTE_SHIFT);
	}
	return ret;
}

/**
 * @brief deal with the msg to this module by callback register from the module
 * @param[in] ctx handle of the module
 * @param[in] msg msg to the module
 *
 */
static void
dispatch_message(struct skynet_context *ctx, struct skynet_message *msg) {
	assert(ctx->init);
	CHECKCALLING_BEGIN(ctx)
	pthread_setspecific(G_NODE.handle_key, (void *)(uintptr_t)(ctx->handle));
	int type = msg->sz >> HANDLE_REMOTE_SHIFT;
	size_t sz = msg->sz & HANDLE_MASK;
	if (ctx->logfile) {
		skynet_log_output(ctx->logfile, msg->source, type, msg->session, msg->data, sz);
	}
	if (!ctx->cb(ctx, ctx->cb_ud, type, msg->session, msg->source, msg->data, sz)) {
		skynet_free(msg->data);
	} 
	CHECKCALLING_END(ctx)
}

/**
 * @brief try to pop one msg from the global queue and deal with it
 * @param[in] ctx handle of the module
 */
void 
skynet_context_dispatchall(struct skynet_context * ctx) {
	// for skynet_error
	struct skynet_message msg;
	struct message_queue *q = ctx->queue;
	/*pop one msg*/
	while (!skynet_mq_pop(q,&msg)) {
	        /*deal with it*/
		dispatch_message(ctx, &msg);
	}
}

/**
 * @brief try to pop msg queue from global queue, them pop msg from the message queue and deal with it
 * @param[in] s module manager
 * @param[in] q message queue for module
 * @param[in] weight used to deal with the num of msg pop out every loop
 *
 */
struct message_queue * 
skynet_context_message_dispatch(struct skynet_monitor *sm, struct message_queue *q, int weight) {
	if (q == NULL) {
		/*try to pop one msg from the queue of the module*/
		q = skynet_globalmq_pop();
		/*pop message fail*/
		if (q==NULL)
			return NULL;
	}

	 /*find id of the module the queue belong to*/
	uint32_t handle = skynet_mq_handle(q);
	    
	/*get the hdnle*/
	struct skynet_context * ctx = skynet_handle_grab(handle);

	/*handle release because module handle release with queue left*/  
	if (ctx == NULL) {
	        /*drop the msg directly*/
		struct drop_t d = { handle };
		skynet_mq_release(q, drop_message, &d);
		/*get next message queue*/
		return skynet_globalmq_pop();
	}

	int i,n=1;
	struct skynet_message msg;
	/*tot nums of msg pop out by weight and payload*/
	for (i=0;i<n;i++) {
		/*try to pop one msg*/
		if (skynet_mq_pop(q,&msg)) {
		         /*used out, so return*/
			skynet_context_release(ctx);
			return skynet_globalmq_pop();
		} else if (i==0 && weight >= 0) {
			/*more msg left in it when pop msg first time*/

		        /*get num of msg left in it*/
			n = skynet_mq_length(q);

                        /*update the msg nums need to pop out*/
			n >>= weight;
		}
		/*check if the queue is overload*/
		int overload = skynet_mq_overload(q);

		/*overload*/
		if (overload) {
			skynet_error(ctx, "May overload, message queue length = %d", overload);
		}

		/*update monitor, as a watch dog for module*/
		/*notice, if the callback of the module in work, dst in trigger is not zero!!!!!, used for monitor*/
		skynet_monitor_trigger(sm, msg.source , handle);

		if (ctx->cb == NULL) {
			skynet_free(msg.data);
		} else {
		        /*deal with the msg*/
			dispatch_message(ctx, &msg);
		}
		/*update monitor, as a watch dog for module*/
		skynet_monitor_trigger(sm, 0,0);
	}

	assert(q == ctx->queue);
	/*try to pop one message queue in the end*/
	struct message_queue *nq = skynet_globalmq_pop();
	if (nq) {
		// If global mq is not empty , push q back, and return next queue (nq)
		// Else (global mq is empty or block, don't push q back, and return q again (for next dispatch)
			
		skynet_globalmq_push(q);
		q = nq;
	} 
	skynet_context_release(ctx);

	return q;
}

/**
 * @brief copy string from addr to name
 * @param[out] name storage as dst
 * @param[in]  addr string from
 */
static void
copy_name(char name[GLOBALNAME_LENGTH], const char * addr) {
	int i;
	for (i=0;i<GLOBALNAME_LENGTH && addr[i];i++) {
		name[i] = addr[i];
	}
	for (;i<GLOBALNAME_LENGTH;i++) {
		name[i] = '\0';
	}
}

/**
 * @brief try to find the module by name
 * @param[in] context handle of the module
 * @param[in] name module name
 *
 */
uint32_t 
skynet_queryname(struct skynet_context * context, const char * name) {
	switch(name[0]) {
	case ':':
	        /* ': module id' support*/
		return strtoul(name+1,NULL,16);
	case '.':
                /* '. module name' support*/
		return skynet_handle_findname(name + 1);
	}
	skynet_error(context, "Don't support query global name %s",name);
	return 0;
}

/**
 * @brief the module exit
 * @param[in] context handle of hte module
 * @param[in] handle id of the module
 *
 */
static void
handle_exit(struct skynet_context * context, uint32_t handle) {
	if (handle == 0) {
		handle = context->handle;
		skynet_error(context, "KILL self");
	} else {
		skynet_error(context, "KILL :%0x", handle);
	}
	if (G_NODE.monitor_exit) {
	        /*report*/
		skynet_send(context,  handle, G_NODE.monitor_exit, PTYPE_CLIENT, 0, NULL, 0);
	}
	/*unregister*/
	skynet_handle_retire(handle);
}

// skynet command
/**
 * @brief command 
 *
 */
struct command_func {
	const char *name;                                                               /*command name*/
	const char * (*func)(struct skynet_context * context, const char * param);      /*callback for this command*/
};

/**
 * @brief register timer for session
 * @param[in] context module handle
 * @param[in] param hold the timer
 *
 */
static const char *
cmd_timeout(struct skynet_context * context, const char * param) {
	char * session_ptr = NULL;
	/*get timer*/
	int ti = strtol(param, &session_ptr, 10);
	/*alloc new session for this module*/
	int session = skynet_context_newsession(context);
	/*register time evnet*/
	skynet_timeout(context->handle, ti, session);
	sprintf(context->result, "%d", session);
	return context->result;
}

/**
 * @brief try to register 'name string = handle' into module manager
 * @param[in] context handle of the module
 * @param[in] param param wait to register
 *
 */
static const char *
cmd_reg(struct skynet_context * context, const char * param) {
	if (param == NULL || param[0] == '\0') {
	        /*put into module id as %x into module handle*/
		sprintf(context->result, ":%x", context->handle);
		return context->result;
	} else if (param[0] == '.') {
	        /*string, so insert  'name string = handle' pair into module name manager*/
		return skynet_handle_namehandle(context->handle, param + 1);
	} else {
		skynet_error(context, "Can't register global name %s in C", param);
		return NULL;
	}
}

static const char *
cmd_query(struct skynet_context * context, const char * param) {
	if (param[0] == '.') {
		uint32_t handle = skynet_handle_findname(param+1);
		if (handle) {
			sprintf(context->result, ":%x", handle);
			return context->result;
		}
	}
	return NULL;
}

static const char *
cmd_name(struct skynet_context * context, const char * param) {
	int size = strlen(param);
	char name[size+1];
	char handle[size+1];
	sscanf(param,"%s %s",name,handle);
	if (handle[0] != ':') {
		return NULL;
	}
	uint32_t handle_id = strtoul(handle+1, NULL, 16);
	if (handle_id == 0) {
		return NULL;
	}
	if (name[0] == '.') {
		return skynet_handle_namehandle(handle_id, name + 1);
	} else {
		skynet_error(context, "Can't set global name %s in C", name);
	}
	return NULL;
}

static const char *
cmd_now(struct skynet_context * context, const char * param) {
	uint32_t ti = skynet_gettime();
	sprintf(context->result,"%u",ti);
	return context->result;
}

static const char *
cmd_exit(struct skynet_context * context, const char * param) {
	handle_exit(context, 0);
	return NULL;
}

static uint32_t
tohandle(struct skynet_context * context, const char * param) {
	uint32_t handle = 0;
	if (param[0] == ':') {
		handle = strtoul(param+1, NULL, 16);
	} else if (param[0] == '.') {
		handle = skynet_handle_findname(param+1);
	} else {
		skynet_error(context, "Can't convert %s to handle",param);
	}

	return handle;
}

static const char *
cmd_kill(struct skynet_context * context, const char * param) {
	uint32_t handle = tohandle(context, param);
	if (handle) {
		handle_exit(context, handle);
	}
	return NULL;
}

static const char *
cmd_launch(struct skynet_context * context, const char * param) {
	size_t sz = strlen(param);
	char tmp[sz+1];
	strcpy(tmp,param);
	char * args = tmp;
	char * mod = strsep(&args, " \t\r\n");
	args = strsep(&args, "\r\n");
	struct skynet_context * inst = skynet_context_new(mod,args);
	if (inst == NULL) {
		return NULL;
	} else {
		id_to_hex(context->result, inst->handle);
		return context->result;
	}
}

static const char *
cmd_getenv(struct skynet_context * context, const char * param) {
	return skynet_getenv(param);
}

static const char *
cmd_setenv(struct skynet_context * context, const char * param) {
	size_t sz = strlen(param);
	char key[sz+1];
	int i;
	for (i=0;param[i] != ' ' && param[i];i++) {
		key[i] = param[i];
	}
	if (param[i] == '\0')
		return NULL;

	key[i] = '\0';
	param += i+1;
	
	skynet_setenv(key,param);
	return NULL;
}

static const char *
cmd_starttime(struct skynet_context * context, const char * param) {
	uint32_t sec = skynet_gettime_fixsec();
	sprintf(context->result,"%u",sec);
	return context->result;
}

static const char *
cmd_endless(struct skynet_context * context, const char * param) {
	if (context->endless) {
		strcpy(context->result, "1");
		context->endless = false;
		return context->result;
	}
	return NULL;
}

static const char *
cmd_abort(struct skynet_context * context, const char * param) {
	skynet_handle_retireall();
	return NULL;
}

static const char *
cmd_monitor(struct skynet_context * context, const char * param) {
	uint32_t handle=0;
	if (param == NULL || param[0] == '\0') {
		if (G_NODE.monitor_exit) {
			// return current monitor serivce
			sprintf(context->result, ":%x", G_NODE.monitor_exit);
			return context->result;
		}
		return NULL;
	} else {
		handle = tohandle(context, param);
	}
	G_NODE.monitor_exit = handle;
	return NULL;
}

static const char *
cmd_mqlen(struct skynet_context * context, const char * param) {
	int len = skynet_mq_length(context->queue);
	sprintf(context->result, "%d", len);
	return context->result;
}

static const char *
cmd_logon(struct skynet_context * context, const char * param) {
	uint32_t handle = tohandle(context, param);
	if (handle == 0)
		return NULL;
	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL)
		return NULL;
	FILE *f = NULL;
	FILE * lastf = ctx->logfile;
	if (lastf == NULL) {
		f = skynet_log_open(context, handle);
		if (f) {
			if (!__sync_bool_compare_and_swap(&ctx->logfile, NULL, f)) {
				// logfile opens in other thread, close this one.
				fclose(f);
			}
		}
	}
	skynet_context_release(ctx);
	return NULL;
}

static const char *
cmd_logoff(struct skynet_context * context, const char * param) {
	uint32_t handle = tohandle(context, param);
	if (handle == 0)
		return NULL;
	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL)
		return NULL;
	FILE * f = ctx->logfile;
	if (f) {
		// logfile may close in other thread
		if (__sync_bool_compare_and_swap(&ctx->logfile, f, NULL)) {
			skynet_log_close(context, f, handle);
		}
	}
	skynet_context_release(ctx);
	return NULL;
}

static const char *
cmd_signal(struct skynet_context * context, const char * param) {
	uint32_t handle = tohandle(context, param);
	if (handle == 0)
		return NULL;
	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL)
		return NULL;
	param = strchr(param, ' ');
	int sig = 0;
	if (param) {
		sig = strtol(param, NULL, 0);
	}
	// NOTICE: the signal function should be thread safe.
	skynet_module_instance_signal(ctx->mod, ctx->instance, sig);

	skynet_context_release(ctx);
	return NULL;
}

static struct command_func cmd_funcs[] = {
	{ "TIMEOUT", cmd_timeout },
	{ "REG", cmd_reg },
	{ "QUERY", cmd_query },
	{ "NAME", cmd_name },
	{ "NOW", cmd_now },
	{ "EXIT", cmd_exit },
	{ "KILL", cmd_kill },
	{ "LAUNCH", cmd_launch },
	{ "GETENV", cmd_getenv },
	{ "SETENV", cmd_setenv },
	{ "STARTTIME", cmd_starttime },
	{ "ENDLESS", cmd_endless },
	{ "ABORT", cmd_abort },
	{ "MONITOR", cmd_monitor },
	{ "MQLEN", cmd_mqlen },
	{ "LOGON", cmd_logon },
	{ "LOGOFF", cmd_logoff },
	{ "SIGNAL", cmd_signal },
	{ NULL, NULL },
};

/**
 * @brief try to call cmd 
 * @param[in] context handle of the module
 * @param[in] cmd cmd to call
 * @param[in] param param for the cmd's callback
 *
 */
const char * 
skynet_command(struct skynet_context * context, const char * cmd , const char * param) {
	struct command_func * method = &cmd_funcs[0];
	/*lookup cmd in O(n)*/
	while(method->name) {
		if (strcmp(cmd, method->name) == 0) {
			return method->func(context, param);
		}
		++method;
	}

	return NULL;
}

/**
 * @brief try to update size, get session, alloc payload 
 * @param[in] contex module handle
 * @param[in] type msg type
 * @param[in] session session id of this msg
 * @param[out] data ptr payload
 * @param[in] sz size of the payload
 */
static void
_filter_args(struct skynet_context * context, int type, int *session, void ** data, size_t * sz) {
	/*check if need copy*/
        int needcopy = !(type & PTYPE_TAG_DONTCOPY);
        /*check if need copy payload*/
	int allocsession = type & PTYPE_TAG_ALLOCSESSION;
	
	type &= 0xff;

	if (allocsession) {
	        /*alloc a new session not used*/
		assert(*session == 0);
		*session = skynet_context_newsession(context);
	}

	if (needcopy && *data) {
	        /*try to copy payload*/
		char * msg = skynet_malloc(*sz+1);
		memcpy(msg, *data, *sz);
		msg[*sz] = '\0';
		*data = msg;
	}

	*sz |= type << HANDLE_REMOTE_SHIFT;
}

/**
 * @brief send msg to other module by destination
 * @param[in] contex module handle
 * @param[in] source msg from
 * @param[in] destination   dst module id
 * @param[in] type msg type
 * @param[in] session session id of this msg
 * @param[out] data ptr payload
 * @param[in] sz size of the payload
 */
int
skynet_send(struct skynet_context * context, uint32_t source, uint32_t destination , int type, int session, void * data, size_t sz) {
	if ((sz & HANDLE_MASK) != sz) {
		skynet_error(context, "The message to %x is too large (sz = %lu)", destination, sz);
		skynet_free(data);
		return -1;
	}
        /*brief try to update size, get session, alloc payload*/
	_filter_args(context, type, &session, (void **)&data, &sz);

	if (source == 0) {
	        /*mark source as the module id*/
		source = context->handle;
	}

	if (destination == 0) {
		return session;
	}
	if (skynet_harbor_message_isremote(destination)) {
	    /*send to the remote when dst is remote*/
	        struct remote_message * rmsg = skynet_malloc(sizeof(*rmsg));
		rmsg->destination.handle = destination;
		rmsg->message = data;
		rmsg->sz = sz;
		skynet_harbor_send(rmsg, source, session);
	} else {
	    /*msg send to local module */
		struct skynet_message smsg;
		smsg.source = source;
		smsg.session = session;
		smsg.data = data;
		smsg.sz = sz;

		if (skynet_context_push(destination, &smsg)) {
			skynet_free(data);
			return -1;
		}
	}
	return session;
}

/**
 * @brief send msg to other module by name
 * @param[in] contex module handle
 * @param[in] source msg from
 * @param[in] addr   dst string 
 * @param[in] type msg type
 * @param[in] session session id of this msg
 * @param[out] data ptr payload
 * @param[in] sz size of the payload
 *
 */
int
skynet_sendname(struct skynet_context * context, uint32_t source, const char * addr , int type, int session, void * data, size_t sz) {
    	/*use module is if source == 0*/
        if (source == 0) {
		source = context->handle;
	}
	uint32_t des = 0;
	if (addr[0] == ':') {
		des = strtoul(addr+1, NULL, 16);
	} else if (addr[0] == '.') {
		des = skynet_handle_findname(addr + 1);
		if (des == 0) {
			if (type & PTYPE_TAG_DONTCOPY) {
				skynet_free(data);
			}
			return -1;
		}
	} else {
	        /*msg send to remote!!!!!!*/
		_filter_args(context, type, &session, (void **)&data, &sz);
            
                /*build remote msg*/
		struct remote_message * rmsg = skynet_malloc(sizeof(*rmsg));
		copy_name(rmsg->destination.name, addr);
		rmsg->destination.handle = 0;
		rmsg->message = data;
		rmsg->sz = sz;
                /*send msg to the remote*/
		skynet_harbor_send(rmsg, source, session);
		return session;
	}
        /*send msg to local module*/
	return skynet_send(context, source, des, type, session, data, sz);
}

/**
 * @brief get the id of the module handle
 * @return the module's id
 */
uint32_t 
skynet_context_handle(struct skynet_context *ctx) {
	return ctx->handle;
}

/**
 * @breif reigster the callback and module structure into module manager 
 * @param[in] cotext module manager
 * @param[in] ud structure for the module
 * @param[in] cb callback for deal with message
 * @ud and cb build by init _callback in the module
 */
void 
skynet_callback(struct skynet_context * context, void *ud, skynet_cb cb) {
	context->cb = cb;
	context->cb_ud = ud;
}


/**
 * @brief module send one msg
 * @param[in] ctx handle  for module
 * @param[in] msg msg wait to sand
 * @param[in] sz size of the payload in msg
 * @param[in] source source of the msg
 * @param[in] type node id for the msg
 * @param[in] session id of the msg
 * @bbuild one msg and push it into module queue
 *
 */
void
skynet_context_send(struct skynet_context * ctx, void * msg, size_t sz, uint32_t source, int type, int session) {
	struct skynet_message smsg;
	smsg.source = source;
	smsg.session = session;
	smsg.data = msg;
	smsg.sz = sz | type << HANDLE_REMOTE_SHIFT;

	skynet_mq_push(ctx->queue, &smsg);
}

/**
 * @brief init the node manager for skynet
 */
void 
skynet_globalinit(void) {
        /*set tot module's record*/
	G_NODE.total = 0;
	
	G_NODE.monitor_exit = 0;

	G_NODE.init = 1;
	if (pthread_key_create(&G_NODE.handle_key, NULL)) {
		fprintf(stderr, "pthread_key_create failed");
		exit(1);
	}
	// set mainthread's key
	skynet_initthread(THREAD_MAIN);
}

/***
 * @brief delete share area for thread
 */
void 
skynet_globalexit(void) {
	pthread_key_delete(G_NODE.handle_key);
}

/** 
 * @brief push -v into thread share area
 *
 */
void
skynet_initthread(int m) {
	uintptr_t v = (uint32_t)(-m);
	pthread_setspecific(G_NODE.handle_key, (void *)v);
}

