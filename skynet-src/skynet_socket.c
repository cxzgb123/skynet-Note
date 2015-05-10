/**
 * @file skynet_socket.c
 * @brief api for use to ctrl socket and socket thread api
 *
 */
#include "skynet.h"

#include "skynet_socket.h"
#include "socket_server.h"
#include "skynet_server.h"
#include "skynet_mq.h"
#include "skynet_harbor.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/**
 * @brief handle in local
 */
static struct socket_server * SOCKET_SERVER = NULL;

/**
 * @brief init the socket manager
 * return the handle of the socket manager
 */
void 
skynet_socket_init() {
	SOCKET_SERVER = socket_server_create();
}

/**
 * @brief exit the socket thread
 */
void
skynet_socket_exit() {
	socket_server_exit(SOCKET_SERVER);
}

/**
 * @brief delete all of the socket thread
 */
void
skynet_socket_free() {
	socket_server_release(SOCKET_SERVER);
	SOCKET_SERVER = NULL;
}

// mainloop thread
/**
 * @brief forward the msg to the usr module 
 * @param[in] type what happened 
 * @param[in] padding store payload after msg ?
 * @param[in] result result msg wait to send 
 */
static void
forward_message(int type, bool padding, struct socket_message * result) {
	struct skynet_socket_message *sm;
	int sz = sizeof(*sm);
	if (padding) {
		if (result->data) {
			sz += strlen(result->data);
		} else {
			result->data = "";
		}
	}
	sm = (struct skynet_socket_message *)skynet_malloc(sz);
	sm->type = type;            //msg type
	sm->id = result->id;        //
	sm->ud = result->ud;
	if (padding) {
		sm->buffer = NULL;
		/*store payload after msg*/
		memcpy(sm+1, result->data, sz - sizeof(*sm));
	} else {
	        /*store payload in data*/
		sm->buffer = result->data;
	}

	struct skynet_message message;
	message.source = 0;
	message.session = 0;
	message.data = sm;
	/*node id in high 8bits*/
	message.sz = sz | PTYPE_SOCKET << HANDLE_REMOTE_SHIFT;
	
	//push the msg into queue to the module
	if (skynet_context_push((uint32_t)result->opaque, &message)) {
		// don't call skynet_socket_close here (It will block mainloop)
		skynet_free(sm->buffer);
		skynet_free(sm);
	}
}

/**
 *  @brief deal with the msg from usr and forward the result msg to the module 
 */
int 
skynet_socket_poll() {
	struct socket_server *ss = SOCKET_SERVER;
	assert(ss);
	/*result used to hold return msg from the socket_server_poll*/
	struct socket_message result;
	int more = 1;
	
	/*wait evnet occured, and solve the event with the result msg as return */
	int type = socket_server_poll(ss, &result, &more);

	/*forward the result msg recive */
	switch (type) {
	case SOCKET_EXIT:   /*close the socket manager*/
		return 0;
	case SOCKET_DATA:   /*forward payload recive from tcp*/
		forward_message(SKYNET_SOCKET_TYPE_DATA, false, &result);
		break;
	case SOCKET_CLOSE:  /*forward socket close notice*/
		forward_message(SKYNET_SOCKET_TYPE_CLOSE, false, &result);
		break;
	case SOCKET_OPEN:   /*forward socket open info*/
		forward_message(SKYNET_SOCKET_TYPE_CONNECT, true, &result);
		break;
	case SOCKET_ERROR: /*forward socket error info*/
		forward_message(SKYNET_SOCKET_TYPE_ERROR, false, &result);
		break;
	case SOCKET_ACCEPT:/*forward socket accept msg*/
		forward_message(SKYNET_SOCKET_TYPE_ACCEPT, true, &result);
		break;
	case SOCKET_UDP:  /*forward payload recive from udp*/
		forward_message(SKYNET_SOCKET_TYPE_UDP, false, &result);
		break;
	default:
		skynet_error(NULL, "Unknown socket message type %d.",type);
		return -1;
	}
	/*exit when more is ture*/
	if (more) {
		return -1;
	}
	return 1;
}

static int
check_wsz(struct skynet_context *ctx, int id, void *buffer, int64_t wsz) {
	if (wsz < 0) {
		skynet_free(buffer);
		return -1;
	} else if (wsz > 1024 * 1024) {
		int kb4 = wsz / 1024 / 4;
		if (kb4 % 256 == 0) {
			skynet_error(ctx, "%d Mb bytes on socket %d need to send out", (int)(wsz / (1024 * 1024)), id);
		}
	}
	return 0;
}

/**
 *  @brief send payload high
 *  @param[in] ctx handle of the module
 *  @param[in] id id of the slot
 *  @param[in] buffer payload
 *  @param[in] sz payload size
 *  @note api for usr
 *
 */
int
skynet_socket_send(struct skynet_context *ctx, int id, void *buffer, int sz) {
	int64_t wsz = socket_server_send(SOCKET_SERVER, id, buffer, sz);
	return check_wsz(ctx, id, buffer, wsz);
}


/**
 *  @brief send payload low
 *  @param[in] ctx handle of the module
 *  @param[in] id id of the slot
 *  @param[in] buffer payload
 *  @param[in] sz payload size
 *  @note api for usr
 *
 */
void
skynet_socket_send_lowpriority(struct skynet_context *ctx, int id, void *buffer, int sz) {
	socket_server_send_lowpriority(SOCKET_SERVER, id, buffer, sz);
}

/**
 *  @brief send listen request
 *  @param[in] ctx handle of the module
 *  @param[in] host host to bind
 *  @param[in] port port to listen
 *  @param[in] backlog listen limit fd
 *  @note api for usr
 *
 */
int 
skynet_socket_listen(struct skynet_context *ctx, const char *host, int port, int backlog) {
	uint32_t source = skynet_context_handle(ctx);
	return socket_server_listen(SOCKET_SERVER, source, host, port, backlog);
}


/**
 *  @brief send connect request
 *  @param[in] ctx handle of the module
 *  @param[in] host host to connect
 *  @param[in] port port to connect
 *  @note api for usr
 *
 */
int 
skynet_socket_connect(struct skynet_context *ctx, const char *host, int port) {
        /*index of the of module*/
	uint32_t source = skynet_context_handle(ctx);
	/*source as the the opaque*/
	return socket_server_connect(SOCKET_SERVER, source, host, port);
}

/**
 *  @brief send bind request
 *  @param[in] ctx handle of the module
 *  @param[in] file file to bind
 *  @note api for usr
 *
 */
int 
skynet_socket_bind(struct skynet_context *ctx, int fd) {
	uint32_t source = skynet_context_handle(ctx);
	return socket_server_bind(SOCKET_SERVER, source, fd);
}

/**
 *  @brief send sockt close request
 *  @param[in] ctx handle of the module
 *  @param[in] id socket id
 *  @note api for usr
 */
void 
skynet_socket_close(struct skynet_context *ctx, int id) {
	uint32_t source = skynet_context_handle(ctx);
	socket_server_close(SOCKET_SERVER, source, id);
}


/**
 *  @brief send sockt start request
 *  @param[in] id socket id
 *  @param[in] ctx handle of the module
 *  @note api for usr
 */
void 
skynet_socket_start(struct skynet_context *ctx, int id) {
	uint32_t source = skynet_context_handle(ctx);
	socket_server_start(SOCKET_SERVER, source, id);
}


/**
 *  @brief send sockt nodely request
 *  @param[in] ctx handle of the module
 *  @param[in] id socket id
 *  @note api for usr
 */
void
skynet_socket_nodelay(struct skynet_context *ctx, int id) {
	socket_server_nodelay(SOCKET_SERVER, id);
}

/**
 *  @brief send udp sockt  request to init udp socket
 *  @param[in] ctx handle of the module
 *  @param[in] addr address to send or null
 *  @param[in] port address to send or null
 *  @note api for us
 */
int 
skynet_socket_udp(struct skynet_context *ctx, const char * addr, int port) {
	uint32_t source = skynet_context_handle(ctx);
	return socket_server_udp(SOCKET_SERVER, source, addr, port);
}

/**
 *  @brief send request to set udp payload send to
 *  @param[in] ctx handle of the module
 *  @param[in] addr address to send or null
 *  @param[in] port address to send or null
 *  @note api for us
 */
int 
skynet_socket_udp_connect(struct skynet_context *ctx, int id, const char * addr, int port) {
	return socket_server_udp_connect(SOCKET_SERVER, id, addr, port);
}

/**
 *  @brief send payload to udp socket
 *  @param[in] ctx handle of the module
 *  @param[in] addr address to send or null
 *  @param[in] buffer payload of udp msg
 *  @param[in] sz szie of payload
 *  @note api for us
 */
int 
skynet_socket_udp_send(struct skynet_context *ctx, int id, const char * address, const void *buffer, int sz) {
	int64_t wsz = socket_server_udp_send(SOCKET_SERVER, id, (const struct socket_udp_address *)address, buffer, sz);
	return check_wsz(ctx, id, (void *)buffer, wsz);
}

/**
 * @brief get the address string of the udp address
 * @return string of the udp address
 */
const char *
skynet_socket_udp_address(struct skynet_socket_message *msg, int *addrsz) {
	if (msg->type != SKYNET_SOCKET_TYPE_UDP) {
		return NULL;
	}
	struct socket_message sm;
	sm.id = msg->id;
	sm.opaque = 0;
	sm.ud = msg->ud;
	sm.data = msg->buffer;
	return (const char *)socket_server_udp_address(SOCKET_SERVER, &sm, addrsz);
}
