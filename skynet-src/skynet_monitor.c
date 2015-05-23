#include "skynet.h"

#include "skynet_monitor.h"
#include "skynet_server.h"
#include "skynet.h"

#include <stdlib.h>
#include <string.h>

/**
 * @brief monitor for module
 *
 */
struct skynet_monitor {
	int version;            /*curr version*/                        
	int check_version;      /*old version*/
	uint32_t source;        /*source for module*/
	uint32_t destination;   /*dst for module, used to check if the module in work*/
};

/**
 * @brief new monitor
 * @return monitor handle
 *
 */
struct skynet_monitor * 
skynet_monitor_new() {
	struct skynet_monitor * ret = skynet_malloc(sizeof(*ret));
	memset(ret, 0, sizeof(*ret));
	return ret;
}

/**
 * @brief delete the monitor
 * @param[in] sm handle of the monitor
 *
 */
void 
skynet_monitor_delete(struct skynet_monitor *sm) {
	skynet_free(sm);
}

/**
 * @brief record status for monitor 
 * @param[in] sm handle of the monitor
 * @param[in] source source of the module
 * @param[in] destination dst for the module
 *
 */
void 
skynet_monitor_trigger(struct skynet_monitor *sm, uint32_t source, uint32_t destination) {
	sm->source = source;
	sm->destination = destination;
	__sync_fetch_and_add(&sm->version , 1);
}

/**
 * @brief watchdog for check if the module unregister
 * @param[in] sm montior fot the module
 */
void 
skynet_monitor_check(struct skynet_monitor *sm) {
	if (sm->version == sm->check_version) {     /*module in work, so we can check if the module uninstalled when we try to use it*/
		if (sm->destination) {
			skynet_context_endless(sm->destination);    /*check if the module exit, and try to mark as exit*/
			skynet_error(NULL, "A message from [ :%08x ] to [ :%08x ] maybe in an endless loop (version = %d)", sm->source , sm->destination, sm->version);
		}
	} else {                                    /*module maybe not in work, just update version*/
		sm->check_version = sm->version;
	}
}
