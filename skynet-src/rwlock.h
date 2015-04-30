#ifndef SKYNET_RWLOCK_H
#define SKYNET_RWLOCK_H

/*@brief �����д��
  *
  */
struct rwlock {
      /*��д����*/
	int write;
	int read;
};

/**
  *@brief ��ʼ����д��
  *@param[out] lock ����ʼ���Ķ�д��
  *
  */
static inline void
rwlock_init(struct rwlock *lock) {
      /*��ʼ����д����*/
	lock->write = 0;
	lock->read = 0;
}

/**
  * @brief ��ȡ����
  * @param[in|out] ��д������
  */
static inline void
rwlock_rlock(struct rwlock *lock) {
	for (;;) {
		/*�ȴ�д�ͷ�*/
		
		/*TODO ����о�ֻ��һ���Ż�*/
		while(lock->write) {                             
			/*�ڴ��ϱ�*/
			__sync_synchronize();
		}
		/*���Ի�ȡ����־*/
		__sync_add_and_fetch(&lock->read,1);
		/*����Ƿ���д�û�*/
		if (lock->write) {
			/*������ȡʧ�ܣ���ԭ������*/
			__sync_sub_and_fetch(&lock->read,1);
		} else {
			/*������ȡ�ɹ�,��������*/
			break;
		}
	}
}

/*
 * @brief ��ȡд��
 * @param[in|out] lock ��д��
 *
 */
static inline void
rwlock_wlock(struct rwlock *lock) {
	/*ԭ������д��־*/
	while (__sync_lock_test_and_set(&lock->write,1)) {}
	/*�ȴ����ж����˳�������������*/
	while(lock->read) {
		/*�ڴ��ϱ�*/
		__sync_synchronize();
	}
}

/**
 * @brief �ͷ�д��
 * @param[in|out] lock ��д��
 */
static inline void
rwlock_wunlock(struct rwlock *lock) {
      /*��д��־��Ϊ0*/
	__sync_lock_release(&lock->write);
}

/**
  * @brief �ͷŶ���
  * @param[in|out] lock ��д��
  */
static inline void
rwlock_runlock(struct rwlock *lock) {
	/*������־ԭ�Ӽ�1*/
	__sync_sub_and_fetch(&lock->read,1);
}

#endif
