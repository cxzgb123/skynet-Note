#ifndef poll_socket_epoll_h
#define poll_socket_epoll_h

#include <netdb.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

/**
 * @brief check if the efd if valid
 * @param[in] rfd fd for socket
 * @return valid ? false : true
 */
static bool 
sp_invalid(int efd) {
	return efd == -1;
}

/**
 * @brief create the handle of epoll
 * @return the handle of epoll
 */
static int
sp_create() {
	return epoll_create(1024);
}

/**
 * @brief close the handle of the epoll
 *
 */
static void
sp_release(int efd) {
	close(efd);
}


/**
 * @brief register a epoll read event 
 * @param[in] efd handle of the epoll
 * @param[in] sock fd of socket
 * @param[in] ud private data for event
 */
static int 
sp_add(int efd, int sock, void *ud) {
	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.ptr = ud;
	if (epoll_ctl(efd, EPOLL_CTL_ADD, sock, &ev) == -1) {
		return 1;
	}
	return 0;
}

/**
 * @brief delete the event in epoll
 *
 */
static void 
sp_del(int efd, int sock) {
	epoll_ctl(efd, EPOLL_CTL_DEL, sock , NULL);
}


/**
 * @brief register event of write
 * @param[in]  efd handle of the epoll
 * @param[in]  ud private data of epoll
 * @param[in]  enable enable the epollout
 *
 */
static void 
sp_write(int efd, int sock, void *ud, bool enable) {
	struct epoll_event ev;
	/*just try to register write action, but read event always registered*/
	ev.events = EPOLLIN | (enable ? EPOLLOUT : 0);
	ev.data.ptr = ud;
	/*change event mod*/
	epoll_ctl(efd, EPOLL_CTL_MOD, sock, &ev);
}

/**
 * @brief wait the event active and mark all evnet occured
 * @param[in] efd handle of the epoll
 * @param[in] e get the evnet actived
 * @reutn 
 *
 */
static int 
sp_wait(int efd, struct event *e, int max) {
	struct epoll_event ev[max];
	int n = epoll_wait(efd , ev, max, -1);
	int i;
	for (i=0;i<n;i++) {
		e[i].s = ev[i].data.ptr;
		unsigned flag = ev[i].events;
		e[i].write = (flag & EPOLLOUT) != 0;
		e[i].read = (flag & EPOLLIN) != 0;
	}
        /*return the tot event actived*/
	return n;
}

/**
 * @brief set the fd in nonblock
 *
 *
 */
static void
sp_nonblocking(int fd) {
	int flag = fcntl(fd, F_GETFL, 0);
	if ( -1 == flag ) {
		return;
	}

	fcntl(fd, F_SETFL, flag | O_NONBLOCK);
}

#endif
