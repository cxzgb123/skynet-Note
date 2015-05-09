#include "skynet.h"

#include "socket_server.h"
#include "socket_poll.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>

#define MAX_INFO 128
// MAX_SOCKET will be 2^MAX_SOCKET_P
#define MAX_SOCKET_P 16             /*max socket will be 2^MAX_SOCKET_P*/  
#define MAX_EVENT 64                /*max event can hold*/
#define MIN_READ_BUFFER 64          /*origin read buffer for tcp*/
#define SOCKET_TYPE_INVALID 0       /*invalid flag*/
#define SOCKET_TYPE_RESERVE 1       /*socket empty*/
#define SOCKET_TYPE_PLISTEN 2       /*build socket before listen*/
#define SOCKET_TYPE_LISTEN 3        /*start listen*/
#define SOCKET_TYPE_CONNECTING 4    /*try to connect but not success connected*/
#define SOCKET_TYPE_CONNECTED 5     /*skynet connect to others success*/
#define SOCKET_TYPE_HALFCLOSE 6     /*half close, so we can trans more data*/
#define SOCKET_TYPE_PACCEPT 7       /*others connect to skynet success*/
#define SOCKET_TYPE_BIND 8          /*bind the address*/

#define MAX_SOCKET (1<<MAX_SOCKET_P)/*max socket*/ 

#define PRIORITY_HIGH 0             /*high write buffer list*/
#define PRIORITY_LOW 1              /*low write buffer list*/

/*hash function*/
#define HASH_ID(id) (((unsigned)id) % MAX_SOCKET) /*convert id to socket slot's index*/

#define PROTOCOL_TCP 0              /*tcp*/
#define PROTOCOL_UDP 1              /*udp*/
#define PROTOCOL_UDPv6 2            /*udpv6*/

#define UDP_ADDRESS_SIZE 19	    // ipv6 128bit + port 16bit + 1 byte type

#define MAX_UDP_PACKAGE 65535       /*max payload for udp*/

/**
 * @brief buffer wait to write
 */
struct write_buffer {
	struct write_buffer * next; /*link all buffer as a list*/
	void *buffer;               /*store the payload wait to write*/
	char *ptr;                  /*point to the start of the partial wait to send*/
	int sz;                     /*payload size*/
	bool userobject;            /*mark if the mem managered by usr*/
	uint8_t udp_address[UDP_ADDRESS_SIZE]; /*udp address want to  send to*/
};

#define SIZEOF_TCPBUFFER (offsetof(struct write_buffer, udp_address[0]))
#define SIZEOF_UDPBUFFER (sizeof(struct write_buffer))

/**
 * @brief payload wait to write linked together as a list
 */
struct wb_list {
	struct write_buffer * head; /*head of the write list*/
	struct write_buffer * tail; /*tail of the write list*/
};


/**
 * @brief manager all the socket
 */
struct socket {
	uintptr_t opaque;   /*id of the module socket belong to*/
	struct wb_list high;/*high rate write buffer list*/    
	struct wb_list low; /*low  rate write buffer list*/
	int64_t wb_size;    /*tot size of the payload wait to send*/
	int fd;             /*socket fd*/
	int id;             /*id alloc for this socket*/ 
	uint16_t protocol;  /*link protocol*/
	uint16_t type;      /*status of the socket*/
	union {
		int size;   /*tcp read buffer size*/
		uint8_t udp_address[UDP_ADDRESS_SIZE]; /*udp address of the udp socket*/
	} p;
};

/**
 * @brief manager of all socket thread
 */
struct socket_server {
	int recvctrl_fd;    /*read fd of pipe*/
	int sendctrl_fd;    /*send fd of pipe*/
	int checkctrl;      /*mark if check the ctrl msg from pipe*/ 
	poll_fd event_fd;   /*epoll handle*/
	int alloc_id;       /*curr id alloc for socket, socket_id = HASH_ID(alloc_id < 0 ? alloc + 0x7fffffff, alloc_id)*/
	int event_n;        /*tot event occured*/
	int event_index;    /*idx of the event need to solve*/
	struct socket_object_interface soi; /*used when need ctrl payload by user api*/
	struct event ev[MAX_EVENT];         /*used to get event when call epoll*/
	struct socket slot[MAX_SOCKET];     /*store all the socket*/
	char buffer[MAX_INFO];
	uint8_t udpbuffer[MAX_UDP_PACKAGE]; /*buffer used to recive udp msg*/
	fd_set rfds;                        /*fd group*/
};

/**
 * @brief msg used to connect to others 
 */
struct request_open {
	int id;                 /*id of the socket*/
	int port;               /*port of the machine to be connected*/
	uintptr_t opaque;       /*id of module socket belong to*/
	char host[1];           /*host if the socket need to open*/   
};

/** 
 *  @brief request to send data
 */
struct request_send {
	int id;                 /*id of the socket*/
	int sz;                 /*data size*/
	char * buffer;          /*payload ptr*/
};

/**
 * @brief request udp msg wait to send
 */
struct request_send_udp {
	struct request_send send;               /*payload*/
	uint8_t address[UDP_ADDRESS_SIZE];      /*udp address*/
};

/**
 * @brief request used to set udp socket address
 * @param[in] id id of the socket
 * @param[in] address string address of the  udp socket
 */
struct request_setudp {
	int id;                                 /*id of the socket*/
	uint8_t address[UDP_ADDRESS_SIZE];      /*string address of the udp*/
};

/**
 * @brief reuquest msg for  close socket
 */
struct request_close {
	int id;                                 /*id of the socket*/
	uintptr_t opaque;                       /*id of module socket belong to*/
};

/**
 * @brief request msg for listen
 */
struct request_listen {
	int id;                                 /*id of the socket*/
	int fd;                                 /*listem fd*/
	uintptr_t opaque;                       /*id of the module socket belong to*/
	char host[1];                           /*TODO not used???*/
};

/**
 * @brief request msg to bind
 */
struct request_bind {
	int id;                                 /*id of the socket*/
	int fd;                                 /*bind file*/
	uintptr_t opaque;                       /*id of the  module socket belong to*/
};

/**
 * @brief request socket start
 */
struct request_start {
	int id;                                 /*id of the socket*/
	uintptr_t opaque;                       /*id of the module socket belong to*/
};

/**
 * @brief request to set socket
 */
struct request_setopt {
	int id;                                 /*id of the socket*/
	int what;                               /*key for setopt*/
	int value;                              /*val for set key= val*/
};

/**
 * @brief to init udp socket
 */
struct request_udp {
	int id;                                 /*id of the s*/
	int fd;                                 /*socket fd*/    
	int family;                             /*famliy of the protocol*/
	uintptr_t opaque;                       /*id of the module socket belong to*/
};

/*
	The first byte is TYPE

	S Start socket
	B Bind socket
	L Listen socket
	K Close socket
	O Connect to (Open)
	X Exit
	D Send package (high)
	P Send package (low)
	A Send UDP package
	T Set opt
	U Create UDP socket
	C set udp address
 */
struct request_package {
	uint8_t header[8];	// 6 bytes dummy
	union {
		char buffer[256];
		struct request_open open;
		struct request_send send;
		struct request_send_udp send_udp;
		struct request_close close;
		struct request_listen listen;
		struct request_bind bind;
		struct request_start start;
		struct request_setopt setopt;
		struct request_udp udp;
		struct request_setudp set_udp;
	} u;
	uint8_t dummy[256];
};

/**
 * @brief normal sockaddr storage
 *
 */
union sockaddr_all {
	struct sockaddr s;          /*normal storage*/
	struct sockaddr_in v4;      /*ipv4 storage*/
	struct sockaddr_in6 v6;     /*ipv6 storage*/
};

/**
 * @brief warp of obj wait to send
 */
struct send_object {
	void * buffer;                          /*payload*/
	int sz;                                 /*size of payload*/
	void (*free_func)(void *);              /*callback used to free payload*/
};

#define MALLOC skynet_malloc
#define FREE skynet_free

/**
 * @brief convert oject to send_object
 * @param[in] ss socket manager
 * @param[in] so warp of object
 * @param[in] object org object
 * @param[in] sz sz < 0 ? object mem managered by usr api : object mem managered by skynet api
 * 
 */
static inline bool
send_object_init(struct socket_server *ss, struct send_object *so, void *object, int sz) {
	if (sz < 0) {
		so->buffer = ss->soi.buffer(object);
		so->sz = ss->soi.size(object);
		so->free_func = ss->soi.free;
		return true;
	} else {
		so->buffer = object;
		so->sz = sz;
		so->free_func = FREE;
		return false;
	}
}


/**
 * @brief try to free write buffer
 * @brief ss socket manager
 * @brief wb write buffer wait to free
 *
 */
static inline void
write_buffer_free(struct socket_server *ss, struct write_buffer *wb) {
	if (wb->userobject) {
		ss->soi.free(wb->buffer);
	} else {
		FREE(wb->buffer);
	}
	FREE(wb);
}

/**
 * @brief socket install keepalive 
 * @param[in]  fd socket wait to set keepalive
 */
static void
socket_keepalive(int fd) {
	int keepalive = 1;
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (void *)&keepalive , sizeof(keepalive));  
}


/**
 * @brief get id by socket_server
 * @return id for the socket
 *
 */
static int
reserve_id(struct socket_server *ss) {
	int i;
	/*lookup all socket*/
	for (i=0;i<MAX_SOCKET;i++) {
	        /*here is will alloc as 1 ,2, 3, INT_MAX, INT_MAX - 1 ... 3 2 1*/
		int id = __sync_add_and_fetch(&(ss->alloc_id), 1);
		if (id < 0) {
			id = __sync_and_and_fetch(&(ss->alloc_id), 0x7fffffff);
		}
		/*lookup slot by hash*/
		struct socket *s = &ss->slot[HASH_ID(id)];
		/*try to get the empty slots*/
		if (s->type == SOCKET_TYPE_INVALID) {
			if (__sync_bool_compare_and_swap(&s->type, SOCKET_TYPE_INVALID, SOCKET_TYPE_RESERVE)) {
				s->id = id;
				s->fd = -1;
				return id;
			} else {
				// retry
				--i;
			}
		}
	}
	return -1;
}

/**
 * @brief init the write buffer list
 * @param[in] wb_list write buffer list
 */
static inline void
clear_wb_list(struct wb_list *list) {
	list->head = NULL;
	list->tail = NULL;
}


/**
 * @brief create server manager for skynet 
 * @return handle for server in skynet
 */
struct socket_server * 
socket_server_create() {
	int i;
	int fd[2];
	/*handle for epoll init*/
	poll_fd efd = sp_create();
	if (sp_invalid(efd)) {
		fprintf(stderr, "socket-server: create event pool failed.\n");
		return NULL;
	}
	/*create pipe*/
	if (pipe(fd)) {
		sp_release(efd);
		fprintf(stderr, "socket-server: create socket pair failed.\n");
		return NULL;
	}
	/*listen read of pipe to recive the msg from the module*/
	if (sp_add(efd, fd[0], NULL)) {
		// add recvctrl_fd to event poll
		fprintf(stderr, "socket-server: can't add server fd to event pool.\n");
		close(fd[0]);
		close(fd[1]);
		sp_release(efd);
		return NULL;
	}

        /*alloc socket manager*/
	struct socket_server *ss = MALLOC(sizeof(*ss));
	/*store the epoll handle in manager*/
	ss->event_fd = efd;

	/*here!! You see, pipe get the request msg from module to socket*/

        /*store the fd of pipe in socket manager*/
	ss->recvctrl_fd = fd[0];
	ss->sendctrl_fd = fd[1];

	/*check ctrl in default*/
	ss->checkctrl = 1;

	/*init all socket handle*/
	for (i=0;i<MAX_SOCKET;i++) {
		struct socket *s = &ss->slot[i];
		s->type = SOCKET_TYPE_INVALID;
		clear_wb_list(&s->high);
		clear_wb_list(&s->low);
	}
	ss->alloc_id = 0;
	ss->event_n = 0;
	ss->event_index = 0;
	memset(&ss->soi, 0, sizeof(ss->soi));
	FD_ZERO(&ss->rfds);
	assert(ss->recvctrl_fd < FD_SETSIZE);

	return ss;
}

/**
 * @brief free all in write buffer list and init the write_buffer list
 * @param[in] socket manager
 * @param[in] list write buffer list
 */
static void
free_wb_list(struct socket_server *ss, struct wb_list *list) {
	struct write_buffer *wb = list->head;
	while (wb) {
		struct write_buffer *tmp = wb;
		wb = wb->next;
		write_buffer_free(ss, tmp);
	}
	list->head = NULL;
	list->tail = NULL;
}

/**
 * @brief force close the socket
 * @param[in] ss socket manager
 * @param[in] s socket wait to close
 * @param[out] result result msg from the function
 */ 
static void
force_close(struct socket_server *ss, struct socket *s, struct socket_message *result) {
	result->id = s->id;
	result->ud = 0;
	result->data = NULL;
	result->opaque = s->opaque;
	if (s->type == SOCKET_TYPE_INVALID) {
		return;
	}
	assert(s->type != SOCKET_TYPE_RESERVE);
	free_wb_list(ss,&s->high);
	free_wb_list(ss,&s->low);
	if (s->type != SOCKET_TYPE_PACCEPT && s->type != SOCKET_TYPE_PLISTEN) {
		sp_del(ss->event_fd, s->fd);
	}
	if (s->type != SOCKET_TYPE_BIND) {
		close(s->fd);
	}
	s->type = SOCKET_TYPE_INVALID;
}


/**
 * @brief release all things in socket manager
 * @param[in] ss socket manager
 */
void 
socket_server_release(struct socket_server *ss) {
	int i;
	struct socket_message dummy;
	/*release the socket */
	for (i=0;i<MAX_SOCKET;i++) {
		struct socket *s = &ss->slot[i];
		if (s->type != SOCKET_TYPE_RESERVE) {
			force_close(ss, s , &dummy);
		}
	}
	/*close ctrl pipe*/
	close(ss->sendctrl_fd);
	close(ss->recvctrl_fd);
	/*close epoll*/
	sp_release(ss->event_fd);
	FREE(ss);
}

/**
 * @brief check the write buffer list if it is empty
 * @param[in] s write buffer list
 */
static inline void
check_wb_list(struct wb_list *s) {
	assert(s->head == NULL);
	assert(s->tail == NULL);
}

/**
 * @brief create a new socket and register read evnet
 * @param[in] ss socket manager
 * @param[in] id socket id
 * @param[in] fd socket fd
 * @param[in] protocol protocol for the socket
 * @param[in] opaque id of the module create this socket 
 * @param[in] add  flag that if need register read event
 * return success ? socket : NULL
 */
static struct socket *
new_fd(struct socket_server *ss, int id, int fd, int protocol, uintptr_t opaque, bool add) {
	struct socket * s = &ss->slot[HASH_ID(id)];
	/*conflict, error*/
	assert(s->type == SOCKET_TYPE_RESERVE);

	if (add) {
	        /*add read event*/
		if (sp_add(ss->event_fd, fd, s)) {
			s->type = SOCKET_TYPE_INVALID;
			return NULL;
		}
	}

	s->id = id;                  /*record the socket id*/
	s->fd = fd;                  /*record the socket file description*/
	s->protocol = protocol;      /*record the protocol*/
	s->p.size = MIN_READ_BUFFER; /*set the origin read buffer size*/
	s->opaque = opaque;          /*record the module id*/

	/*init the write buffer*/
	s->wb_size = 0;
	check_wb_list(&s->high);    /*high rate payload list*/
	check_wb_list(&s->low);     /*loew rate payload list*/
	return s;
}

// return -1 when connecting
/**
 * @brief open tcp socket 
 * @param[in] ss socket manager
 * @param[in] request request open msg
 * @param[in] result get result of open tcp
 * @note 
 * change to connecting status when EINPROGRESS occured in connecting,
 *
 */
static int
open_socket(struct socket_server *ss, struct request_open * request, struct socket_message *result) {
	int id = request->id;
	/*build result msg*/
	result->opaque = request->opaque;
	result->id = id;
	result->ud = 0;
	result->data = NULL;
	struct socket *ns;
	int status;
	struct addrinfo ai_hints;
	struct addrinfo *ai_list = NULL;
	struct addrinfo *ai_ptr = NULL;
	char port[16];
	sprintf(port, "%d", request->port);

	memset(&ai_hints, 0, sizeof( ai_hints ) );
	ai_hints.ai_family = AF_UNSPEC;
	ai_hints.ai_socktype = SOCK_STREAM;
	ai_hints.ai_protocol = IPPROTO_TCP;
        
        /*lookup address of the host*/
	status = getaddrinfo( request->host, port, &ai_hints, &ai_list );
	if ( status != 0 ) {
		goto _failed;
	}
	int sock= -1;
	for (ai_ptr = ai_list; ai_ptr != NULL; ai_ptr = ai_ptr->ai_next ) {
		sock = socket( ai_ptr->ai_family, ai_ptr->ai_socktype, ai_ptr->ai_protocol );
		if ( sock < 0 ) {
			continue;
		}
		/*install keepalive*/
		socket_keepalive(sock);
		/*set socket nonblock*/
		sp_nonblocking(sock);
		/*try to connect to the host*/
		status = connect( sock, ai_ptr->ai_addr, ai_ptr->ai_addrlen);
		if ( status != 0 && errno != EINPROGRESS) {
		        /*close sock if fail*/
			close(sock);
			sock = -1;
			continue;
		}
                /*EINPOGRESS !! , so we try to connect next time*/
		break;
	}

	if (sock < 0) {
		goto _failed;
	}

	/*get a new socket with tcp*/
	ns = new_fd(ss, id, sock, PROTOCOL_TCP, request->opaque, true);
	if (ns == NULL) {
		close(sock);
		goto _failed;
	}

	if(status == 0) {
	        /*open success*/
		ns->type = SOCKET_TYPE_CONNECTED;
		struct sockaddr * addr = ai_ptr->ai_addr;
		void * sin_addr = (ai_ptr->ai_family == AF_INET) ? (void*)&((struct sockaddr_in *)addr)->sin_addr : (void*)&((struct sockaddr_in6 *)addr)->sin6_addr;
		if (inet_ntop(ai_ptr->ai_family, sin_addr, ss->buffer, sizeof(ss->buffer))) {
			result->data = ss->buffer;
		}
		freeaddrinfo( ai_list );
		return SOCKET_OPEN;
	} else {
	        /*we'll try connect next time*/
		ns->type = SOCKET_TYPE_CONNECTING;
		sp_write(ss->event_fd, ns->fd, ns, true);
	}

	freeaddrinfo( ai_list );
	return -1;
_failed:
	freeaddrinfo( ai_list );
	ss->slot[HASH_ID(id)].type = SOCKET_TYPE_INVALID;
	return SOCKET_ERROR;
}

/**
 * @brief send msg to tcp socket in socket manager
 * @param[in] ss socket manger
 * @param[in] list  list of payload wait to send 
 * @param[out] result msg result 
 */ 
static int
send_list_tcp(struct socket_server *ss, struct socket *s, struct wb_list *list, struct socket_message *result) {
	while (list->head) {
		struct write_buffer * tmp = list->head;
		for (;;) {
			int sz = write(s->fd, tmp->ptr, tmp->sz);
			if (sz < 0) {
				switch(errno) {
				case EINTR:
				        /*other interupt, so we try again*/
					continue;
				case EAGAIN:
				        /*nonblock write, may buffer in linux kernel full! so try next time*/
					return -1;
				}
				/*error, so close socket*/
				force_close(ss,s, result);
				return SOCKET_CLOSE;
			}
			/*write buffer into socket here*/
			s->wb_size -= sz;
			if (sz != tmp->sz) {
				tmp->ptr += sz;
				tmp->sz -= sz;
				return -1;
			}
			break;
		}
		list->head = tmp->next;
		write_buffer_free(ss,tmp);
	}
	list->tail = NULL;

	return -1;
}

/**
 * @brief build sa by address 
 * @param[s] s socket 
 * @param[in] udp_address udp addres string
 * @param[out] sa storage for udp msg
 * @return sa len
 *
 */
static socklen_t
udp_socket_address(struct socket *s, const uint8_t udp_address[UDP_ADDRESS_SIZE], union sockaddr_all *sa) {
	int type = (uint8_t)udp_address[0];
	if (type != s->protocol)
		return 0;
	uint16_t port = 0;
	memcpy(&port, udp_address+1, sizeof(uint16_t));
	switch (s->protocol) {
	case PROTOCOL_UDP:
		memset(&sa->v4, 0, sizeof(sa->v4));
		sa->s.sa_family = AF_INET;
		sa->v4.sin_port = port;
		memcpy(&sa->v4.sin_addr, udp_address + 1 + sizeof(uint16_t), sizeof(sa->v4.sin_addr));	// ipv4 address is 32 bits
		return sizeof(sa->v4);
	case PROTOCOL_UDPv6:
		memset(&sa->v6, 0, sizeof(sa->v6));
		sa->s.sa_family = AF_INET6;
		sa->v6.sin6_port = port;
		memcpy(&sa->v6.sin6_addr, udp_address + 1 + sizeof(uint16_t), sizeof(sa->v6.sin6_addr)); // ipv6 address is 128 bits
		return sizeof(sa->v6);
	}
	return 0;
}

/**
 * @brief send udp msg
 * @param[in] ss socket manager
 * @param[in] s socket handle
 * @param[in] list buffer wait to send
 * @param[out] result for store function return 
 */
static int
send_list_udp(struct socket_server *ss, struct socket *s, struct wb_list *list, struct socket_message *result) {
	while (list->head) {
		struct write_buffer * tmp = list->head;
		union sockaddr_all sa;
		/*conver all udp address to normal address*/
		socklen_t sasz = udp_socket_address(s, tmp->udp_address, &sa);
		/*send udp packet*/
		int err = sendto(s->fd, tmp->ptr, tmp->sz, 0, &sa.s, sasz);
		if (err < 0) {
			switch(errno) {
			/*ignore all erro in udp send*/
			case EINTR:
			case EAGAIN:
				return -1;
			}
			fprintf(stderr, "socket-server : udp (%d) sendto error %s.\n",s->id, strerror(errno));
			return -1;
/*			// ignore udp sendto error
			
			result->opaque = s->opaque;
			result->id = s->id;
			result->ud = 0;
			result->data = NULL;

			return SOCKET_ERROR;
*/
		}

		s->wb_size -= tmp->sz;
		list->head = tmp->next;
		write_buffer_free(ss,tmp);
	}
	list->tail = NULL;

	return -1;
}

/**
 * @brief send buffer into socket
 * @param[in] socket manager
 * @param[in] socket socket which buffer send  to 
 * @param[in] list buffer wait to send
 * @param[in] result result used to hold return msg
 * @note support tcp and udp
 *
 */
static int
send_list(struct socket_server *ss, struct socket *s, struct wb_list *list, struct socket_message *result) {
	if (s->protocol == PROTOCOL_TCP) {
	    /*tcp support*/
		return send_list_tcp(ss, s, list, result);
	} else {
	    /*udp support*/
		return send_list_udp(ss, s, list, result);
	}
}

/**
 * @brief check if there is partial in the head of write_buffer list
 * @param[in] s write buffer list
 */
static inline int
list_uncomplete(struct wb_list *s) {
	struct write_buffer *wb = s->head;
	if (wb == NULL)
		return 0;
	return (void *)wb->ptr != wb->buffer;
}

/**
 * @brief move one payload from low list to high
 * @param[s] s socket handle`
 */
static void
raise_uncomplete(struct socket * s) {
	struct wb_list *low = &s->low;
	struct write_buffer *tmp = low->head;
	low->head = tmp->next;
	if (low->head == NULL) {
		low->tail = NULL;
	}

	// move head of low list (tmp) to the empty high list
	struct wb_list *high = &s->high;
	assert(high->head == NULL);

	tmp->next = NULL;
	high->head = high->tail = tmp;
}

/*
	Each socket has two write buffer list, high priority and low priority.

	1. send high list as far as possible.
	2. If high list is empty, try to send low list.
	3. If low list head is uncomplete (send a part before), move the head of low list to empty high list (call raise_uncomplete) .
	4. If two lists are both empty, turn off the event. (call check_close)
 */
static int
send_buffer(struct socket_server *ss, struct socket *s, struct socket_message *result) {
	assert(!list_uncomplete(&s->low));
	// step 1 
	if (send_list(ss,s,&s->high,result) == SOCKET_CLOSE) {
		return SOCKET_CLOSE;
	}
	if (s->high.head == NULL) {
		// step 2
		if (s->low.head != NULL) {
			if (send_list(ss,s,&s->low,result) == SOCKET_CLOSE) {
				return SOCKET_CLOSE;
			}
			// step 3
			if (list_uncomplete(&s->low)) {
				raise_uncomplete(s);
			}
		} else {
			// step 4
			sp_write(ss->event_fd, s->fd, s, false);

			if (s->type == SOCKET_TYPE_HALFCLOSE) {
				force_close(ss, s, result);
				return SOCKET_CLOSE;
			}
		}
	}

	return -1;
}

/**
 * @brief appened new payload into write buffer list
 * @param[in] ss socket manager
 * @param[in] s write buffer list
 * @param[in] request request send msg from usr
 * @param[in] size size of buffer wait to send
 * @param[in] n as the offset of the write_buffer
 * @return new write buffer handle
 */
static struct write_buffer *
append_sendbuffer_(struct socket_server *ss, struct wb_list *s, struct request_send * request, int size, int n) {
	struct write_buffer * buf = MALLOC(size);
	struct send_object so;
	//build usrobject
	buf->userobject = send_object_init(ss, &so, request->buffer, request->sz);
	/*here!!! valid data of write buffer maybe a partital*/
	buf->ptr = (char*)so.buffer+n;
	buf->sz = so.sz - n;
	buf->buffer = request->buffer;
	buf->next = NULL;
	
	if (s->head == NULL) {
		s->head = s->tail = buf;
	} else {
		assert(s->tail != NULL);
		assert(s->tail->next == NULL);
		s->tail->next = buf;
		s->tail = buf;
	}
	return buf;
}

/**
 * @brief append to write_buffer
 * @param[in] ss socket manager
 * @param[in] s socket 
 * @param[in] requesst hold the payload wait to send
 * @param[in] string address of the udp
 */
static inline void
append_sendbuffer_udp(struct socket_server *ss, struct socket *s, int priority, struct request_send * request, const uint8_t udp_address[UDP_ADDRESS_SIZE]) {
	struct wb_list *wl = (priority == PRIORITY_HIGH) ? &s->high : &s->low;
	struct write_buffer *buf = append_sendbuffer_(ss, wl, request, SIZEOF_UDPBUFFER, 0);
	/*notice!!! write_buffer could hold the dest address*/
	memcpy(buf->udp_address, udp_address, UDP_ADDRESS_SIZE);
	/*update the tot payload size of the write buffer*/
	s->wb_size += buf->sz;
}

/**
 * @brief append new payload as a write buffer into high write buffer list
 * @param[in] ss socket manager
 * @param[in] s socket
 * @param[in] request  hold new payload wait to send
 */
static inline void
append_sendbuffer(struct socket_server *ss, struct socket *s, struct request_send * request, int n) {
	struct write_buffer *buf = append_sendbuffer_(ss, &s->high, request, SIZEOF_TCPBUFFER, n);
	s->wb_size += buf->sz;
}

/**
 * @brief append new payload as a write buffer into high write buffer list
 * @param[in] ss socket manager
 * @param[in] s socket
 * @param[in] request  hold new payload wait to send
 */
static inline void
append_sendbuffer_low(struct socket_server *ss,struct socket *s, struct request_send * request) {
	struct write_buffer *buf = append_sendbuffer_(ss, &s->low, request, SIZEOF_TCPBUFFER, 0);
	s->wb_size += buf->sz;
}

/**
 * @brief check if the write buffer is empty
 */
static inline int
send_buffer_empty(struct socket *s) {
	return (s->high.head == NULL && s->low.head == NULL);
}

/*
	When send a package , we can assign the priority : PRIORITY_HIGH or PRIORITY_LOW

	If socket buffer is empty, write to fd directly.
		If write a part, append the rest part to high list. (Even priority is PRIORITY_LOW)
	Else append package to high (PRIORITY_HIGH) or low (PRIORITY_LOW) list.
 */
static int
send_socket(struct socket_server *ss, struct request_send * request, struct socket_message *result, int priority, const uint8_t *udp_address) {
	int id = request->id;
	struct socket * s = &ss->slot[HASH_ID(id)];
	struct send_object so;
	/*convert obj to send_obj*/
	send_object_init(ss, &so, request->buffer, request->sz);
	/*check the status of the socket*/
	if (s->type == SOCKET_TYPE_INVALID || s->id != id 
		|| s->type == SOCKET_TYPE_HALFCLOSE
		|| s->type == SOCKET_TYPE_PACCEPT) {
		so.free_func(request->buffer);
		return -1;
	}
	assert(s->type != SOCKET_TYPE_PLISTEN && s->type != SOCKET_TYPE_LISTEN);

	/*connected but write buffer empty*/
	if (send_buffer_empty(s) && s->type == SOCKET_TYPE_CONNECTED) {
	        /*write buffer list is empty!!! so we can try to send directly*/
		if (s->protocol == PROTOCOL_TCP) {
		        /*try to send directly*/
			int n = write(s->fd, so.buffer, so.sz);
			if (n<0) {
				switch(errno) {
				case EINTR:
				case EAGAIN:
					n = 0;
					break;
					/*EINTR or EAGIN will try again*/
				default:
					fprintf(stderr, "socket-server: write to %d (fd=%d) error :%s.\n",id,s->fd,strerror(errno));
					force_close(ss,s,result);
					return SOCKET_CLOSE;
				}
			}
			if (n == so.sz) {
				so.free_func(request->buffer);
				return -1;
			}
			/*send EAGAIN or EINTR*/
			append_sendbuffer(ss, s, request, n);	// add to high priority list, even priority == PRIORITY_LOW
		} else {
			// udp
			if (udp_address == NULL) {
				udp_address = s->p.udp_address;
			}
			union sockaddr_all sa;
			socklen_t sasz = udp_socket_address(s, udp_address, &sa);
			int n = sendto(s->fd, so.buffer, so.sz, 0, &sa.s, sasz);
			if (n != so.sz) {
				append_sendbuffer_udp(ss,s,priority,request,udp_address);
			} else {
				so.free_func(request->buffer);
				return -1;
			}
		}
		/*register write evnet again if there is payload in write buffer list*/
		sp_write(ss->event_fd, s->fd, s, true);
	} else {
	        /*just put the payload into list when there is payload in the write buffer list*/
		if (s->protocol == PROTOCOL_TCP) {
			if (priority == PRIORITY_LOW) {
				append_sendbuffer_low(ss, s, request);
			} else {
				append_sendbuffer(ss, s, request, 0);
			}
		} else {
			if (udp_address == NULL) {
				udp_address = s->p.udp_address;
			}
			append_sendbuffer_udp(ss,s,priority,request,udp_address);
		}
	}
	return -1;
}

/**
 * @brief set the socket listen by reuqest listen msg 
 * @param[in] ss socket manager
 * @param[in] request request for set socket listen
 * @param[in] result used for hold the return val
 * @note just set type PLISTEN after create the socket
 *
 */
static int
listen_socket(struct socket_server *ss, struct request_listen * request, struct socket_message *result) {
    	/*get the socket id and the listen_fd*/
        int id = request->id;
	int listen_fd = request->fd;

	/*init the socket*/
	struct socket *s = new_fd(ss, id, listen_fd, PROTOCOL_TCP, request->opaque, false);
	if (s == NULL) {
		goto _failed;
	}
	s->type = SOCKET_TYPE_PLISTEN;
	return -1;
_failed:
	close(listen_fd);
	result->opaque = request->opaque;
	result->id = id;
	result->ud = 0;
	result->data = NULL;
	ss->slot[HASH_ID(id)].type = SOCKET_TYPE_INVALID;

	return SOCKET_ERROR;
}

/**
 * @brief close the socket by request close msg  
 * @param[in] ss socket manager
 * @param[in] request request close msg
 * @param[out] result hold the result msg
 *
 */
static int
close_socket(struct socket_server *ss, struct request_close *request, struct socket_message *result) {
	int id = request->id;
	struct socket * s = &ss->slot[HASH_ID(id)];
	if (s->type == SOCKET_TYPE_INVALID || s->id != id) {
		result->id = id;
		result->opaque = request->opaque;
		result->ud = 0;
		result->data = NULL;
		return SOCKET_CLOSE;
	}
	/*try to clean all payload in the write buffer list*/
	if (!send_buffer_empty(s)) { 
		int type = send_buffer(ss,s,result);
		/*try again, if there is payload wait to send*/
		if (type != -1)
			return type;
	}
        
        /*close socket after all payload sended */
	if (send_buffer_empty(s)) {
		force_close(ss,s,result);
		result->id = id;
		result->opaque = request->opaque;
		return SOCKET_CLOSE;
	}
	/*set as half close when close socket with payload left*/
	s->type = SOCKET_TYPE_HALFCLOSE;

	return -1;
}

/**
 * @brief bind the socket by the request bind msg 
 * @param[in] ss socket manger
 * @param[in] request request msg for bind
 * @param[out] hold the result from bind
 */
static int
bind_socket(struct socket_server *ss, struct request_bind *request, struct socket_message *result) {
	int id = request->id;
	/*init the result msg*/
	result->id = id;
	result->opaque = request->opaque;
	result->ud = 0;
	/*init the socket, tcp need bind*/
	struct socket *s = new_fd(ss, id, request->fd, PROTOCOL_TCP, request->opaque, true);
	if (s == NULL) {
		result->data = NULL;
		return SOCKET_ERROR;
	}
	/*set the socket in nonblock*/
	sp_nonblocking(request->fd);
	s->type = SOCKET_TYPE_BIND;
	result->data = "binding";
	return SOCKET_OPEN;
}

/**
 * @brief start socket after the socket attribute init 
 * @param[in] ss socket manager
 * @param[in] request request for start the socket
 * @param[in] result hold the result from start socket
 */
static int
start_socket(struct socket_server *ss, struct request_start *request, struct socket_message *result) {
	int id = request->id;

	/*build result msg*/
	result->id = id;
	result->opaque = request->opaque;
	result->ud = 0;
	result->data = NULL;
	struct socket *s = &ss->slot[HASH_ID(id)];

	/*check the socket wait to start*/
	if (s->type == SOCKET_TYPE_INVALID || s->id !=id) {
		return SOCKET_ERROR;
	}

	/*start socket by register the read event if the type in (PACCEPT PLISTEN)*/
	if (s->type == SOCKET_TYPE_PACCEPT || s->type == SOCKET_TYPE_PLISTEN) {
	        /*register the read evet*/
		if (sp_add(ss->event_fd, s->fd, s)) {
			s->type = SOCKET_TYPE_INVALID;
			return SOCKET_ERROR;
		}
		/*update the type of the socket*/
		s->type = (s->type == SOCKET_TYPE_PACCEPT) ? SOCKET_TYPE_CONNECTED : SOCKET_TYPE_LISTEN;
		s->opaque = request->opaque;
		result->data = "start";
		return SOCKET_OPEN;
	} else if (s->type == SOCKET_TYPE_CONNECTED) {
	        /*update module id if the socket connected*/
		s->opaque = request->opaque;
		result->data = "transfer";
		return SOCKET_OPEN;
	}
	return -1;
}

/**
 * @brief set opt for the socket
 * @param[in] ss socket manager
 * @param[in] request request setopt msg used to set socket
 *
 */
static void
setopt_socket(struct socket_server *ss, struct request_setopt *request) {
	int id = request->id;
	/*get the socket */
	struct socket *s = &ss->slot[HASH_ID(id)];
	if (s->type == SOCKET_TYPE_INVALID || s->id !=id) {
		return;
	}
	/*set the val for the socket*/
	int v = request->value;
	setsockopt(s->fd, IPPROTO_TCP, request->what, &v, sizeof(v));
}

/**
 * @brief block read ctrl msg from the pipe
 * @param[in] fd of the pipe
 * @param[out] hold payload  from the pipe
 * @param[in] sz the size of the storage which used to get payload
 */
static void
block_readpipe(int pipefd, void *buffer, int sz) {
	for (;;) {
	        /*try to read msg from pipe*/
		int n = read(pipefd, buffer, sz);
		if (n<0) {
		    /*continue if EINTR occured*/
			if (errno == EINTR)
				continue;
			fprintf(stderr, "socket-server : read pipe error %s.\n",strerror(errno));
			return;
		}
		// must atomic read from a pipe
		assert(n == sz);
		return;
	}
}

/**
 * @brief wait request msg from usr and slove this msg
 * @param[ss] socket manager
 */
static int
has_cmd(struct socket_server *ss) {
        
        /*nonblock */
	struct timeval tv = {0,0};
	int retval;

	FD_SET(ss->recvctrl_fd, &ss->rfds);

	retval = select(ss->recvctrl_fd+1, &ss->rfds, NULL, NULL, &tv);
	if (retval == 1) {
		return 1;
	}
	return 0;
}

/** 
 * @brief init udp socket for request msg from module 
 * @param[in] ss socket manager
 * @param[in] udp request msg for alloc udp socket in socket manager 
 *
 */
static void
add_udp_socket(struct socket_server *ss, struct request_udp *udp) {
        /*get the id of socket vec in socket manager*/
	int id = udp->id;
	int protocol;
	/*check protocol family*/
	if (udp->family == AF_INET6) {
		protocol = PROTOCOL_UDPv6;
	} else {
		protocol = PROTOCOL_UDP;
	}
	/*alloc socket manager*/
	struct socket *ns = new_fd(ss, id, udp->fd, protocol, udp->opaque, true);
	if (ns == NULL) {
		close(udp->fd);
		ss->slot[HASH_ID(id)].type = SOCKET_TYPE_INVALID;
		return;
	}
	/*!!!here, marked as connected directly*/
	ns->type = SOCKET_TYPE_CONNECTED;
	memset(ns->p.udp_address, 0, sizeof(ns->p.udp_address));
}

/**
 *  @brief set the udp address for the udp socket
 *  @param[in] ss socket manager
 *  @param[in] request request for set address for uddp socket
 *  @param[out] result hold the resuld for this action
 */
static int
set_udp_address(struct socket_server *ss, struct request_setudp *request, struct socket_message *result) {
	int id = request->id;
	struct socket *s = &ss->slot[HASH_ID(id)];
	if (s->type == SOCKET_TYPE_INVALID || s->id !=id) {
		return -1;
	}
	int type = request->address[0];
	if (type != s->protocol) {
		// protocol mismatch
		result->opaque = s->opaque;
		result->id = s->id;
		result->ud = 0;
		result->data = NULL;

		return SOCKET_ERROR;
	}
	if (type == PROTOCOL_UDP) {
		memcpy(s->p.udp_address, request->address, 1+2+4);	// 1 type, 2 port, 4 ipv4
	} else {
		memcpy(s->p.udp_address, request->address, 1+2+16);	// 1 type, 2 port, 16 ipv6
	}
	return -1;
}

// return type
/**
 * @brief deal with the ctrl msg
 * @param[in] sssocket manager
 * @param[in] result result msg from the usr
 */
static int
ctrl_cmd(struct socket_server *ss, struct socket_message *result) {
	int fd = ss->recvctrl_fd;
	// the length of message is one byte, so 256+8 buffer size is enough.
	uint8_t buffer[256];
	uint8_t header[2];
	/*TODO select pipe, why use select???*/
	block_readpipe(fd, header, sizeof(header));
	int type = header[0];
	int len = header[1];
	/*read ctrl msg*/
	block_readpipe(fd, buffer, len);

	// ctrl command only exist in local fd, so don't worry about endian.
	switch (type) {
	case 'S':
	        // Start socket
		return start_socket(ss,(struct request_start *)buffer, result);
	case 'B':
	        //Bind socket
		return bind_socket(ss,(struct request_bind *)buffer, result);
	case 'L':
	        //Listen socket
		return listen_socket(ss,(struct request_listen *)buffer, result);
	case 'K':
	        //Close socket
		return close_socket(ss,(struct request_close *)buffer, result);
	case 'O':
	        //Connect to (Open)
		return open_socket(ss, (struct request_open *)buffer, result);
	case 'X':
	        //Exit
		result->opaque = 0;
		result->id = 0;
		result->ud = 0;
		result->data = NULL;
		return SOCKET_EXIT;
	case 'D':
	        //Send package (high)
		return send_socket(ss, (struct request_send *)buffer, result, PRIORITY_HIGH, NULL);
	case 'P':
	        //Send package (low)
		return send_socket(ss, (struct request_send *)buffer, result, PRIORITY_LOW, NULL);
	case 'A': {
	        //A Send UDP package
		struct request_send_udp * rsu = (struct request_send_udp *)buffer;
		return send_socket(ss, &rsu->send, result, PRIORITY_HIGH, rsu->address);
	}
	case 'C':
	        //set udp address
		return set_udp_address(ss, (struct request_setudp *)buffer, result);
	case 'T':
	        //Set opt
		setopt_socket(ss, (struct request_setopt *)buffer);
		return -1;
	case 'U':
	        //Create UDP socket
		add_udp_socket(ss, (struct request_udp *)buffer);
		return -1;
	default:
		fprintf(stderr, "socket-server: Unknown ctrl %c.\n",type);
		return -1;
	};

	return -1;
}

// return -1 (ignore) when error
/**
 * @brief recive tcp payload from socket
 * @param[in] ss socket manager
 * @param[in] s socket 
 * @param[in] result hold the return msg
 */
static int
forward_message_tcp(struct socket_server *ss, struct socket *s, struct socket_message * result) {
    	/*try to read from the fd*/
        int sz = s->p.size;
	char * buffer = MALLOC(sz);
	int n = (int)read(s->fd, buffer, sz);
	if (n<0) {
		FREE(buffer);
		switch(errno) {
		case EINTR:
			break;
		case EAGAIN:
			fprintf(stderr, "socket-server: EAGAIN capture.\n");
			break;
		default:
			// close when error
			force_close(ss, s, result);
			return SOCKET_ERROR;
		}
		return -1;
	}
	if (n==0) {
		FREE(buffer);
		force_close(ss, s, result);
		return SOCKET_CLOSE;
	}
        
	if (s->type == SOCKET_TYPE_HALFCLOSE) {
		// discard recv data
		FREE(buffer);
		return -1;
	}

	//expand or contract the read buffer by the payload read this time

	if (n == sz) {
		s->p.size *= 2;
	} else if (sz > MIN_READ_BUFFER && n*2 < sz) {
	        /*TODO here maybe error*/
		s->p.size /= 2;
	}
        
        /*tcp payload as the result msg*/
	result->opaque = s->opaque;
	result->id = s->id;
	result->ud = n;
	result->data = buffer;
	return SOCKET_DATA;
}

/**
 * @brief convert the ipv4 or ipv6 to the public address
 * @param[in] protocol protol of the adderess
 * @param[in] sa the public storage of the udp address
 * @param[in] udp_address the udp address wait to be converted
 */
static int
gen_udp_address(int protocol, union sockaddr_all *sa, uint8_t * udp_address) {
	int addrsz = 1;
	udp_address[0] = (uint8_t)protocol;
	/*convert by the protocl */
	if (protocol == PROTOCOL_UDP) {
		memcpy(udp_address+addrsz, &sa->v4.sin_port, sizeof(sa->v4.sin_port));
		addrsz += sizeof(sa->v4.sin_port);
		memcpy(udp_address+addrsz, &sa->v4.sin_addr, sizeof(sa->v4.sin_addr));
		addrsz += sizeof(sa->v4.sin_addr);
	} else {
		memcpy(udp_address+addrsz, &sa->v6.sin6_port, sizeof(sa->v6.sin6_port));
		addrsz += sizeof(sa->v6.sin6_port);
		memcpy(udp_address+addrsz, &sa->v6.sin6_addr, sizeof(sa->v6.sin6_addr));
		addrsz += sizeof(sa->v6.sin6_addr);
	}
	return addrsz;
}

/**
 * @brief recive udp payload from socket
 * @param[in] ss socket manager
 * @param[in] s socket 
 * @param[in] result hold the return msg
 */
static int
forward_message_udp(struct socket_server *ss, struct socket *s, struct socket_message * result) {
	union sockaddr_all sa;
	socklen_t slen = sizeof(sa);
	/*try to recive udp packet with size 65535*/
	int n = recvfrom(s->fd, ss->udpbuffer,MAX_UDP_PACKAGE,0,&sa.s,&slen);
	if (n<0) {
		switch(errno) {
		case EINTR:
		case EAGAIN:
			break;
		default:
			// close when error
			force_close(ss, s, result);
			return SOCKET_ERROR;
		}
		return -1;
	}
	uint8_t * data;
	if (slen == sizeof(sa.v4)) {
		if (s->protocol != PROTOCOL_UDP)
			return -1;
		data = MALLOC(n + 1 + 2 + 4);
		gen_udp_address(PROTOCOL_UDP, &sa, data + n);
	} else {
		if (s->protocol != PROTOCOL_UDPv6)
			return -1;
		data = MALLOC(n + 1 + 2 + 16);
		gen_udp_address(PROTOCOL_UDPv6, &sa, data + n);
	}
	memcpy(data, ss->udpbuffer, n);
        
        /*udp payload as the result msg*/
	result->opaque = s->opaque;
	result->id = s->id;
	result->ud = n;                 /*ud is the payload size of the udp*/
	result->data = (char *)data;

	return SOCKET_UDP;
}

/**
 * @brief register the write evnet and build repoert msg when connect success
 * @param[in] ss socket manager
 * @param[in] s socket active 
 * @param[out] result hold the connect success report msg
 */
static int
report_connect(struct socket_server *ss, struct socket *s, struct socket_message *result) {
	int error;
	/*check socket*/
	socklen_t len = sizeof(error);  
	int code = getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &error, &len);  
	if (code < 0 || error) {  
		force_close(ss,s, result);
		return SOCKET_ERROR;
	} else {
	        /*chang socket status*/
		s->type = SOCKET_TYPE_CONNECTED;

		/*build msg*/
		result->opaque = s->opaque;
		result->id = s->id;
		result->ud = 0;
		/*register the write event*/
		if (send_buffer_empty(s)) {
			sp_write(ss->event_fd, s->fd, s, false);
		}
		union sockaddr_all u;
		socklen_t slen = sizeof(u);
		/*get the peer address*/
		if (getpeername(s->fd, &u.s, &slen) == 0) {
			void * sin_addr = (u.s.sa_family == AF_INET) ? (void*)&u.v4.sin_addr : (void *)&u.v6.sin6_addr;
			if (inet_ntop(u.s.sa_family, sin_addr, ss->buffer, sizeof(ss->buffer))) {
				result->data = ss->buffer;
				return SOCKET_OPEN;
			}
		}
		result->data = NULL;
		return SOCKET_OPEN;
	}
}

// return 0 when failed
//
/**
 * @brief accpect one connect from user and build the socket msg 
 * @param[in] ss master of all socket 
 * @param[in] s  socket accept connect
 * @param[out]  result msg wait to build
 * @return get new clinet fail ? 0 : 1
 *
 */
static int
report_accept(struct socket_server *ss, struct socket *s, struct socket_message *result) {
	union sockaddr_all u;

	/*accpet usr and try to get fd*/
	socklen_t len = sizeof(u);
	int client_fd = accept(s->fd, &u.s, &len);
	if (client_fd < 0) {
		return 0;
	}

	/*get id for this client*/
	int id = reserve_id(ss);
	if (id < 0) {
		close(client_fd);
		return 0;
	}

	/*install  keepalive timer*/
	socket_keepalive(client_fd);
	/*set the clinet as nonblock*/
	sp_nonblocking(client_fd);
	/*alloc new socket manager and register read fd*/
	struct socket *ns = new_fd(ss, id, client_fd, PROTOCOL_TCP, s->opaque, false);
	if (ns == NULL) {
		close(client_fd);
		return 0;
	}

	/*change type to PACCEPT*/
	ns->type = SOCKET_TYPE_PACCEPT;
	/*store the attributes into the result*/
	result->opaque = s->opaque; /*store the module id listen fd belong to*/
	result->id = s->id;         /*listen socket id*/
	result->ud = id;            /*client socket id*/
	result->data = NULL;
        
        /*store the address in string*/
	void * sin_addr = (u.s.sa_family == AF_INET) ? (void*)&u.v4.sin_addr : (void *)&u.v6.sin6_addr;
	int sin_port = ntohs((u.s.sa_family == AF_INET) ? u.v4.sin_port : u.v6.sin6_port);
	char tmp[INET6_ADDRSTRLEN];
	if (inet_ntop(u.s.sa_family, sin_addr, tmp, sizeof(tmp))) {
		snprintf(ss->buffer, sizeof(ss->buffer), "%s:%d", tmp, sin_port);
		result->data = ss->buffer;
	}

	return 1;
}

/**
 * @brief clear evnet closed
 * @param[in] ss socket manager
 * @param[in] result socket wait to delete
 * @param[in] type action type
 */
static inline void 
clear_closed_event(struct socket_server *ss, struct socket_message * result, int type) {
	if (type == SOCKET_CLOSE || type == SOCKET_ERROR) {
		int id = result->id;
		int i;
		for (i=ss->event_index; i<ss->event_n; i++) {
			struct event *e = &ss->ev[i];
			struct socket *s = e->s;
			if (s) {
				if (s->type == SOCKET_TYPE_INVALID && s->id == id) {
				        /*clean the socket*/
					e->s = NULL;
				}
			}
		}
	}
}

// return type
/**
 * @brief wait event by epoll
 * @param[in] ss manager of all socket
 * @param[out] result hold result msg
 * @param[out] more set as 0  when read or write evnet occured
 * @return type
 *
 */
int 
socket_server_poll(struct socket_server *ss, struct socket_message * result, int * more) {
	for (;;) {
	        //1. try to get the ctrl msg from modules by pipe
		if (ss->checkctrl) {
		        /*try to get msg from pipe*/
			if (has_cmd(ss)) {
                                /*run ctrl msg and get the result*/
				int type = ctrl_cmd(ss, result);
				if (type != -1) {
				        /*fail, so we clear the event*/
					clear_closed_event(ss, result, type);
					return type;
				} else
					continue;
			} else {
			        
				ss->checkctrl = 0;
			}
		}
		//2.try to get more event if all the event occured solved
		if (ss->event_index == ss->event_n) {
		        /*wait event occured and record what occured*/
			ss->event_n = sp_wait(ss->event_fd, ss->ev, MAX_EVENT);
			ss->checkctrl = 1;
			if (more) {
				*more = 0;
			}
			ss->event_index = 0;
			if (ss->event_n <= 0) {
				ss->event_n = 0;
				return -1;
			}
		}
		/*3.try to solve one evnet*/
		struct event *e = &ss->ev[ss->event_index++];
		struct socket *s = e->s;
		if (s == NULL) {
			// dispatch pipe message at beginning
			continue;
		}
		//do action for the event's result msg
		switch (s->type) {
		case SOCKET_TYPE_CONNECTING:
		        /*report to module that connect to others success*/
			return report_connect(ss, s, result);
		case SOCKET_TYPE_LISTEN:
		        /*report to module that new connect accept*/
			if (report_accept(ss, s, result)) {
				return SOCKET_ACCEPT;
			} 
			break;
		case SOCKET_TYPE_INVALID:
			fprintf(stderr, "socket-server: invalid socket\n");
			break;
		default:
		        /*read msg from  from socket if read event occured*/
			if (e->read) {
				int type;
				if (s->protocol == PROTOCOL_TCP) {
				        /*forward the tcp msg*/
					type = forward_message_tcp(ss, s, result);
				} else {
				        /*forward the udp msg*/
					type = forward_message_udp(ss, s, result);
					if (type == SOCKET_UDP) {
						// try read again
						--ss->event_index;
						return SOCKET_UDP;
					}
				}
				if (e->write) {
					// Try to dispatch write message next step if write flag set.
					e->read = false;
					--ss->event_index;
				}
				if (type == -1)
					break;
				clear_closed_event(ss, result, type);
				return type;
			}
			/*write payload after write event occured*/
			if (e->write) {
				int type = send_buffer(ss, s, result);
				if (type == -1)
					break;
				clear_closed_event(ss, result, type);
				return type;
			}
			break;
		}
	}
}

/**
 * @brief send a ctrl msg to the module
 * @param[in] msg store
 * @param[in] type msg type
 * @param[in] msg tot len
 */
static void
send_request(struct socket_server *ss, struct request_package *request, char type, int len) {
	request->header[6] = (uint8_t)type;
	request->header[7] = (uint8_t)len;
	for (;;) {
	        /*send ctrl msg to socket by pipe*/
		int n = write(ss->sendctrl_fd, &request->header[6], len+2);
		if (n<0) {
			if (errno != EINTR) {
				fprintf(stderr, "socket-server : send ctrl command error %s.\n", strerror(errno));
			}
			continue;
		}
		assert(n == len+2);
		return;
	}
}

/**
 * @brief build the open request msg
 * @param[in] ss socket manager
 * @param[out] req the msg want to build
 * @param[in] opaque the id of the module produce this msg
 * @param[in] addr address string of the connect machine
 * @param[in] port port of the connect machine
 */
static int
open_request(struct socket_server *ss, struct request_package *req, uintptr_t opaque, const char *addr, int port) {
	int len = strlen(addr);
	if (len + sizeof(req->u.open) > 256) {
		fprintf(stderr, "socket-server : Invalid addr %s.\n",addr);
		return -1;
	}
	/*alloc the socket id*/
	int id = reserve_id(ss);
	if (id < 0)
		return -1;
	req->u.open.opaque = opaque;
	req->u.open.id = id;
	req->u.open.port = port;
	memcpy(req->u.open.host, addr, len);
	req->u.open.host[len] = '\0';

	return len;
}

/**
 * @brief build request connect msg and send to socket manager thread
 * @param[in] ss ssocket manager
 * @param[in] opaque id of the module this msg from
 * @param[in] addr address string of the connect machine
 * @param[in] port port of the connect machine
 */
int 
socket_server_connect(struct socket_server *ss, uintptr_t opaque, const char * addr, int port) {
	struct request_package request;
	/*try to build open request*/
	int len = open_request(ss, &request, opaque, addr, port);
	if (len < 0)
		return -1;
	/*send the request to the socket manager*/
	send_request(ss, &request, 'O', sizeof(request.u.open) + len);
	return request.u.open.id;
}

// return -1 when error
//
int64_t 
socket_server_send(struct socket_server *ss, int id, const void * buffer, int sz) {
	struct socket * s = &ss->slot[HASH_ID(id)];
	if (s->id != id || s->type == SOCKET_TYPE_INVALID) {
		return -1;
	}

	struct request_package request;
	request.u.send.id = id;
	request.u.send.sz = sz;
	request.u.send.buffer = (char *)buffer;

	send_request(ss, &request, 'D', sizeof(request.u.send));
	return s->wb_size;
}

void 
socket_server_send_lowpriority(struct socket_server *ss, int id, const void * buffer, int sz) {
	struct socket * s = &ss->slot[HASH_ID(id)];
	if (s->id != id || s->type == SOCKET_TYPE_INVALID) {
		return;
	}

	struct request_package request;
	request.u.send.id = id;
	request.u.send.sz = sz;
	request.u.send.buffer = (char *)buffer;

	send_request(ss, &request, 'P', sizeof(request.u.send));
}

/**
 * @brief send socket manager exit msg by ctrl pipe
 * @param[in] ss socket_server manager
 * return 
 */
void
socket_server_exit(struct socket_server *ss) {
	struct request_package request;
	send_request(ss, &request, 'X', 0);
}

void
socket_server_close(struct socket_server *ss, uintptr_t opaque, int id) {
	struct request_package request;
	request.u.close.id = id;
	request.u.close.opaque = opaque;
	send_request(ss, &request, 'K', sizeof(request.u.close));
}

// return -1 means failed
// or return AF_INET or AF_INET6
static int
do_bind(const char *host, int port, int protocol, int *family) {
	int fd;
	int status;
	int reuse = 1;
	struct addrinfo ai_hints;
	struct addrinfo *ai_list = NULL;
	char portstr[16];
	if (host == NULL || host[0] == 0) {
		host = "0.0.0.0";	// INADDR_ANY
	}
	sprintf(portstr, "%d", port);
	memset( &ai_hints, 0, sizeof( ai_hints ) );
	ai_hints.ai_family = AF_UNSPEC;
	if (protocol == IPPROTO_TCP) {
		ai_hints.ai_socktype = SOCK_STREAM;
	} else {
		assert(protocol == IPPROTO_UDP);
		ai_hints.ai_socktype = SOCK_DGRAM;
	}
	ai_hints.ai_protocol = protocol;

	status = getaddrinfo( host, portstr, &ai_hints, &ai_list );
	if ( status != 0 ) {
		return -1;
	}
	*family = ai_list->ai_family;
	/*init socket*/
	fd = socket(*family, ai_list->ai_socktype, 0);
	if (fd < 0) {
		goto _failed_fd;
	}
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *)&reuse, sizeof(int))==-1) {
		goto _failed;
	}
	/*bind address*/
	status = bind(fd, (struct sockaddr *)ai_list->ai_addr, ai_list->ai_addrlen);
	if (status != 0)
		goto _failed;

	freeaddrinfo( ai_list );
	return fd;
_failed:
	close(fd);
_failed_fd:
	freeaddrinfo( ai_list );
	return -1;
}

/**
 * @brief listen socket
 * 
 *
 */

static int
do_listen(const char * host, int port, int backlog) {
	int family = 0;
	int listen_fd = do_bind(host, port, IPPROTO_TCP, &family);
	if (listen_fd < 0) {
		return -1;
	}
	if (listen(listen_fd, backlog) == -1) {
		close(listen_fd);
		return -1;
	}
	return listen_fd;
}

int 
socket_server_listen(struct socket_server *ss, uintptr_t opaque, const char * addr, int port, int backlog) {
	int fd = do_listen(addr, port, backlog);
	if (fd < 0) {
		return -1;
	}
	struct request_package request;
	int id = reserve_id(ss);
	if (id < 0) {
		close(fd);
		return id;
	}
	request.u.listen.opaque = opaque;
	request.u.listen.id = id;
	request.u.listen.fd = fd;
	send_request(ss, &request, 'L', sizeof(request.u.listen));
	return id;
}

int
socket_server_bind(struct socket_server *ss, uintptr_t opaque, int fd) {
	struct request_package request;
	int id = reserve_id(ss);
	if (id < 0)
		return -1;
	request.u.bind.opaque = opaque;
	request.u.bind.id = id;
	request.u.bind.fd = fd;
	send_request(ss, &request, 'B', sizeof(request.u.bind));
	return id;
}

void 
socket_server_start(struct socket_server *ss, uintptr_t opaque, int id) {
	struct request_package request;
	request.u.start.id = id;
	request.u.start.opaque = opaque;
	send_request(ss, &request, 'S', sizeof(request.u.start));
}

void
socket_server_nodelay(struct socket_server *ss, int id) {
	struct request_package request;
	request.u.setopt.id = id;
	request.u.setopt.what = TCP_NODELAY;
	request.u.setopt.value = 1;
	send_request(ss, &request, 'T', sizeof(request.u.setopt));
}

void 
socket_server_userobject(struct socket_server *ss, struct socket_object_interface *soi) {
	ss->soi = *soi;
}

// UDP
/**
 * @brief try to send request msg to socket manager to init a udp socket
 * @param[in] opaque module id
 * @param[in] addr string of address 
 * @param[in] port port of the udp socket
 *
 *
 */
int 
socket_server_udp(struct socket_server *ss, uintptr_t opaque, const char * addr, int port) {
	int fd;
	int family;
	/*work as a server*/
	if (port != 0 || addr != NULL) {
		// bind
		fd = do_bind(addr, port, IPPROTO_UDP, &family);
		if (fd < 0) {
			return -1;
		}
	} else {
	/*work as a client*/
		family = AF_INET;
		fd = socket(family, SOCK_DGRAM, 0);
		if (fd < 0) {
			return -1;
		}
	}
	/*set nonblock*/
	sp_nonblocking(fd);

	int id = reserve_id(ss);
	if (id < 0) {
		close(fd);
		return -1;
	}
	/*buid request msg*/
	struct request_package request; 
	request.u.udp.id = id;              
	request.u.udp.fd = fd;
	request.u.udp.opaque = opaque;
	request.u.udp.family = family;

	send_request(ss, &request, 'U', sizeof(request.u.udp));	
	return id;
}

/**
 * @breif transe udp msg to socket thread by pipe
 * @param[in] ss socket manager
 * @param[ib] id id of hte socket
 * @param[in] addr address of the udp 
 * @param[in] buffer pyload of the udp msg
 * @param[in] sz size of the payload in udp msg
 *return  payload size of the socket wait to send
 */
int64_t 
socket_server_udp_send(struct socket_server *ss, int id, const struct socket_udp_address *addr, const void *buffer, int sz) {
	struct socket * s = &ss->slot[HASH_ID(id)];
	if (s->id != id || s->type == SOCKET_TYPE_INVALID) {
		return -1;
	}

	struct request_package request;
	request.u.send_udp.send.id = id;
	request.u.send_udp.send.sz = sz;
	request.u.send_udp.send.buffer = (char *)buffer;

	const uint8_t *udp_address = (const uint8_t *)addr;
	int addrsz;
	switch (udp_address[0]) {
	case PROTOCOL_UDP:
		addrsz = 1+2+4;		// 1 type, 2 port, 4 ipv4
		break;
	case PROTOCOL_UDPv6:
		addrsz = 1+2+16;	// 1 type, 2 port, 16 ipv6
		break;
	default:
		return -1;
	}

	memcpy(request.u.send_udp.address, udp_address, addrsz);	
        
        //trans udp data to socket thread here ( size = payload + udp address size)
	send_request(ss, &request, 'A', sizeof(request.u.send_udp.send)+addrsz);
	return s->wb_size;
}

/**
 * @brief send connect request msg to the socket thread
 * @param[in] ss socket manager handle
 * @param[in] id id of the socket
 * @param[in] addr address  of the udp
 * @param[in] port port of the udp
 *
 */
int
socket_server_udp_connect(struct socket_server *ss, int id, const char * addr, int port) {
	int status;
	struct addrinfo ai_hints;
	struct addrinfo *ai_list = NULL;
	char portstr[16];
	sprintf(portstr, "%d", port);
	/*get the udp daddress */
	memset( &ai_hints, 0, sizeof( ai_hints ) );
	ai_hints.ai_family = AF_UNSPEC;
	ai_hints.ai_socktype = SOCK_DGRAM;
	ai_hints.ai_protocol = IPPROTO_UDP;
    
	status = getaddrinfo(addr, portstr, &ai_hints, &ai_list );
	if ( status != 0 ) {
		return -1;
	}
	struct request_package request;
	request.u.set_udp.id = id;
	int protocol;

	if (ai_list->ai_family == AF_INET) {
		protocol = PROTOCOL_UDP;
	} else if (ai_list->ai_family == AF_INET6) {
		protocol = PROTOCOL_UDPv6;
	} else {
		freeaddrinfo( ai_list );
		return -1;
	}
        //get the public udp address
	int addrsz = gen_udp_address(protocol, (union sockaddr_all *)ai_list->ai_addr, request.u.set_udp.address);

	freeaddrinfo( ai_list );

	/*sed the connect request msg to the socket thread*/

	send_request(ss, &request, 'C', sizeof(request.u.set_udp) - sizeof(request.u.set_udp.address) +addrsz);

	return 0;
}

/**
 * @brief get the address and size of the socket_message address
 * @param[in] ss manager all socket 
 * @param[in] msg socket msg
 * @param[out] addrsz get the size of the udp address
 * @return socket_udp_address
 */
const struct socket_udp_address *
socket_server_udp_address(struct socket_server *ss, struct socket_message *msg, int *addrsz) {
        /*address is in the end of the msg data*/	
        uint8_t * address = (uint8_t *)(msg->data + msg->ud);
	int type = address[0];
	switch(type) {
	        /*set address size by type of udp address*/
	case PROTOCOL_UDP:
		*addrsz = 1+2+4;
		break;
	case PROTOCOL_UDPv6:
		*addrsz = 1+2+16;
		break;
	default:
		return NULL;
	}
	return (const struct socket_udp_address *)address;
}
