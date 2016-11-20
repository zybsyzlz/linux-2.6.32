#ifndef __ASM_SPINLOCK_TYPES_H
#define __ASM_SPINLOCK_TYPES_H

#ifndef __LINUX_SPINLOCK_TYPES_H
# error "please don't include this file directly"
#endif

typedef struct {
	volatile unsigned int lock;
} raw_spinlock_t;

#define __RAW_SPIN_LOCK_UNLOCKED	{ 0 }

/*读写自旋锁数据结构*/
typedef struct {
	/*lock字段可以分为两部分：
	 * 24位计数器[23: 0]：表示受保护数据并发地进行读
	 *             操作的内核控制路径数,这个计数器的
	 *             二进制补码存放在这个字段
	 * 未锁标志[24]：表示是否持有自旋锁标记
	*/
	volatile unsigned int lock;
} raw_rwlock_t;

#define __RAW_RW_LOCK_UNLOCKED		{ 0 }

#endif
