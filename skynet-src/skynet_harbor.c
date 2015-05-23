#include "skynet.h"
#include "skynet_harbor.h"
#include "skynet_server.h"

#include <string.h>
#include <stdio.h>
#include <assert.h>

static struct skynet_context * REMOTE = 0;
static unsigned int HARBOR = ~0;

/**
 * @brief send msg to the harbor
 * @param[in] rmsg msg to the remote
 * @param[in] source source of the  msg
 * @param[in] session session id
 */
void 
skynet_harbor_send(struct remote_message *rmsg, uint32_t source, int session) {
        /**get msg type*/
	int type = rmsg->sz >> HANDLE_REMOTE_SHIFT;
	rmsg->sz &= HANDLE_MASK;
	assert(type != PTYPE_SYSTEM && type != PTYPE_HARBOR && REMOTE);
	/*try to send msg*/
	skynet_context_send(REMOTE, rmsg, sizeof(*rmsg) , source, type , session);
}

/**
 * @brief check if the handle is remote
 * @return  remote ? true : false
 *
 */
int 
skynet_harbor_message_isremote(uint32_t handle) {
	assert(HARBOR != ~0);
	int h = (handle & ~HANDLE_MASK);
	return h != HARBOR && h !=0;
}

/**
  * @brief record global harborID
  * @param[in] harbor ID
  */
void
skynet_harbor_init(int harbor) {
      /*harbit id in high 8bit*/
	HARBOR = (unsigned int)harbor << HANDLE_REMOTE_SHIFT;
}

/**
 * @brief store the pointer to remote module in REMOTE
 * @param[in] ctx module handle
 *
 */
void
skynet_harbor_start(void *ctx) {
	// the HARBOR must be reserved to ensure the pointer is valid.
	// It will be released at last by calling skynet_harbor_exit
	skynet_context_reserve(ctx);
	REMOTE = ctx;
}

/**
 * @brief destory the module for remote
 *
 */
void
skynet_harbor_exit() {
	struct skynet_context * ctx = REMOTE;
	REMOTE= NULL;
	if (ctx) {
		skynet_context_release(ctx);
	}
}
