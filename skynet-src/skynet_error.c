#include "skynet.h"
#include "skynet_handle.h"
#include "skynet_mq.h"
#include "skynet_server.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define LOG_MESSAGE_SIZE 256

/**
 * @brief send the error msg into the module
 * @param[in] context handle of which module the error occured
 * @param[in] msg
 * @param[in] .. 
 *
 *
 */
void 
skynet_error(struct skynet_context * context, const char *msg, ...) {
	static uint32_t logger = 0;
	if (logger == 0) {
	        /*find the handle of the module my name*/
		logger = skynet_handle_findname("logger");
	}
	if (logger == 0) {
		return;
	}

        /*store the msg*/
	char tmp[LOG_MESSAGE_SIZE];
	char *data = NULL;

	va_list ap;

	va_start(ap,msg);
	int len = vsnprintf(tmp, LOG_MESSAGE_SIZE, msg, ap);
	va_end(ap);
	/*get the data of the msg*/
	if (len < LOG_MESSAGE_SIZE) {
		data = skynet_strdup(tmp);
	} else {
		int max_size = LOG_MESSAGE_SIZE;
		for (;;) {
			max_size *= 2;
			data = skynet_malloc(max_size);
			va_start(ap,msg);
			len = vsnprintf(data, max_size, msg, ap);
			va_end(ap);
			if (len < max_size) {
				break;
			}
			skynet_free(data);
		}
	}

        /*init the msg wait to send*/
	struct skynet_message smsg;
	if (context == NULL) {
		smsg.source = 0;
	} else {
	        /*set source as the module handle*/
		smsg.source = skynet_context_handle(context);
	}
        /*msg in skynet, so session = 0*/
	smsg.session = 0;
	smsg.data = data;
	/*set msg type and len in smsg.sz*/
	smsg.sz = len | (PTYPE_TEXT << HANDLE_REMOTE_SHIFT);
	/*push the msg into queue of the log module*/
	skynet_context_push(logger, &smsg);
}

