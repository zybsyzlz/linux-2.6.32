/*
 *  bootmem - A boot-time physical memory allocator and configurator
 *
 *  Copyright (C) 1999 Ingo Molnar
 *                1999 Kanoj Sarcar, SGI
 *                2008 Johannes Weiner
 *
 * Access to this subsystem has to be serialized externally (which is true
 * for the boot process anyway).
 * 本文件功能:内存节点bootm分配器
 */
#include <linux/init.h>
#include <linux/pfn.h>
#include <linux/bootmem.h>
#include <linux/module.h>
#include <linux/kmemleak.h>

#include <asm/bug.h>
#include <asm/io.h>
#include <asm/processor.h>

#include "internal.h"

unsigned long max_low_pfn;
unsigned long min_low_pfn;
unsigned long max_pfn;

#ifdef CONFIG_CRASH_DUMP
/*
 * If we have booted due to a crash, max_pfn will be a very low value. We need
 * to know the amount of memory that the previous kernel used.
 */
unsigned long saved_max_pfn;
#endif

bootmem_data_t bootmem_node_data[MAX_NUMNODES] __initdata;

/*管理所有的node的bootmem分配器链表*/
static struct list_head bdata_list __initdata = LIST_HEAD_INIT(bdata_list);

static int bootmem_debug;

static int __init bootmem_debug_setup(char *buf)
{
	bootmem_debug = 1;
	return 0;
}
early_param("bootmem_debug", bootmem_debug_setup);

#define bdebug(fmt, args...) ({				\
	if (unlikely(bootmem_debug))			\
		printk(KERN_INFO			\
			"bootmem::%s " fmt,		\
			__func__, ## args);		\
})

/**
 * bootmap_bytes:计算出需要多少字节数来表示pages页数
 * @ pages:页数
*/
static unsigned long __init bootmap_bytes(unsigned long pages)
{
	/*其中每一位代表一页*/
	unsigned long bytes = (pages + 7) / 8;

	/*对其到系统自然边界上*/
	return ALIGN(bytes, sizeof(long));
}

/**
 * bootmem_bootmap_pages:计算管理pages页需要的位图的大小(单位:页)
 */
unsigned long __init bootmem_bootmap_pages(unsigned long pages)
{
	unsigned long bytes = bootmap_bytes(pages);

	/*计算出表示pages页所需的位图(位图的大小按页计算页数）*/
	return PAGE_ALIGN(bytes) >> PAGE_SHIFT;
}

/*
 * link_bootmem:bdata的bootmem分配器加入到bdata_list链表中(降序) 
 */
static void __init link_bootmem(bootmem_data_t *bdata)
{
	struct list_head *iter;

	list_for_each(iter, &bdata_list) {
		bootmem_data_t *ent;

		ent = list_entry(iter, bootmem_data_t, list);
		if (bdata->node_min_pfn < ent->node_min_pfn)
			break;
	}
	list_add_tail(&bdata->list, iter);
}

/**
 * init_bootmem_core:初始化bdata bootm分配器
 * @ bdata: bootmem分配器数据结构
 * @ mapstart: 位图内存空间的起始页号
 * @ start: bootmem管理的物理页的起始页号
 * @ end: bootmem管理的物理页的结束页号
*/
static unsigned long __init init_bootmem_core(bootmem_data_t *bdata,
	unsigned long mapstart, unsigned long start, unsigned long end)
{
	unsigned long mapsize;

	mminit_validate_memmodel_limits(&start, &end);
	/*bdata内存映射位图起始地址(线性地址)*/
	bdata->node_bootmem_map = phys_to_virt(PFN_PHYS(mapstart));
	bdata->node_min_pfn = start;
	bdata->node_low_pfn = end;
	/*将bdata加入到bdata_list链表中*/
	link_bootmem(bdata);

	/*
	 * 设置start ~ end之间物理页的管理位图为已使用
	 */
	mapsize = bootmap_bytes(end - start);
	memset(bdata->node_bootmem_map, 0xff, mapsize);

	bdebug("nid=%td start=%lx map=%lx end=%lx mapsize=%lx\n",
		bdata - bootmem_node_data, start, mapstart, end, mapsize);

	return mapsize;
}

/**
 * init_bootmem_node: 将[startpfn, endpfn]之间的页加入到bootmmem内存分配器中
 * @ pgdat:node的数据结构
 * @ freepfn: [startpfn, endpfn]的管理的位图的起始页号
 * @ startpfn:起始物理页号
 * @ endpfn: 结束物理页号
 * @ return : 返回位图的字节数
 */
unsigned long __init init_bootmem_node(pg_data_t *pgdat, unsigned long freepfn,
				unsigned long startpfn, unsigned long endpfn)
{
	return init_bootmem_core(pgdat->bdata, freepfn, startpfn, endpfn);
}

/**
 * init_bootmem:初始化内存节点0的bootmem分配器 
 * @start: bootmem页框管理位图起始页号 
 * @pages: 页数
 */
unsigned long __init init_bootmem(unsigned long start, unsigned long pages)
{
	max_low_pfn = pages;
	min_low_pfn = start;
	return init_bootmem_core(NODE_DATA(0)->bdata, start, 0, pages);
}

/**
 * free_all_bootmem_core: 销毁bootmem内存分配器
 * @ bdata:bootmem分配器结构
*/
static unsigned long __init free_all_bootmem_core(bootmem_data_t *bdata)
{
	int aligned;
	struct page *page;
	unsigned long start, end, pages, count = 0;

	if (!bdata->node_bootmem_map)
		return 0;

	/*计算出bootmem分配器中起始页号与结束页号*/
	start = bdata->node_min_pfn;
	end = bdata->node_low_pfn;

	/*
	 * If the start is aligned to the machines wordsize, we might
	 * be able to free pages in bulks of that order.
	 */
	aligned = !(start & (BITS_PER_LONG - 1));

	bdebug("nid=%td start=%lx end=%lx aligned=%d\n",
		bdata - bootmem_node_data, start, end, aligned);

	while (start < end) {
		unsigned long *map, idx, vec;

		map = bdata->node_bootmem_map;
		idx = start - bdata->node_min_pfn;
		/*得到bootmem分配器中页框的管理位图*/
		vec = ~map[idx / BITS_PER_LONG];
		/**
		 * 此处的优化建议:
		 * step1:计算出vec位图中的页框数p
		 * step2:释放2^ilog2(p)页框数
		 * step3:如果p刚好不是2的几何数,则释放p2 = p - ilog2(p)页框
		 * 通过这样的优化后,代码可以统一逻辑管理
		 */
		if (aligned && vec == ~0UL && start + BITS_PER_LONG < end) {
			int order = ilog2(BITS_PER_LONG);
			/* 如果start对齐于BITS_PER_LONG边界，则
			 * 每次可以一次性释放 BITS_PER_LONG个页框
			 * 这样是为了处于优化的目的，才这样做*/
			__free_pages_bootmem(pfn_to_page(start), order);
			count += BITS_PER_LONG;
		} else {
			unsigned long off = 0;
			/*逐页的释放*/
			while (vec && off < BITS_PER_LONG) {
				if (vec & 1) {
					page = pfn_to_page(start + off);
					__free_pages_bootmem(page, 0);
					count++;
				}
				vec >>= 1;
				off++;
			}
		}
		start += BITS_PER_LONG;
	}

	/*释放bootmem的管理位图的空间*/
	page = virt_to_page(bdata->node_bootmem_map);
	pages = bdata->node_low_pfn - bdata->node_min_pfn;
	pages = bootmem_bootmap_pages(pages);
	count += pages;
	while (pages--)
		__free_pages_bootmem(page++, 0);

	bdebug("nid=%td released=%lx\n", bdata - bootmem_node_data, count);

	/*返回bootm释放的页框数,包括管理位图占用的页框数*/
	return count;
}

/**
 * free_all_bootmem_node:销毁pgdat的bootmem分配器
 */
unsigned long __init free_all_bootmem_node(pg_data_t *pgdat)
{
	register_page_bootmem_info_node(pgdat);
	return free_all_bootmem_core(pgdat->bdata);
}

/**
 * free_all_bootmem:销毁系统中全部的bootmem分配器
 * 注:一致性内存访问只有一个node,因此，这个地方使用
 * NODE_DATA(0)表示系统的node
*/
unsigned long __init free_all_bootmem(void)
{
	return free_all_bootmem_core(NODE_DATA(0)->bdata);
}

/**
 * __free:清空bdata sidx ~ eidx之间的位图
 * @ bdata:
 * @ sidx: 起始页框位图索引
 * @ eidx: 结束页框位图索引
*/
static void __init __free(bootmem_data_t *bdata,
			unsigned long sidx, unsigned long eidx)
{
	unsigned long idx;

	bdebug("nid=%td start=%lx end=%lx\n", bdata - bootmem_node_data,
		sidx + bdata->node_min_pfn,
		eidx + bdata->node_min_pfn);

	if (bdata->hint_idx > sidx)
		bdata->hint_idx = sidx;

	for (idx = sidx; idx < eidx; idx++)
		if (!test_and_clear_bit(idx, bdata->node_bootmem_map))
			BUG();
}

/**
 * __reserve:将bdata中sinx ~ eidx之间的页标记为保留
 * @ bdata: node对应的数据结构
 * @ sidx: 起始页框位图索引
 * @ eidx: 结束页框位图索引
 * @ flags:标记
*/
static int __init __reserve(bootmem_data_t *bdata, unsigned long sidx,
			unsigned long eidx, int flags)
{
	unsigned long idx;
	int exclusive = flags & BOOTMEM_EXCLUSIVE;

	bdebug("nid=%td start=%lx end=%lx flags=%x\n",
		bdata - bootmem_node_data,
		sidx + bdata->node_min_pfn,
		eidx + bdata->node_min_pfn,
		flags);

	/*设置node_bootmem_map位图*/
	for (idx = sidx; idx < eidx; idx++)
		if (test_and_set_bit(idx, bdata->node_bootmem_map)) {
			if (exclusive) {
				__free(bdata, sidx, idx);
				return -EBUSY;
			}
			bdebug("silent double reserve of PFN %lx\n",
				idx + bdata->node_min_pfn);
		}
	return 0;
}

/**
 * mark_bootmem_node:设置bootmem分配器中start ~ end之间页框的管理位图
 * @ bdata:bootmem分配器结构
 * @ start:bootmem分配器中页框的起始地址
 * @ end: bootmem分配器中页框的结束地址
 * @ reserve: 0:空闲，1: 保留
 * @ flags: 页框的属性值
 */
static int __init mark_bootmem_node(bootmem_data_t *bdata,
				unsigned long start, unsigned long end,
				int reserve, int flags)
{
	unsigned long sidx, eidx;

	bdebug("nid=%td start=%lx end=%lx reserve=%d flags=%x\n",
		bdata - bootmem_node_data, start, end, reserve, flags);

	/*起始与结束的页号不能超过该node的所含页的起始与结束页号*/
	BUG_ON(start < bdata->node_min_pfn);
	BUG_ON(end > bdata->node_low_pfn);

	/*计算出node内的页号索引*/
	sidx = start - bdata->node_min_pfn;
	eidx = end - bdata->node_min_pfn;

	if (reserve)
		return __reserve(bdata, sidx, eidx, flags);
	else
		__free(bdata, sidx, eidx);
	return 0;
}

/**
 * mark_bootmem:设置start ~ end之间的bootmem分配器中管理页位图的标记
 * @ start: 起始页号
 * @ end: 结束页号
 * @ reserve:是否为保留页标记
 * @ flags: 需要设置的位图标记
*/
static int __init mark_bootmem(unsigned long start, unsigned long end,
				int reserve, int flags)
{
	unsigned long pos;
	bootmem_data_t *bdata;

	pos = start;
	list_for_each_entry(bdata, &bdata_list, list) {
		int err;
		unsigned long max;

		if (pos < bdata->node_min_pfn ||
		    pos >= bdata->node_low_pfn) {
			BUG_ON(pos != start);
			continue;
		}

		max = min(bdata->node_low_pfn, end);

		err = mark_bootmem_node(bdata, pos, max, reserve, flags);
		if (reserve && err) {
			mark_bootmem(start, pos, 0, 0);
			return err;
		}

		if (max == end)
			return 0;
		pos = bdata->node_low_pfn;
	}
	BUG();
}

/**
 * free_bootmem_node: 清空[physaddr, physaddr + size]之间的bootmem管理位图
 * @ pgdat: node数据结构
 * @ physaddr:起始物理地址
 * @ size:长度
 */
void __init free_bootmem_node(pg_data_t *pgdat, unsigned long physaddr,
			      unsigned long size)
{
	unsigned long start, end;

	kmemleak_free_part(__va(physaddr), size);

	/*将物理地址转化为页号*/
	start = PFN_UP(physaddr);
	end = PFN_DOWN(physaddr + size);

	mark_bootmem_node(pgdat->bdata, start, end, 0, 0);
}

/**
 * free_bootmem: bootmem分配器释放size大小的内存空间
 * @ addr: 待释放内存起始地址 
 * @ size: 释放内存的大小 
 */
void __init free_bootmem(unsigned long addr, unsigned long size)
{
	unsigned long start, end;

	kmemleak_free_part(__va(addr), size);

	start = PFN_UP(addr);
	end = PFN_DOWN(addr + size);

	mark_bootmem(start, end, 0, 0);
}

/**
 * reserve_bootmem_node:bootmem设置size大小的内存空间为保留 
 * @ pgdat: node结构 
 * @ physaddr:起始物理地址 
 * @ size: 保留内存的长度 
 * @ flags:保留内存设置标记 
 */
int __init reserve_bootmem_node(pg_data_t *pgdat, unsigned long physaddr,
				 unsigned long size, int flags)
{
	unsigned long start, end;

	/*计算出开始和结束的页号*/
	start = PFN_DOWN(physaddr);
	end = PFN_UP(physaddr + size);

	return mark_bootmem_node(pgdat->bdata, start, end, 1, flags);
}

/**
 * reserve_bootmem:设置[addr,addr+size]连续物理内存为保留
 */
int __init reserve_bootmem(unsigned long addr, unsigned long size,
			    int flags)
{
	unsigned long start, end;

	start = PFN_DOWN(addr);
	end = PFN_UP(addr + size);

	return mark_bootmem(start, end, 1, flags);
}

/**
 * align_base:返回对齐于step的相对索引号
 * 注:如果base+idx的值刚好对齐于step，则返回原始idx
 *    如果base+idx的值不是对齐于step，修正对齐于step后,
 *    返回新的idx值
 */
static inline unsigned long align_base(unsigned long base, unsigned long idx, 
		unsigned long step)
{
	/* step1：计算出绝对页号:base+idx
	 * step2: 计算出绝对页号对齐于step后的页号索引
	 * step3: 返回对齐于step页号的相对偏移页号
	 * 这是一个非常操蛋的计算过程*/
	return ALIGN(base + idx, step) -base;
}

/**
 * aligin_idx:返回对齐与step的idx值
 */
static unsigned long align_idx(struct bootmem_data *bdata, unsigned long idx,
			unsigned long step)
{
	unsigned long base = bdata->node_min_pfn;
	return align_base(base, idx, step);
}

/**
 * aligin_off:返回对齐于step的off值
 */
static unsigned long align_off(struct bootmem_data *bdata, unsigned long off,
			unsigned long align)
{
	unsigned long base = PFN_PHYS(bdata->node_min_pfn);
	return align_base(base, off, align);
}

/**
 * alloc_bootmem_core:在bootmem分配器中申请size字节的内存
 * @ bdata: bootmem分配器数据结构
 * @ size: 分配内存的大小
 * @ align:期望分配内存的字节对齐
 * @ goal: 期望开始分配内存所在的区域的起始地址
 * @ limit:分配空间的限制区域 
*/
static void * __init alloc_bootmem_core(struct bootmem_data *bdata,
					unsigned long size, unsigned long align,
					unsigned long goal, unsigned long limit)
{
	unsigned long fallback = 0;
	unsigned long min, max, start, sidx, midx, step;

	bdebug("nid=%td size=%lx [%lu pages] align=%lx goal=%lx limit=%lx\n",
		bdata - bootmem_node_data, size, PAGE_ALIGN(size) >> PAGE_SHIFT,
		align, goal, limit);

	BUG_ON(!size);
	/*如果align不是2的N次方*/
	BUG_ON(align & (align - 1));
	/*如果超出他的限制值*/
	BUG_ON(limit && goal + size > limit);

	if (!bdata->node_bootmem_map)
		return NULL;

	/*node低端内存的起始页号与结束页号*/
	min = bdata->node_min_pfn;
	max = bdata->node_low_pfn;

	goal >>= PAGE_SHIFT;
	limit >>= PAGE_SHIFT;

	/*计算出进行分配空间的最大页号，不能超过limit的限制值*/
	if (limit && max > limit)
		max = limit;
	if (max <= min)
		return NULL;

	/* 计算出为了满足align对齐条件，
	 * 在寻找页框时的对齐因子, 以页为
	 * 单位,最小的对齐因子也是1页*/
	step = max(align >> PAGE_SHIFT, 1UL);

	/* 如果goal存在，并且goal满足[min, max]区间
	 * 则起始页号从goal开始*/
	if (goal && min < goal && goal < max)
		start = ALIGN(goal, step);
	else
		start = ALIGN(min, step);

	/* sidx:起始索引, midx:结束索引
	 * 这两个索引值是通过align limit goal等限制
	 * 条件都满足的条件下在node中相对页号偏移量，假如：
	 * align = limit = goal = 0,则没有任何的限制
	 * 条件，则 sidx = min, midx = max
	*/
	sidx = start - bdata->node_min_pfn;
	midx = max - bdata->node_min_pfn;

	/* hint_idx：表示上次分配内存的末端地址的页号
	 * 如果，hint_idx > sidx ，则hint_idx与sidx
	 * 发生重叠，则需要上调sidx值*/
	if (bdata->hint_idx > sidx) {
		/*
		 * Handle the valid case of sidx being zero and still
		 * catch the fallback below.
		 */
		fallback = sidx + 1;
		sidx = align_idx(bdata, bdata->hint_idx, step);
	}

	while (1) {
		int merge;
		void *region;
		unsigned long eidx, i, start_off, end_off;
find_block:
		/*找到bootmem中空闲的page页的页号*/
		sidx = find_next_zero_bit(bdata->node_bootmem_map, midx, sidx);
		sidx = align_idx(bdata, sidx, step);
		eidx = sidx + PFN_UP(size);

		/*超出了bootmem管理的页框索引不能分配*/
		if (sidx >= midx || eidx > midx)
			break;

		/*查看sidx ~ eidx之间的页是否已经被分配出去了*/
		for (i = sidx; i < eidx; i++)
			if (test_bit(i, bdata->node_bootmem_map)) {
				sidx = align_idx(bdata, i, step);
				/*i与sidx发生重叠*/
				if (sidx == i)
					sidx += step; /*重新确定下一个查找的地方*/
				goto find_block;
			}

		/* 如果不是第一次进行内存申请，则last_end_off != 0,
		 * last_end_off:保存了上次进行内存申请的末端地址
		 * last_end_off & PAGE_SIZE - 1 != 0:表示上次的分配
		 * 的结束地址并不是页框对齐,此次的分配从last_end_off的aligin地址开始
		 * 这样做的目的是为了防止产生更多的内存碎片*/
		if (bdata->last_end_off & (PAGE_SIZE - 1) &&
				PFN_DOWN(bdata->last_end_off) + 1 == sidx) {
			/*从上次内存分配的结束地址为基地址，得出满足align对齐条件的
			 * 物理地址的开始*/
			start_off = align_off(bdata, bdata->last_end_off, align);
		}
		else
			start_off = PFN_PHYS(sidx);

		/* 如果start_off地址所在的页号小于sidx，也就说
		 * 此次的分配,并不是从sidx页号开始，则将
		 * start_off所在的页号的剩余空闲内存空间参与到此次
		 * 的内存分配当中，并且将它所在的页号也设置为已分配
		 * 因此,merge = 1:表示需要设置start_off所在页号的位图
		 *      merge = 0:则不需要*/
		merge = PFN_DOWN(start_off) < sidx;
		end_off = start_off + size;

		/*确定申请空间的结束物理地址与页号*/
		bdata->last_end_off = end_off;
		bdata->hint_idx = PFN_UP(end_off);

		/* 将PFN_DOWN(start_off) +merge ~ PFN_UP(end_off)
		 * 之间的页的位图标记为使用，即到了满足条件的
		 * 页的区间*/
		if (__reserve(bdata, PFN_DOWN(start_off) + merge,
				PFN_UP(end_off), BOOTMEM_EXCLUSIVE))
			BUG();

		/*将申请内存的物理地址转化为虚拟地址*/
		region = phys_to_virt(PFN_PHYS(bdata->node_min_pfn) +
				start_off);
		memset(region, 0, size);
		/*
		 * The min_count is set to 0 so that bootmem allocated blocks
		 * are never reported as leaks.
		 */
		kmemleak_alloc(region, size, 0, 0);
		return region;
	}

	/*如果没有合适的空间，则从fallback - 1(刚好恢复到旧的sidx)，继续查找*/
	if (fallback) {
		sidx = align_idx(bdata, fallback - 1, step);
		fallback = 0;
		goto find_block;
	}

	return NULL;
}

/**
 * alloc_arch_preferred_bootmem:指定node中分配size大小的内存
 */
static void * __init alloc_arch_preferred_bootmem(bootmem_data_t *bdata,
					unsigned long size, unsigned long align,
					unsigned long goal, unsigned long limit)
{
	if (WARN_ON_ONCE(slab_is_available()))
		return kzalloc(size, GFP_NOWAIT);

#ifdef CONFIG_HAVE_ARCH_BOOTMEM
	{
		bootmem_data_t *p_bdata;

		p_bdata = bootmem_arch_preferred_node(bdata, size, align,
							goal, limit);
		if (p_bdata)
			return alloc_bootmem_core(p_bdata, size, align,
							goal, limit);
	}
#endif
	return NULL;
}

static void * __init ___alloc_bootmem_nopanic(unsigned long size,
					unsigned long align,
					unsigned long goal,
					unsigned long limit)
{
	bootmem_data_t *bdata;
	void *region;

restart:
	region = alloc_arch_preferred_bootmem(NULL, size, align, goal, limit);
	if (region)
		return region;

	list_for_each_entry(bdata, &bdata_list, list) {
		/*找到满足node的起始页号大于goal页号的node*/
		if (goal && bdata->node_low_pfn <= PFN_DOWN(goal))
			continue;
		/*如果找到的node起始页号超出限制区域，则直接跳出,没有满足条件的区域*/
		if (limit && bdata->node_min_pfn >= PFN_DOWN(limit))
			break;

		/*找到符合goal 与limit的node，则在其中分配size大小空间*/
		region = alloc_bootmem_core(bdata, size, align, goal, limit);
		if (region)
			return region;
	}
	/*在goal期望的起始地址处没有分配到内存,则从内存起始地址查找分配*/
	if (goal) {
		goal = 0;
		goto restart;
	}

	return NULL;
}

/**
 * __alloc_bootmem_nopanic - allocate boot memory without panicking
 */
void * __init __alloc_bootmem_nopanic(unsigned long size, unsigned long align,
					unsigned long goal)
{
	return ___alloc_bootmem_nopanic(size, align, goal, 0);
}

static void * __init ___alloc_bootmem(unsigned long size, unsigned long align,
					unsigned long goal, unsigned long limit)
{
	void *mem = ___alloc_bootmem_nopanic(size, align, goal, limit);

	if (mem)
		return mem;

	printk(KERN_ALERT "bootmem alloc of %lu bytes failed!\n", size);
	panic("Out of memory");
	return NULL;
}

/**
 * __alloc_bootmem:分配size大小的内存
 */
void * __init __alloc_bootmem(unsigned long size, unsigned long align,
			      unsigned long goal)
{
	return ___alloc_bootmem(size, align, goal, 0);
}

static void * __init ___alloc_bootmem_node(bootmem_data_t *bdata,
				unsigned long size, unsigned long align,
				unsigned long goal, unsigned long limit)
{
	void *ptr;

	ptr = alloc_arch_preferred_bootmem(bdata, size, align, goal, limit);
	if (ptr)
		return ptr;

	/*从指定的内存节点申请内存*/
	ptr = alloc_bootmem_core(bdata, size, align, goal, limit);
	if (ptr)
		return ptr;

	/*如果申请失败,则从内存节点0中申请内存*/
	return ___alloc_bootmem(size, align, goal, limit);
}

/**
 * __alloc_bootmem_node:从制定的node中分配size内存
 */
void * __init __alloc_bootmem_node(pg_data_t *pgdat, unsigned long size,
				   unsigned long align, unsigned long goal)
{
	if (WARN_ON_ONCE(slab_is_available()))
		return kzalloc_node(size, GFP_NOWAIT, pgdat->node_id);

	return ___alloc_bootmem_node(pgdat->bdata, size, align, goal, 0);
}

#ifdef CONFIG_SPARSEMEM
/**
 * alloc_bootmem_section:从制定的区中分配size大小的内存
 */
void * __init alloc_bootmem_section(unsigned long size,
				    unsigned long section_nr)
{
	bootmem_data_t *bdata;
	unsigned long pfn, goal, limit;

	pfn = section_nr_to_pfn(section_nr);
	goal = pfn << PAGE_SHIFT;
	limit = section_nr_to_pfn(section_nr + 1) << PAGE_SHIFT;
	bdata = &bootmem_node_data[early_pfn_to_nid(pfn)];

	return alloc_bootmem_core(bdata, size, SMP_CACHE_BYTES, goal, limit);
}
#endif

/**
 * __alloc_bootmem_nopanic:分配size大小的内存
 *	注:如果制定node中分配失败,则从满足goal的
 *	node中分配内存
 */
void * __init __alloc_bootmem_node_nopanic(pg_data_t *pgdat, unsigned long size,
				   unsigned long align, unsigned long goal)
{
	void *ptr;

	if (WARN_ON_ONCE(slab_is_available()))
		return kzalloc_node(size, GFP_NOWAIT, pgdat->node_id);

	ptr = alloc_arch_preferred_bootmem(pgdat->bdata, size, align, goal, 0);
	if (ptr)
		return ptr;

	ptr = alloc_bootmem_core(pgdat->bdata, size, align, goal, 0);
	if (ptr)
		return ptr;

	return __alloc_bootmem_nopanic(size, align, goal);
}

#ifndef ARCH_LOW_ADDRESS_LIMIT
#define ARCH_LOW_ADDRESS_LIMIT	0xffffffffUL
#endif

/**
 * __alloc_bootmem_low - allocate low boot memory
 */
void * __init __alloc_bootmem_low(unsigned long size, unsigned long align,
				  unsigned long goal)
{
	return ___alloc_bootmem(size, align, goal, ARCH_LOW_ADDRESS_LIMIT);
}

/**
 * __alloc_bootmem_low_node:从制定的node中分配size的内存
 */
void * __init __alloc_bootmem_low_node(pg_data_t *pgdat, unsigned long size,
				       unsigned long align, unsigned long goal)
{
	if (WARN_ON_ONCE(slab_is_available()))
		return kzalloc_node(size, GFP_NOWAIT, pgdat->node_id);

	return ___alloc_bootmem_node(pgdat->bdata, size, align,
				goal, ARCH_LOW_ADDRESS_LIMIT);
}
