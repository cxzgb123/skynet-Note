#ifndef SKYNET_RWLOCK_H
#define SKYNET_RWLOCK_H

/*@brief 管理读写锁
  *
  */
struct rwlock {
      /*读写记数*/
	int write;
	int read;
};

/**
  *@brief 初始化读写锁
  *@param[out] lock 待初始化的读写锁
  *
  */
static inline void
rwlock_init(struct rwlock *lock) {
      /*初始化读写计数*/
	lock->write = 0;
	lock->read = 0;
}

/**
  * @brief 获取读锁
  * @param[in|out] 读写锁管理
  */
static inline void
rwlock_rlock(struct rwlock *lock) {
	for (;;) {
		/*等待写释放*/
		
		/*TODO 这里感觉只是一个优化*/
		while(lock->write) {                             
			/*内存障壁*/
			__sync_synchronize();
		}
		/*尝试获取读标志*/
		__sync_add_and_fetch(&lock->read,1);
		/*检查是否有写用户*/
		if (lock->write) {
			/*读锁获取失败，还原读计数*/
			__sync_sub_and_fetch(&lock->read,1);
		} else {
			/*读锁获取成功,跳出阻塞*/
			break;
		}
	}
}

/*
 * @brief 获取写锁
 * @param[in|out] lock 读写锁
 *
 */
static inline void
rwlock_wlock(struct rwlock *lock) {
	/*原子设置写标志*/
	while (__sync_lock_test_and_set(&lock->write,1)) {}
	/*等待所有读方退出读锁持有区域*/
	while(lock->read) {
		/*内存障壁*/
		__sync_synchronize();
	}
}

/**
 * @brief 释放写锁
 * @param[in|out] lock 读写锁
 */
static inline void
rwlock_wunlock(struct rwlock *lock) {
      /*将写标志设为0*/
	__sync_lock_release(&lock->write);
}

/**
  * @brief 释放读锁
  * @param[in|out] lock 读写锁
  */
static inline void
rwlock_runlock(struct rwlock *lock) {
	/*将读标志原子减1*/
	__sync_sub_and_fetch(&lock->read,1);
}

#endif
