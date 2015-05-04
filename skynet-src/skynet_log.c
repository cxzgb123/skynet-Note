#include "skynet_log.h"
#include "skynet_timer.h"
#include "skynet.h"
#include "skynet_socket.h"
#include <string.h>
#include <time.h>

/**
 * @brief open the logfile
 * @param[in] ctx modules open logfile
 * @param[in] handle handle logfile = logpath/str(handle).log
 * @reutn return the handle of the file
 *
 */
FILE * 
skynet_log_open(struct skynet_context * ctx, uint32_t handle) {
	const char * logpath = skynet_getenv("logpath");
	if (logpath == NULL)
		return NULL;
	size_t sz = strlen(logpath);
	char tmp[sz + 16];
	sprintf(tmp, "%s/%08x.log", logpath, handle);
	FILE *f = fopen(tmp, "ab");
	if (f) {
		uint32_t starttime = skynet_gettime_fixsec();
		uint32_t currenttime = skynet_gettime();
		time_t ti = starttime + currenttime/100;
		skynet_error(ctx, "Open log file %s", tmp);
		fprintf(f, "open time: %u %s", currenttime, ctime(&ti));
		fflush(f);
	} else {
		skynet_error(ctx, "Open log file %s fail", tmp);
	}
	return f;
}

/**
 * @brief close the logfile
 * @param[in] ctx handle of the module
 * @param[in] f logfile handle 
 * @param[in] handle handle of the module
 */
void
skynet_log_close(struct skynet_context * ctx, FILE *f, uint32_t handle) {
	skynet_error(ctx, "Close log file :%08x", handle);
	fprintf(f, "close time: %u\n", skynet_gettime());
	fclose(f);
}

/**
 * @brief write info into logfile
 * @param[in] buffer store the msg
 * @param[in] sz size of the msg
 *
 */
static void
log_blob(FILE *f, void * buffer, size_t sz) {
	size_t i;
	uint8_t * buf = buffer;
	for (i=0;i!=sz;i++) {
		fprintf(f, "%02x", buf[i]);
	}
}

/**
 * @brief output socket msg
 * @param[in] message socket message
 * @param[in] size of message
 *
 *
 */
static void
log_socket(FILE * f, struct skynet_socket_message * message, size_t sz) {
	fprintf(f, "[socket] %d %d %d ", message->type, message->id, message->ud);

	if (message->buffer == NULL) {
	/*here!!!! if mseeage->buffer == NULL, 
	         * buffer must at the end of the message*/
		const char *buffer = (const char *)(message + 1);
		sz -= sizeof(*message);
		const char * eol = memchr(buffer, '\0', sz);
		if (eol) {
			sz = eol - buffer;
		}
		fprintf(f, "[%*s]", (int)sz, (const char *)buffer);
	} else {
	        /*msg int the message->buffer*/
		sz = message->ud;
		log_blob(f, message->buffer, sz);
	}
	fprintf(f, "\n");
	/*refresh data*/
	fflush(f);
}

/**
 * @brief output log
 * @param[in] source where the message from
 * @param[in] type type of the message
 * @param[in] session session id of the message
 * @param[in] buffer buffer store the mesage
 * @param[in] sz size of the buffer
 */
void 
skynet_log_output(FILE *f, uint32_t source, int type, int session, void * buffer, size_t sz) {
	if (type == PTYPE_SOCKET) {
		log_socket(f, buffer, sz);
	} else {
		uint32_t ti = skynet_gettime();
		fprintf(f, ":%08x %d %d %u ", source, type, session, ti);
		log_blob(f, buffer, sz);
		fprintf(f,"\n");
		fflush(f);
	}
}
