/*
 * High memory handling common code and variables.
 *
 * (C) 1999 Andrea Arcangeli, SuSE GmbH, andrea@suse.de
 *          Gerhard Wichert, Siemens AG, Gerhard.Wichert@pdb.siemens.de
 *
 *
 * Redesigned the x86 32-bit VM architecture to deal with
 * 64-bit physical space. With current x86 CPUs this
 * means up to 64 Gigabytes physical RAM.
 *
 * Rewrote high memory support to move the page cache into
 * high memory. Implemented permanent (schedulable) kmaps
 * based on Linus' idea.
 *
 * Copyright (C) 1999 Ingo Molnar <mingo@redhat.com>
 */

#include <linux/mm.h>
#include <linux/module.h>
#include <linux/swap.h>
#include <linux/bio.h>
#include <linux/pagemap.h>
#include <linux/mempool.h>
#include <linux/blkdev.h>
#include <linux/init.h>
#include <linux/hash.h>
#include <linux/highmem.h>
#include <asm/tlbflush.h>

/*
 * Virtual_count is not a pure "count".
 *  0 means that it is not mapped, and has not been mapped
 *    since a TLB flush - it is usable.
 *  1 means that there are no users, but it has been mapped
 *    since the last TLB flush - so we can't use it.
 *  n means that there are (n-1) current users of it.
 */
#ifdef CONFIG_HIGHMEM

unsigned long totalhigh_pages __read_mostly;
EXPORT_SYMBOL(totalhigh_pages);

/**
 * nr_free_highpages:返回系统中所有的高端内存物理页框
 */
unsigned int nr_free_highpages (void)
{
	pg_data_t *pgdat;
	unsigned int pages = 0;

	for_each_online_pgdat(pgdat) {
		pages += zone_page_state(&pgdat->node_zones[ZONE_HIGHMEM],
			NR_FREE_PAGES);
		if (zone_movable_is_highmem())
			pages += zone_page_state(
					&pgdat->node_zones[ZONE_MOVABLE],
					NR_FREE_PAGES);
	}

	return pages;
}
/**
 * 跟踪高端内存页框的永久性映射情况,数组每
 * 一项跟跟踪高端内存页框的映射次数
 * 最多可以永久性映射的高端内存页框数为LAST_PKMAP
 */
static int pkmap_count[LAST_PKMAP];
/*保存上次被映射的高端内存页表项索引*/
static unsigned int last_pkmap_nr;
static  __cacheline_aligned_in_smp DEFINE_SPINLOCK(kmap_lock);

/**
 * 永久性高端内存映射的页表项数组，
 * 每一项保存一个映射的页框的线性地址
 */
pte_t * pkmap_page_table;

static DECLARE_WAIT_QUEUE_HEAD(pkmap_map_wait);

/*
 * Most architectures have no use for kmap_high_get(), so let's abstract
 * the disabling of IRQ out of the locking in that case to save on a
 * potential useless overhead.
 */
#ifdef ARCH_NEEDS_KMAP_HIGH_GET
#define lock_kmap()             spin_lock_irq(&kmap_lock)
#define unlock_kmap()           spin_unlock_irq(&kmap_lock)
#define lock_kmap_any(flags)    spin_lock_irqsave(&kmap_lock, flags)
#define unlock_kmap_any(flags)  spin_unlock_irqrestore(&kmap_lock, flags)
#else
#define lock_kmap()             spin_lock(&kmap_lock)
#define unlock_kmap()           spin_unlock(&kmap_lock)
#define lock_kmap_any(flags)    \
		do { spin_lock(&kmap_lock); (void)(flags); } while (0)
#define unlock_kmap_any(flags)  \
		do { spin_unlock(&kmap_lock); (void)(flags); } while (0)
#endif

/**
 * flush_all_zero_pkmaps:刷新高端内存页框的页表项映射的页框
 */
static void flush_all_zero_pkmaps(void)
{
	int i;
	int need_flush = 0;

	flush_cache_kmaps();

	for (i = 0; i < LAST_PKMAP; i++) {
		struct page *page;

		if (pkmap_count[i] != 1)
			continue;
		pkmap_count[i] = 0;

		/* sanity check */
		BUG_ON(pte_none(pkmap_page_table[i]));

		/*
		 * Don't need an atomic fetch-and-clear op here;
		 * no-one has the page mapped, and cannot get at
		 * its virtual address (and hence PTE) without first
		 * getting the kmap_lock (which is held here).
		 * So no dangers, even with speculative execution.
		 */
		page = pte_page(pkmap_page_table[i]);
		pte_clear(&init_mm, (unsigned long)page_address(page),
			  &pkmap_page_table[i]);

		set_page_address(page, NULL);
		need_flush = 1;
	}
	if (need_flush)
		flush_tlb_kernel_range(PKMAP_ADDR(0), PKMAP_ADDR(LAST_PKMAP));
}

/**
 * kmap_flush_unused:刷新高端内存的页表项映射的页框
 */
void kmap_flush_unused(void)
{
	lock_kmap();
	flush_all_zero_pkmaps();
	unlock_kmap();
}

/**
 * map_new_virtual:将高端内存页框映射到内核线性地址空间，
 * @ page: 待映射的高端内存物理页框
 * @ return :并返回映射后的内核虚拟地址
 */
static inline unsigned long map_new_virtual(struct page *page)
{
	unsigned long vaddr;
	int count;

start:
	count = LAST_PKMAP;
	/* Find an empty entry */
	for (;;) {
		/*找到可以映射的页表项索引*/
		last_pkmap_nr = (last_pkmap_nr + 1) & LAST_PKMAP_MASK;
		if (!last_pkmap_nr) {
			/*循环映射*/
			flush_all_zero_pkmaps();
			count = LAST_PKMAP;
		}
		if (!pkmap_count[last_pkmap_nr])
			break;	/* Found a usable entry */
		if (--count)
			continue;

		/*count = 0,则此时并没有空闲的内核为高端内存映射保留
		 * 的内核线性地址空间，此时，将会导致请求映射的进程
		 * 进入睡眠，直到有空闲的线性地址空间出现
		 */
		{
			DECLARE_WAITQUEUE(wait, current);

			__set_current_state(TASK_UNINTERRUPTIBLE);
			add_wait_queue(&pkmap_map_wait, &wait);
			unlock_kmap();
			schedule();
			remove_wait_queue(&pkmap_map_wait, &wait);
			lock_kmap();

			/* Somebody else might have mapped it while we slept */
			if (page_address(page))
				return (unsigned long)page_address(page);

			/*重新尝试*/
			goto start;
		}
	}
	/*计算出last_pkmap_nr页号对应页框的线性地址,
	 * 也就是,page高端内存页框映射到内核空间的
	 * 线性地址*/
	vaddr = PKMAP_ADDR(last_pkmap_nr);
	/*将vaddr线性地址存入pkmap_page_tables页表项数组中*/
	set_pte_at(&init_mm, vaddr,
		   &(pkmap_page_table[last_pkmap_nr]), mk_pte(page, kmap_prot));

	/*增加last_pkmap_nr页框的映射计数,但是此事该映射的页框还是空闲的*/
	pkmap_count[last_pkmap_nr] = 1;
	/*设置该高端内存页框的线性地址*/
	set_page_address(page, (void *)vaddr);

	return vaddr;
}

/**
 * kmap_high :将一个高端内存页框映射到内核线性地址空间 
 * @page: 待映射的高端内存页框
 * @return:返回映射后的内核线性地址
 */
void *kmap_high(struct page *page)
{
	unsigned long vaddr;

	/*
	 * For highmem pages, we can't trust "virtual" until
	 * after we have the lock.
	 */
	lock_kmap();
	/*判断该页框是否已经映射到内核空间？*/
	vaddr = (unsigned long)page_address(page);
	if (!vaddr)
		vaddr = map_new_virtual(page);
	/*增加该页框的映射计数*/
	pkmap_count[PKMAP_NR(vaddr)]++;
	BUG_ON(pkmap_count[PKMAP_NR(vaddr)] < 2);
	unlock_kmap();
	return (void*) vaddr;
}

EXPORT_SYMBOL(kmap_high);

#ifdef ARCH_NEEDS_KMAP_HIGH_GET
/**
 * kmap_high_get:返回高端内存页框page的内核线性地址
 */
void *kmap_high_get(struct page *page)
{
	unsigned long vaddr, flags;

	lock_kmap_any(flags);
	vaddr = (unsigned long)page_address(page);
	if (vaddr) {
		BUG_ON(pkmap_count[PKMAP_NR(vaddr)] < 1);
		/*增加内核线性地址的引用计数*/
		pkmap_count[PKMAP_NR(vaddr)]++;
	}
	unlock_kmap_any(flags);
	return (void*) vaddr;
}
#endif

/**
 * kunmap_high :撤销高端内存页框page的永久性映射
 * @page: &struct page to unmap
 */
void kunmap_high(struct page *page)
{
	unsigned long vaddr;
	unsigned long nr;
	unsigned long flags;
	int need_wakeup;

	lock_kmap_any(flags);
	vaddr = (unsigned long)page_address(page);
	BUG_ON(!vaddr);
	/*高端内存页表项索引*/
	nr = PKMAP_NR(vaddr);

	/*
	 * A count must never go down to zero
	 * without a TLB flush!
	 */
	need_wakeup = 0;
	switch (--pkmap_count[nr]) {
	case 0:
		BUG();
	case 1:
		/*映射次数减1后变为1,表示没有其他进程映射该页框了
		 * 因此唤醒等待队列中的进程
		 */
		need_wakeup = waitqueue_active(&pkmap_map_wait);
	}
	unlock_kmap_any(flags);

	/* do wake-up, if needed, race-free outside of the spin lock */
	if (need_wakeup)
		wake_up(&pkmap_map_wait);
}

EXPORT_SYMBOL(kunmap_high);
#endif

#if defined(HASHED_PAGE_VIRTUAL)

#define PA_HASH_ORDER	7

/*
 *管理高端内存页框结构 
 */
struct page_address_map {
	struct page *page;    /*高端内存页框*/
	void *virtual;        /*高端内存页框所映射的内核线性地址*/
	struct list_head list; /*链表结点结构*/
};

/*
 * 管理空闲的 page_address_maps链表.
 */
static struct list_head page_address_pool;	/* freelist */
static spinlock_t pool_lock;			/* protects page_address_pool */

/*
 * Hash table bucket 高端内存映射散列表,跟踪高端内存页框的永久性映射页框
 */
static struct page_address_slot {
	struct list_head lh;			/* List of page_address_maps 保存映射的物理页框*/
	spinlock_t lock;
} ____cacheline_aligned_in_smp page_address_htable[1<<PA_HASH_ORDER];

/* page_address_slot:返回高端内存永久性映射的散列表项*/
static struct page_address_slot *page_slot(struct page *page)
{
	return &page_address_htable[hash_ptr(page, PA_HASH_ORDER)];
}

/* page_address:返回page页框对应的线性地址*/
void *page_address(struct page *page)
{
	unsigned long flags;
	void *ret;
	struct page_address_slot *pas;

	if (!PageHighMem(page))
		return lowmem_page_address(page);

	/*如果是高端内存，则查看高端内存是否被映射到内核
	 *的线性空间中,如果被映射，则返回对应的映射后的虚拟地址
	 * 否则，返回NULL
	*/
	pas = page_slot(page);
	ret = NULL;
	spin_lock_irqsave(&pas->lock, flags);
	if (!list_empty(&pas->lh)) {
		struct page_address_map *pam;

		list_for_each_entry(pam, &pas->lh, list) {
			if (pam->page == page) {
				ret = pam->virtual;
				goto done;
			}
		}
	}
done:
	spin_unlock_irqrestore(&pas->lock, flags);
	return ret;
}

EXPORT_SYMBOL(page_address);

/**
 * set_page_address:设置高端内存page页框的线性地址 
 * @page: 高端内存页框
 * @virtual:page页框被映射后的线性地址
 */
void set_page_address(struct page *page, void *virtual)
{
	unsigned long flags;
	struct page_address_slot *pas;
	struct page_address_map *pam;

	BUG_ON(!PageHighMem(page));

	/*计算出在跟踪高端内存映射中的hash项*/
	pas = page_slot(page);
	if (virtual) {		/* Add */
		BUG_ON(list_empty(&page_address_pool));

		/*从page_address_pool空闲链表中
		 * 获得一个pam节点*/
		spin_lock_irqsave(&pool_lock, flags);
		pam = list_entry(page_address_pool.next,
				struct page_address_map, list);
		list_del(&pam->list);
		spin_unlock_irqrestore(&pool_lock, flags);

		/*初始化pam节点*/
		pam->page = page;
		pam->virtual = virtual;

		spin_lock_irqsave(&pas->lock, flags);
		/*加入到跟踪永久性高端内存映射的hash链表中*/
		list_add_tail(&pam->list, &pas->lh);
		spin_unlock_irqrestore(&pas->lock, flags);
	} else {		/* Remove */
		spin_lock_irqsave(&pas->lock, flags);
		list_for_each_entry(pam, &pas->lh, list) {
			/*从跟踪高端内存映射的hash链表中找到
			 * page的节点，删除并加入到page_address_pool空闲
			 * 管理链表中*/
			if (pam->page == page) {
				list_del(&pam->list);
				spin_unlock_irqrestore(&pas->lock, flags);
				spin_lock_irqsave(&pool_lock, flags);
				list_add_tail(&pam->list, &page_address_pool);
				spin_unlock_irqrestore(&pool_lock, flags);
				goto done;
			}
		}
		spin_unlock_irqrestore(&pas->lock, flags);
	}
done:
	return;
}

static struct page_address_map page_address_maps[LAST_PKMAP];

/**
 * @ page_address_init: 高端内存管理结构的初始化
*/
void __init page_address_init(void)
{
	int i;

	INIT_LIST_HEAD(&page_address_pool);
	for (i = 0; i < ARRAY_SIZE(page_address_maps); i++)
		list_add(&page_address_maps[i].list, &page_address_pool);
	for (i = 0; i < ARRAY_SIZE(page_address_htable); i++) {
		INIT_LIST_HEAD(&page_address_htable[i].lh);
		spin_lock_init(&page_address_htable[i].lock);
	}
	spin_lock_init(&pool_lock);
}

#endif	/* defined(CONFIG_HIGHMEM) && !defined(WANT_PAGE_VIRTUAL) */

#if defined(CONFIG_DEBUG_HIGHMEM) && defined(CONFIG_TRACE_IRQFLAGS_SUPPORT)

void debug_kmap_atomic(enum km_type type)
{
	static int warn_count = 10;

	if (unlikely(warn_count < 0))
		return;

	if (unlikely(in_interrupt())) {
		if (in_nmi()) {
			if (type != KM_NMI && type != KM_NMI_PTE) {
				WARN_ON(1);
				warn_count--;
			}
		} else if (in_irq()) {
			if (type != KM_IRQ0 && type != KM_IRQ1 &&
			    type != KM_BIO_SRC_IRQ && type != KM_BIO_DST_IRQ &&
			    type != KM_BOUNCE_READ && type != KM_IRQ_PTE) {
				WARN_ON(1);
				warn_count--;
			}
		} else if (!irqs_disabled()) {	/* softirq */
			if (type != KM_IRQ0 && type != KM_IRQ1 &&
			    type != KM_SOFTIRQ0 && type != KM_SOFTIRQ1 &&
			    type != KM_SKB_SUNRPC_DATA &&
			    type != KM_SKB_DATA_SOFTIRQ &&
			    type != KM_BOUNCE_READ) {
				WARN_ON(1);
				warn_count--;
			}
		}
	}

	if (type == KM_IRQ0 || type == KM_IRQ1 || type == KM_BOUNCE_READ ||
			type == KM_BIO_SRC_IRQ || type == KM_BIO_DST_IRQ ||
			type == KM_IRQ_PTE || type == KM_NMI ||
			type == KM_NMI_PTE ) {
		if (!irqs_disabled()) {
			WARN_ON(1);
			warn_count--;
		}
	} else if (type == KM_SOFTIRQ0 || type == KM_SOFTIRQ1) {
		if (irq_count() == 0 && !irqs_disabled()) {
			WARN_ON(1);
			warn_count--;
		}
	}
}

#endif
