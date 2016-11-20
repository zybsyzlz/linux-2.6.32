#ifndef _LINUX_SLAB_DEF_H
#define	_LINUX_SLAB_DEF_H

/*
 * Definitions unique to the original Linux SLAB allocator.
 *
 * What we provide here is a way to optimize the frequent kmalloc
 * calls in the kernel by selecting the appropriate general cache
 * if kmalloc was called with a size that can be established at
 * compile time.
 */

#include <linux/init.h>
#include <asm/page.h>		/* kmalloc_sizes.h needs PAGE_SIZE */
#include <asm/cache.h>		/* kmalloc_sizes.h needs L1_CACHE_BYTES */
#include <linux/compiler.h>
#include <linux/kmemtrace.h>

/*
 *cache管理结构
 * Cache与slabs的联系
 * Cache用于管理一段连续的页框内存
 * 在这些连续的内存中，可以再被细分为
 * 一个个较小的页框，每一个页框可以被当作
 * 一个slab。在slab中可以存放具有相同大小的
 * objects(空闲和已经使用的).因此，在一个cache中可以包含很多个
 * slab。
 */

struct kmem_cache {
/* 1) per-cpu data, touched during every alloc/free */
	struct array_cache *array[NR_CPUS]; /*每个cpu本地缓存的objects结构*/
/* 2) Cache tunables. Protected by cache_chain_mutex */
	unsigned int batchcount;
	unsigned int limit;
	unsigned int shared;

	unsigned int buffer_size;   /*cache中对象大小*/
	u32 reciprocal_buffer_size; /*cache中对象大小的倒数值*/
/* 3) touched by every alloc & free from the backend */

	unsigned int flags;		/* constant flags */
	unsigned int num;		/* cache中对象的个数*/

/* 4) cache_grow/shrink */
	/* order of pgs per slab (2^n) */
	unsigned int gfporder;

	/* force GFP flags, e.g. GFP_DMA */
	gfp_t gfpflags;
	/* 在slab管理的内存空间中，将剩余的不满足object大小的内存用作缓存
	 * 着色的最大值.
	 * 原理是：将slab管理的内存空间中不满足object大小的内存除以
	 * cache缓存对齐的值，得到缓存着色需要的字节数，
	 * 目的：为了防止不同slabs中具有相同offset的object被映射
	 * 到相同的cache line中
	*/
	size_t colour;               /*slab缓存着色值*/				
	unsigned int colour_off;	/*高速缓存中对象放置的偏移量*/
	/*当slab属于外部时，申请外部slab管理结构的高速缓存*/
	struct kmem_cache *slabp_cache;   
	unsigned int slab_size;      /*slab的大小(不包含数据区的大小)*/
	unsigned int dflags;		/* dynamic flags */

	/* constructor func */
	void (*ctor)(void *obj);

/* 5) cache creation/removal */
	const char *name;
	struct list_head next;

/* 6) statistics */
#ifdef CONFIG_DEBUG_SLAB
	unsigned long num_active;
	unsigned long num_allocations;
	unsigned long high_mark;
	unsigned long grown;
	unsigned long reaped;
	unsigned long errors;
	unsigned long max_freeable;
	unsigned long node_allocs;
	unsigned long node_frees;
	unsigned long node_overflow;
	atomic_t allochit;
	atomic_t allocmiss;
	atomic_t freehit;
	atomic_t freemiss;

	/*
	 * If debugging is enabled, then the allocator can add additional
	 * fields and/or padding to every object. buffer_size contains the total
	 * object size including these internal fields, the following two
	 * variables contain the offset to the user object and its size.
	 */
	int obj_offset;
	int obj_size;
#endif /* CONFIG_DEBUG_SLAB */

	/*
	 * We put nodelists[] at the end of kmem_cache, because we want to size
	 * this array to nr_node_ids slots instead of MAX_NUMNODES
	 * (see kmem_cache_init())
	 * We still use [MAX_NUMNODES] and not [1] or [0] because cache_cache
	 * is statically defined, so we reserve the max number of nodes.
	 */
	struct kmem_list3 *nodelists[MAX_NUMNODES];
	/*
	 * Do not add fields after nodelists[]
	 */
};

/*普通cache的描述结构*/
struct cache_sizes {
	size_t		 	cs_size; /*cache对象的大小*/
	struct kmem_cache	*cs_cachep; /*存放该类对象的cache的指针*/
#ifdef CONFIG_ZONE_DMA
	struct kmem_cache	*cs_dmacachep; /*存放cs_size对象具有DMA属性的cache的指针*/
#endif
};
extern struct cache_sizes malloc_sizes[];

void *kmem_cache_alloc(struct kmem_cache *, gfp_t);
void *__kmalloc(size_t size, gfp_t flags);

#ifdef CONFIG_KMEMTRACE
extern void *kmem_cache_alloc_notrace(struct kmem_cache *cachep, gfp_t flags);
extern size_t slab_buffer_size(struct kmem_cache *cachep);
#else
static __always_inline void *
kmem_cache_alloc_notrace(struct kmem_cache *cachep, gfp_t flags)
{
	return kmem_cache_alloc(cachep, flags);
}
static inline size_t slab_buffer_size(struct kmem_cache *cachep)
{
	return 0;
}
#endif

static __always_inline void *kmalloc(size_t size, gfp_t flags)
{
	struct kmem_cache *cachep;
	void *ret;

	if (__builtin_constant_p(size)) {
		int i = 0;

		if (!size)
			return ZERO_SIZE_PTR;

#define CACHE(x) \
		if (size <= x) \
			goto found; \
		else \
			i++;
#include <linux/kmalloc_sizes.h>
#undef CACHE
		return NULL;
found:
#ifdef CONFIG_ZONE_DMA
		if (flags & GFP_DMA)
			cachep = malloc_sizes[i].cs_dmacachep;
		else
#endif
			cachep = malloc_sizes[i].cs_cachep;

		ret = kmem_cache_alloc_notrace(cachep, flags);

		trace_kmalloc(_THIS_IP_, ret,
			      size, slab_buffer_size(cachep), flags);

		return ret;
	}
	return __kmalloc(size, flags);
}

#ifdef CONFIG_NUMA
extern void *__kmalloc_node(size_t size, gfp_t flags, int node);
extern void *kmem_cache_alloc_node(struct kmem_cache *, gfp_t flags, int node);

#ifdef CONFIG_KMEMTRACE
extern void *kmem_cache_alloc_node_notrace(struct kmem_cache *cachep,
					   gfp_t flags,
					   int nodeid);
#else
static __always_inline void *
kmem_cache_alloc_node_notrace(struct kmem_cache *cachep,
			      gfp_t flags,
			      int nodeid)
{
	return kmem_cache_alloc_node(cachep, flags, nodeid);
}
#endif

static __always_inline void *kmalloc_node(size_t size, gfp_t flags, int node)
{
	struct kmem_cache *cachep;
	void *ret;

	if (__builtin_constant_p(size)) {
		int i = 0;

		if (!size)
			return ZERO_SIZE_PTR;

#define CACHE(x) \
		if (size <= x) \
			goto found; \
		else \
			i++;
#include <linux/kmalloc_sizes.h>
#undef CACHE
		return NULL;
found:
#ifdef CONFIG_ZONE_DMA
		if (flags & GFP_DMA)
			cachep = malloc_sizes[i].cs_dmacachep;
		else
#endif
			cachep = malloc_sizes[i].cs_cachep;

		ret = kmem_cache_alloc_node_notrace(cachep, flags, node);

		trace_kmalloc_node(_THIS_IP_, ret,
				   size, slab_buffer_size(cachep),
				   flags, node);

		return ret;
	}
	return __kmalloc_node(size, flags, node);
}

#endif	/* CONFIG_NUMA */

#endif	/* _LINUX_SLAB_DEF_H */
