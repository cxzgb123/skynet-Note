#include "skynet.h"

#include "skynet_module.h"

#include <assert.h>
#include <string.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#define MAX_MODULE_TYPE 32

/***
 * @brief manager of all module
 */
struct modules {
	int count;                              /*nums module loaded*/
	int lock;                               /*lock */
	const char * path;                      /*待装载的模块目录*/
	struct skynet_module m[MAX_MODULE_TYPE];/*存放装载的模块*/
};

static struct modules * M = NULL;               /*实例化一个模块管理结构*/

/**
 * @brief 从制定文件将模块信息进行加载
 * @param[out] 模块指针
 * @param[in]  模块名称
 * @note 这里实际是尝试在path指定的所有目录尝试查找并加载该库
 * @return 
 */
static void *
_try_open(struct modules *m, const char * name) {
	const char *l;
	const char * path = m->path;
	size_t path_size = strlen(path);
	size_t name_size = strlen(name);

	int sz = path_size + name_size;
	//search path
	void * dl = NULL;
	char tmp[sz];
	do
	{
		memset(tmp,0,sz);
		/*过滤开始的 ; */
		while (*path == ';') path++;
		if (*path == '\0') break;

		/*尝试以;结尾切割一个字串*/
		l = strchr(path, ';');
		if (l == NULL) l = path + strlen(path);
		int len = l - path;
		int i;
		for (i=0;path[i]!='?' && i < len ;i++) {
			tmp[i] = path[i];
		}
		memcpy(tmp+i,name,name_size);
		if (path[i] == '?') {
			strncpy(tmp+i+name_size,path+i+1,len - i - 1);
		} else {
			fprintf(stderr,"Invalid C service path\n");
			exit(1);
		}
		/*立即解析该库， 且其后的函数能使用该库的函数*/
		dl = dlopen(tmp, RTLD_NOW | RTLD_GLOBAL);
		path = l;
	}while(dl == NULL);

	if (dl == NULL) {
		fprintf(stderr, "try open %s failed : %s\n",name,dlerror());
	}
        
        /*返回被加载的库*/
	return dl;
}

/**
 * @brief 检查是否有同名模块
 * @return 找到同名模块? 模块指针 ： NULL
 */
static struct skynet_module * 
_query(const char * name) {
	int i;
	for (i=0;i<M->count;i++) {
		if (strcmp(M->m[i].name,name)==0) {
			return &M->m[i];
		}
	}
	return NULL;
}

/**
 * @brief 尝试将库中的几个固定回调函数加载
 * @param[in] mod 模块库指针
 * @return 是否成功加载 init 函数 ? true : false
 */ 
static int
_open_sym(struct skynet_module *mod) {
	size_t name_size = strlen(mod->name);
	char tmp[name_size + 9]; // create/init/release/signal , longest name is release (7)
	memcpy(tmp, mod->name, name_size);
	/*加载create 回调*/
	strcpy(tmp+name_size, "_create");
	mod->create = dlsym(mod->module, tmp);
	/*加载初始化模块回调*/
	strcpy(tmp+name_size, "_init");
	mod->init = dlsym(mod->module, tmp);
	/*加载模块释放回调*/
	strcpy(tmp+name_size, "_release");
	mod->release = dlsym(mod->module, tmp);
	/*加载信号处理回调*/
	strcpy(tmp+name_size, "_signal");
	mod->signal = dlsym(mod->module, tmp);
	return mod->init == NULL;
}

/**
 * @brief 尝试加载一个制定的模块
 * @param[in] name 模块名称
 * @return 成功 ? 模块的指针:NULL
 */
struct skynet_module * 
skynet_module_query(const char * name) {
        /*查看该模块是否已经加载*/
	struct skynet_module * result = _query(name);
	if (result)
		return result;
        /*获取锁*/
	while(__sync_lock_test_and_set(&M->lock,1)) {}
        
        /*再次检查*/
	result = _query(name); // double check
        
        /*不存在同名模块，且模块存储器未超过容量*/
	if (result == NULL && M->count < MAX_MODULE_TYPE) {
	        /*获取空位索引*/
		int index = M->count;
                
                /*加载库*/
		void * dl = _try_open(M,name);
		if (dl) {
		        /*将库放入槽位*/
			M->m[index].name = name;
			M->m[index].module = dl;
			/*加载模块的回调函数*/
			if (_open_sym(&M->m[index]) == 0) {
				M->m[index].name = skynet_strdup(name);
				M->count ++;
				result = &M->m[index];
			}
		}
	}
	__sync_lock_release(&M->lock);

	return result;
}

void 
skynet_module_insert(struct skynet_module *mod) {
	while(__sync_lock_test_and_set(&M->lock,1)) {}

	struct skynet_module * m = _query(mod->name);
	assert(m == NULL && M->count < MAX_MODULE_TYPE);
	int index = M->count;
	M->m[index] = *mod;
	++M->count;
	__sync_lock_release(&M->lock);
}

void * 
skynet_module_instance_create(struct skynet_module *m) {
	if (m->create) {
		return m->create();
	} else {
		return (void *)(intptr_t)(~0);
	}
}

int
skynet_module_instance_init(struct skynet_module *m, void * inst, struct skynet_context *ctx, const char * parm) {
	return m->init(inst, ctx, parm);
}

void 
skynet_module_instance_release(struct skynet_module *m, void *inst) {
	if (m->release) {
		m->release(inst);
	}
}

void
skynet_module_instance_signal(struct skynet_module *m, void *inst, int signal) {
	if (m->signal) {
		m->signal(inst, signal);
	}
}

void 
skynet_module_init(const char *path) {
	struct modules *m = skynet_malloc(sizeof(*m));
	m->count = 0;
	m->path = skynet_strdup(path);
	m->lock = 0;

	M = m;
}
