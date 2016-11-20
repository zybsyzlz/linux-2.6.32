/*
 *  linux/mm/mempool.c
 *
 *  memory buffer pool support. Such pools are mostly used
 *  for guaranteed, deadlock-free memory allocations during
 *  extreme VM load.
 *
 *  started by Ingo Molnar, Copyright (C) 2001
 */

#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/mempool.h>
#include <linux/blkdev.h>
#include <linux/writeback.h>

/**
 * add_element:向pool内存池中添加元素
 */
static void add_element(mempool_t *pool, void *element)
{
	BUG_ON(pool->curr_nr >= pool->min_nr);
	pool->elements[pool->curr_nr++] = element;
}

/**
 * remove_element:pool内存池删除最近加入的一个内存元素
 */
static void *remove_element(mempool_t *pool)
{
	BUG_ON(pool->curr_nr <= 0);
	return pool->elements[--pool->curr_nr];
}

/**
 * free_pool:释放内存池
 */
static void free_pool(mempool_t *pool)
{
	/*释放内存池中存在的元素的内存空间*/
	while (pool->curr_nr) {
		void *element = remove_element(pool);
		pool->free(element, pool->pool_data);
	}
	/*释放保存内存元素的空间*/
	kfree(pool->elements);
	/*释放内存池本身*/
	kfree(pool);
}

/**
 * mempool_create:创建一个内存池 
 * @min_nr:   内存池中存放的内存元素数组大小
 * @alloc_fn: 申请内存元素的函数接口
 * @free_fn:  释放内存元素的函数接口  
 * @pool_data:内存池的私有数据
 * @!IRQ contexts.
 */
mempool_t *mempool_create(int min_nr, mempool_alloc_t *alloc_fn,
				mempool_free_t *free_fn, void *pool_data)
{
	return  mempool_create_node(min_nr,alloc_fn,free_fn, pool_data,-1);
}
EXPORT_SYMBOL(mempool_create);

/**
 * mempool_create_node:创建一个内存池
 * @min_nr:内存池中存放的内存元素数组大小
 * @alloc_fn:申请内存元素的函数接口
 * @free_fn:释放内存元素的函数接口
 * @pool_data:内存池私有数据
 * @node_id: node id
*/
mempool_t *mempool_create_node(int min_nr, mempool_alloc_t *alloc_fn,
			mempool_free_t *free_fn, void *pool_data, int node_id)
{
	/*创建内存池结构*/
	mempool_t *pool;
	pool = kmalloc_node(sizeof(*pool), GFP_KERNEL | __GFP_ZERO, node_id);
	if (!pool)
		return NULL;
	/*创建内存池元素内存空间*/
	pool->elements = kmalloc_node(min_nr * sizeof(void *),
					GFP_KERNEL, node_id);
	if (!pool->elements) {
		kfree(pool);
		return NULL;
	}
	spin_lock_init(&pool->lock);
	pool->min_nr = min_nr;
	pool->pool_data = pool_data;
	/*初始化内存池等待队列*/
	init_waitqueue_head(&pool->wait);
	pool->alloc = alloc_fn;
	pool->free = free_fn;

	/*给内存池添加内存元素*/
	while (pool->curr_nr < pool->min_nr) {
		void *element;

		/*调用申请内存元素的方法*/
		element = pool->alloc(GFP_KERNEL, pool->pool_data);
		if (unlikely(!element)) {
			free_pool(pool);
			return NULL;
		}
		add_element(pool, element);
	}
	return pool;
}
EXPORT_SYMBOL(mempool_create_node);

/**
 * mempool_resize:调整内存池元素空间的大小
 */
int mempool_resize(mempool_t *pool, int new_min_nr, gfp_t gfp_mask)
{
	void *element;
	void **new_elements;
	unsigned long flags;

	BUG_ON(new_min_nr <= 0);

	spin_lock_irqsave(&pool->lock, flags);
	if (new_min_nr <= pool->min_nr) {
		while (new_min_nr < pool->curr_nr) {
			element = remove_element(pool);
			spin_unlock_irqrestore(&pool->lock, flags);
			pool->free(element, pool->pool_data);
			spin_lock_irqsave(&pool->lock, flags);
		}
		pool->min_nr = new_min_nr;
		goto out_unlock;
	}
	spin_unlock_irqrestore(&pool->lock, flags);

	/* Grow the pool */
	new_elements = kmalloc(new_min_nr * sizeof(*new_elements), gfp_mask);
	if (!new_elements)
		return -ENOMEM;

	spin_lock_irqsave(&pool->lock, flags);
	if (unlikely(new_min_nr <= pool->min_nr)) {
		/* Raced, other resize will do our work */
		spin_unlock_irqrestore(&pool->lock, flags);
		kfree(new_elements);
		goto out;
	}
	memcpy(new_elements, pool->elements,
			pool->curr_nr * sizeof(*new_elements));
	kfree(pool->elements);
	pool->elements = new_elements;
	pool->min_nr = new_min_nr;

	while (pool->curr_nr < pool->min_nr) {
		spin_unlock_irqrestore(&pool->lock, flags);
		element = pool->alloc(gfp_mask, pool->pool_data);
		if (!element)
			goto out;
		spin_lock_irqsave(&pool->lock, flags);
		if (pool->curr_nr < pool->min_nr) {
			add_element(pool, element);
		} else {
			spin_unlock_irqrestore(&pool->lock, flags);
			pool->free(element, pool->pool_data);	/* Raced */
			goto out;
		}
	}
out_unlock:
	spin_unlock_irqrestore(&pool->lock, flags);
out:
	return 0;
}
EXPORT_SYMBOL(mempool_resize);

/**
 * mempool_destroy:销毁内存池
 */
void mempool_destroy(mempool_t *pool)
{
	/* Check for outstanding elements */
	BUG_ON(pool->curr_nr != pool->min_nr);
	free_pool(pool);
}
EXPORT_SYMBOL(mempool_destroy);

/**
 * mempool_alloc:从内存池中申请一个元素
 */
void * mempool_alloc(mempool_t *pool, gfp_t gfp_mask)
{
	void *element;
	unsigned long flags;
	wait_queue_t wait;
	gfp_t gfp_temp;

	might_sleep_if(gfp_mask & __GFP_WAIT);

	gfp_mask |= __GFP_NOMEMALLOC;	/* don't allocate emergency reserves */
	gfp_mask |= __GFP_NORETRY;	/* don't loop in __alloc_pages */
	gfp_mask |= __GFP_NOWARN;	/* failures are OK */

	gfp_temp = gfp_mask & ~(__GFP_WAIT|__GFP_IO);

repeat_alloc:

	element = pool->alloc(gfp_temp, pool->pool_data);
	if (likely(element != NULL))
		return element;

	spin_lock_irqsave(&pool->lock, flags);
	if (likely(pool->curr_nr)) {
		element = remove_element(pool);
		spin_unlock_irqrestore(&pool->lock, flags);
		return element;
	}
	spin_unlock_irqrestore(&pool->lock, flags);

	/* We must not sleep in the GFP_ATOMIC case */
	if (!(gfp_mask & __GFP_WAIT))
		return NULL;

	/* Now start performing page reclaim */
	gfp_temp = gfp_mask;
	init_wait(&wait);
	prepare_to_wait(&pool->wait, &wait, TASK_UNINTERRUPTIBLE);
	smp_mb();
	if (!pool->curr_nr) {
		/*
		 * FIXME: this should be io_schedule().  The timeout is there
		 * as a workaround for some DM problems in 2.6.18.
		 */
		io_schedule_timeout(5*HZ);
	}
	finish_wait(&pool->wait, &wait);

	goto repeat_alloc;
}
EXPORT_SYMBOL(mempool_alloc);

/**
 * mempool_free:元素返回给内存池
 */
void mempool_free(void *element, mempool_t *pool)
{
	unsigned long flags;

	if (unlikely(element == NULL))
		return;

	smp_mb();
	if (pool->curr_nr < pool->min_nr) {
		spin_lock_irqsave(&pool->lock, flags);
		if (pool->curr_nr < pool->min_nr) {
			add_element(pool, element);
			spin_unlock_irqrestore(&pool->lock, flags);
			wake_up(&pool->wait);
			return;
		}
		spin_unlock_irqrestore(&pool->lock, flags);
	}
	pool->free(element, pool->pool_data);
}
EXPORT_SYMBOL(mempool_free);

/*slab申请内存和释放元素的接口,用于内存池
 *中分配和释放内存元素的alloc_fn与free_fn接口
*/
void *mempool_alloc_slab(gfp_t gfp_mask, void *pool_data)
{
	struct kmem_cache *mem = pool_data;
	return kmem_cache_alloc(mem, gfp_mask);
}
EXPORT_SYMBOL(mempool_alloc_slab);

void mempool_free_slab(void *element, void *pool_data)
{
	struct kmem_cache *mem = pool_data;
	kmem_cache_free(mem, element);
}
EXPORT_SYMBOL(mempool_free_slab);

/*kmalloc申请内存和释放元素的接口,用于内存池
 *中分配和释放内存元素的alloc_fn与free_fn接口
*/
void *mempool_kmalloc(gfp_t gfp_mask, void *pool_data)
{
	size_t size = (size_t)pool_data;
	return kmalloc(size, gfp_mask);
}
EXPORT_SYMBOL(mempool_kmalloc);

void mempool_kfree(void *element, void *pool_data)
{
	kfree(element);
}
EXPORT_SYMBOL(mempool_kfree);

/*区分配器申请内存和释放元素的接口,用于内存池
 *中分配和释放内存元素的alloc_fn与free_fn接口
*/
void *mempool_alloc_pages(gfp_t gfp_mask, void *pool_data)
{
	int order = (int)(long)pool_data;
	return alloc_pages(gfp_mask, order);
}
EXPORT_SYMBOL(mempool_alloc_pages);

void mempool_free_pages(void *element, void *pool_data)
{
	int order = (int)(long)pool_data;
	__free_pages(element, order);
}
EXPORT_SYMBOL(mempool_free_pages);
