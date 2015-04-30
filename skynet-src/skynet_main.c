#include "skynet.h"

#include "skynet_imp.h"
#include "skynet_env.h"
#include "skynet_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <signal.h>
#include <assert.h>

/**
  * @brief ³¢ÊÔ»ñÈ¡key¶ÔÓ¦µÄval£¬Èç¹ûÎ´¶¨Òå
  *    Ôò½«ÆäÉèÖÃÎªopt¶ÔÓ¦µÄÖ²²¢·µ»
  * @param[in] key ¹Ø¼ü×Ö
  * @param[in] opt Öµ
  */
static int
optint(const char *key, int opt) {
	const char * str = skynet_getenv(key);
	if (str == NULL) {
		char tmp[20];
		sprintf(tmp,"%d",opt);
		skynet_setenv(key, tmp);
		return opt;
	}
	return strtol(str, NULL, 10);
}

/*
static int
optboolean(const char *key, int opt) {
	const char * str = skynet_getenv(key);
	if (str == NULL) {
		skynet_setenv(key, opt ? "true" : "false");
		return opt;
	}
	return strcmp(str,"true")==0;
}
*/

/**
  * @brief ³¢ÊÔ»ñÈ¡key¶ÔÓ¦µÄval£¬Èç¹ûÎ´¶¨Òå
  *    Ôò½«ÆäÉèÖÃÎªopt¶ÔÓ¦µÄÖ²²¢·µ»
  * @param[in] key ¹Ø¼ü×Ö
  * @param[in] opt Öµ
  */
static const char *
optstring(const char *key,const char * opt) {
	const char * str = skynet_getenv(key);
	if (str == NULL) {
		if (opt) {
			skynet_setenv(key, opt);
			opt = skynet_getenv(key);
		}
		return opt;
	}
	return str;
}

/**
  * @brief »ñÈ¡»·¾³±äÁ¿
  * @param[in|out] L lua¾ä±ú
  * @note ±éÀú´æ·ÅÔÚÒ»¸ö±íÖĞµÄËùÓĞ»·¾³±äÁ¿
  * ½«ÆäÖĞµÄÌõÄ¿ÉèÖÃÎªÈ«¾Ö»·¾³±äÁ¿
  *
  */
static void
_init_env(lua_State *L) {
	/*Ñ¹Èë³õÊ¼key*/
	lua_pushnil(L);  /* first key */

	/*³¢ÊÔ»ñÈ¡tableµÄÒ»¸ökey-value¶Ô*/
	while (lua_next(L, -2) != 0) {
		/*»ñÈ¡key*/
		int keyt = lua_type(L, -2);
		if (keyt != LUA_TSTRING) {
			fprintf(stderr, "Invalid config table\n");
			exit(1);
		}
		const char * key = lua_tostring(L,-2);
		/*»ñÈ¡value*/
		if (lua_type(L,-1) == LUA_TBOOLEAN) {
			/*boolÖµvalue¸ù¾İÕæ¼Ù½«¼üÉèÖÃÎªtrue»òfalse*/
			int b = lua_toboolean(L,-1);
			skynet_setenv(key,b ? "true" : "false" );
		} else {
			/*·ÇboolÖµÔò½«keyÉèÖÃÎªÖ¸¶¨µÄvalueÖµ*/
			const char * value = lua_tostring(L,-1);
			if (value == NULL) {
				fprintf(stderr, "Invalid config table key = %s\n", key);
				exit(1);
			}
			skynet_setenv(key,value);
		}
		/*pop³övalue*/
		lua_pop(L,1);
	}
	/*°Ñtableµ¯³ö*/
	lua_pop(L,1);
}

/**
  * @brief ÓÃÓÚÆÁ±Î¹ÜµÀÆÆÁÑ
  */
int sigign() {
	struct sigaction sa;
	sa.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &sa, 0);
	return 0;
}

/**
  * »ñÈ¡ÎÄ¼şÖĞ¸÷¸ökey¶ÔÓ¦µÄ»·¾³±äÁ¿
  * TODO Ã»¿´Ì«¶®
  */
static const char * load_config = "\
	local config_name = ...\
	local f = assert(io.open(config_name))\
	local code = assert(f:read \'*a\')\
	local function getenv(name) return assert(os.getenv(name), \'os.getenv() failed: \' .. name) end\
	code = string.gsub(code, \'%$([%w_%d]+)\', getenv)\
	f:close()\
	local result = {}\
	assert(load(code,\'=(load)\',\'t\',result))()\
	return result\
";

/**
  * @brief skynet Ö÷º¯Êı
  * @param[in] argc ²ÎÊı¸öÊı
  * @param[in]argvl[] ¶¯²Î±í
  *
  */
int
main(int argc, char *argv[]) {
	const char * config_file = NULL ;
	if (argc > 1) {
		/*´Ó²ÎÊı»ñÈ¡ÅäÖÃÎÄ¼şÂ·¾¶*/
		config_file = argv[1];
	} else {
		fprintf(stderr, "Need a config file. Please read skynet wiki : https://github.com/cloudwu/skynet/wiki/Config\n"
			"usage: skynet configfilename\n");
		return 1;
	}

	/*³õÊ¼»¯Ïß³Ì¹²Ïí²ÎÊı*/
	skynet_globalinit();
	/*¼ÓÔØlua»·¾³*/
	skynet_env_init();
	/*ÆÁ±Î¹ÜµÀÆÆÁÑ*/
	sigign();
      /*¼ÓÔØluaÄ£¿é*/
	struct skynet_config config;

      /*³õÊ¼lua½»»¥½á¹¹*/
	struct lua_State *L = lua_newstate(skynet_lalloc, NULL);
	 /*¼ÓÔØÖ¸¶¨lua¿â*/
	luaL_openlibs(L);	// link lua lib

	/*¼ÓÔØÅäÖÃÎÄ¼ş*/
	int err = luaL_loadstring(L, load_config);
	assert(err == LUA_OK);
	
	lua_pushstring(L, config_file);

	err = lua_pcall(L, 1, 1, 0);
	if (err) {
		fprintf(stderr,"%s\n",lua_tostring(L,-1));
		lua_close(L);
		return 1;
	}
	_init_env(L);

	config.thread =  optint("thread",8);
	/*³¢ÊÔ»ñÈ¡threadµÄÖµ£¬Ä¬ÈÏÉèÖÃÎª 8*/
	config.module_path = optstring("cpath","./cservice/?.so");
	/*³¢ÊÔ»ñÈ¡harborµÄÖµ£¬Ä¬ÈÏÉèÖÃÎª 1*/
	config.harbor = optint("harbor", 1);
	/*³¢ÊÔ»ñÈ¡bootstrapµÄÖµ£¬Ä¬ÈÏÉèÖÃÎª bootstrap*/
	config.bootstrap = optstring("bootstrap","snlua bootstrap");
	/*³¢ÊÔ»ñÈ¡daemonµÄÖµ*/
	config.daemon = optstring("daemon", NULL);
	/*³¢ÊÔ»ñÈ¡loggerµÄÖµ*/
	config.logger = optstring("logger", NULL);

	lua_close(L);

	/*¿ªÊ¼ÔËĞĞskynet*/  
	skynet_start(&config);

	/*ÇåÀí¹¤×÷*/
	skynet_globalexit();

	return 0;
}
