#include "skynet.h"
#include "skynet_env.h"

#include <lua.h>
#include <lauxlib.h>

#include <stdlib.h>
#include <assert.h>

/*lua handle manger*/
struct skynet_env {
	int lock;           /*lock */
	lua_State *L;       /*lua handle*/
};

static struct skynet_env *E = NULL;     /*local handle of the lua*/

#define LOCK(q) while (__sync_lock_test_and_set(&(q)->lock,1)) {}   /*get the lock*/
#define UNLOCK(q) __sync_lock_release(&(q)->lock);                  /*release the lock*/

/**
 * @brief get the val of key
 * @param[in] key
 * @return val
 */
const char * 
skynet_getenv(const char *key) {
	LOCK(E)

	lua_State *L = E->L;
	
	lua_getglobal(L, key);
	const char * result = lua_tostring(L, -1);
	lua_pop(L, 1);

	UNLOCK(E)

	return result;
}


/**
 * @brief set key = value
 * @param[in] key key 
 * @param[in] val val
 */
void 
skynet_setenv(const char *key, const char *value) {
	LOCK(E)
	
	lua_State *L = E->L;
	lua_getglobal(L, key);
	assert(lua_isnil(L, -1));
	lua_pop(L,1);
	lua_pushstring(L,value);
	lua_setglobal(L,key);

	UNLOCK(E)
}

/**
 * @brief init the handle of lua
 */
void
skynet_env_init() {
	E = skynet_malloc(sizeof(*E));
	E->lock = 0;
	E->L = luaL_newstate();
}
