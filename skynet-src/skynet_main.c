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
 * @brief 查看key对应的val是否存在，不再则设置为opt
 * @param[in] key 键
 * @param[in] opt 数值
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
 * @brief 查看key对应的val是否存在，不再则设置为opt
 * @param[in] key 键
 * @param[in] opt 数值
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
 * @brief lua的表中提取所有键值对并将其设置为全局变量
 * @param[in|out] L lua 句柄
 *
 */
static void
_init_env(lua_State *L) {
	/*push入nil*/
	lua_pushnil(L);  /* first key */

        /*尝试取一个键值对*/
	while (lua_next(L, -2) != 0) {
	        /*提取键*/
		int keyt = lua_type(L, -2);
		if (keyt != LUA_TSTRING) {
			fprintf(stderr, "Invalid config table\n");
			exit(1);
		}
		const char * key = lua_tostring(L,-2);
		/*提取值*/
		if (lua_type(L,-1) == LUA_TBOOLEAN) {
			int b = lua_toboolean(L,-1);
			skynet_setenv(key,b ? "true" : "false" );
		} else {
			const char * value = lua_tostring(L,-1);
			if (value == NULL) {
				fprintf(stderr, "Invalid config table key = %s\n", key);
				exit(1);
			}
			skynet_setenv(key,value);
		}
		lua_pop(L,1);
	}
	lua_pop(L,1);
}

/**
  * @brief 屏蔽管道破裂
  */
int sigign() {
	struct sigaction sa;
	sa.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &sa, 0);
	return 0;
}

/**
  * @brief 读取配置文件，并将配置文件中是环境表量的值进行替换，然后设置全局变量
  * 
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
  * @brief skynet 主函数
  *
  */
int
main(int argc, char *argv[]) {
	const char * config_file = NULL ;
	if (argc > 1) {
	        /*必须制定配置文件路径*/
		config_file = argv[1];
	} else {
		fprintf(stderr, "Need a config file. Please read skynet wiki : https://github.com/cloudwu/skynet/wiki/Config\n"
			"usage: skynet configfilename\n");
		return 1;
	}
        
        /*初始化全局数据*/
	skynet_globalinit();
	
	/*申请lua句柄*/
	skynet_env_init();
        
        /*屏蔽管道破裂*/
	sigign();

	struct skynet_config config;

        /*打开lua状态, 初始lua句柄*/
	struct lua_State *L = lua_newstate(skynet_lalloc, NULL);

	/*打开多个lua库*/
	luaL_openlibs(L);	// link lua lib

        /*加载配置文件*/
	int err = luaL_loadstring(L, load_config);
	assert(err == LUA_OK);
	
	lua_pushstring(L, config_file);

	err = lua_pcall(L, 1, 1, 0);
	if (err) {
		fprintf(stderr,"%s\n",lua_tostring(L,-1));
		lua_close(L);
		return 1;
	}
	/*使用配置文件的key-val设置环境变量*/
	_init_env(L);

        /*配置文件没设置的将设置为默认大小*/
        /*TODO 可以在建立一个默认配置文件来实现，增加灵活性*/
	config.thread =  optint("thread",8);
	config.module_path = optstring("cpath","./cservice/?.so");
	config.harbor = optint("harbor", 1);
	config.bootstrap = optstring("bootstrap","snlua bootstrap");
	config.daemon = optstring("daemon", NULL);
	config.logger = optstring("logger", NULL);

	lua_close(L);

        /*运转keynet*/
	skynet_start(&config);
        
        /*退出skynet*/
	skynet_globalexit();

	return 0;
}
