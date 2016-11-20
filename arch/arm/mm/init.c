/*
 *  linux/arch/arm/mm/init.c
 *
 *  Copyright (C) 1995-2005 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/swap.h>
#include <linux/init.h>
#include <linux/bootmem.h>
#include <linux/mman.h>
#include <linux/nodemask.h>
#include <linux/initrd.h>
#include <linux/sort.h>
#include <linux/highmem.h>

#include <asm/mach-types.h>
#include <asm/sections.h>
#include <asm/setup.h>
#include <asm/sizes.h>
#include <asm/tlb.h>

#include <asm/mach/arch.h>
#include <asm/mach/map.h>

#include "mm.h"

static unsigned long phys_initrd_start __initdata = 0;
static unsigned long phys_initrd_size __initdata = 0;

static void __init early_initrd(char **p)
{
	unsigned long start, size;

	start = memparse(*p, p);
	if (**p == ',') {
		size = memparse((*p) + 1, p);

		phys_initrd_start = start;
		phys_initrd_size = size;
	}
}
__early_param("initrd=", early_initrd);

static int __init parse_tag_initrd(const struct tag *tag)
{
	printk(KERN_WARNING "ATAG_INITRD is deprecated; "
		"please update your bootloader.\n");
	phys_initrd_start = __virt_to_phys(tag->u.initrd.start);
	phys_initrd_size = tag->u.initrd.size;
	return 0;
}

__tagtable(ATAG_INITRD, parse_tag_initrd);

static int __init parse_tag_initrd2(const struct tag *tag)
{
	phys_initrd_start = tag->u.initrd.start;
	phys_initrd_size = tag->u.initrd.size;
	return 0;
}

__tagtable(ATAG_INITRD2, parse_tag_initrd2);

/*
 * This keeps memory configuration data used by a couple memory
 * initialization functions, as well as show_mem() for the skipping
 * of holes in the memory map.  It is populated by arm_add_memory().
 */
struct meminfo meminfo;

void show_mem(void)
{
	int free = 0, total = 0, reserved = 0;
	int shared = 0, cached = 0, slab = 0, node, i;
	struct meminfo * mi = &meminfo;

	printk("Mem-info:\n");
	show_free_areas();
	for_each_online_node(node) {
		pg_data_t *n = NODE_DATA(node);
		struct page *map = pgdat_page_nr(n, 0) - n->node_start_pfn;

		for_each_nodebank (i,mi,node) {
			struct membank *bank = &mi->bank[i];
			unsigned int pfn1, pfn2;
			struct page *page, *end;

			pfn1 = bank_pfn_start(bank);
			pfn2 = bank_pfn_end(bank);

			page = map + pfn1;
			end  = map + pfn2;

			do {
				total++;
				if (PageReserved(page))
					reserved++;
				else if (PageSwapCache(page))
					cached++;
				else if (PageSlab(page))
					slab++;
				else if (!page_count(page))
					free++;
				else
					shared += page_count(page) - 1;
				page++;
			} while (page < end);
		}
	}

	printk("%d pages of RAM\n", total);
	printk("%d free pages\n", free);
	printk("%d reserved pages\n", reserved);
	printk("%d slab pages\n", slab);
	printk("%d pages shared\n", shared);
	printk("%d pages swap cached\n", cached);
}

/**
 * find_node_limits: 查找node中的最小页帧号和最大页帧号
 * @ node: node编号
 * @ mi: meminfo 指针
 * @ min: 输出最小页号
 * @ max_low: 输出最大低端内存页帧号
 * @ max_high:输出最大高端内存页帧号
*/
static void __init find_node_limits(int node, struct meminfo *mi,
	unsigned long *min, unsigned long *max_low, unsigned long *max_high)
{
	int i;

	*min = -1UL;
	*max_low = *max_high = 0;

	/*一个node中包含多个bank,如果bank不是
	 * 高端内存，则max_low = max_high,即低端内存
	 * 和高端内存的页号相等
	*/
	for_each_nodebank(i, mi, node) {
		struct membank *bank = &mi->bank[i];
		unsigned long start, end;

		start = bank_pfn_start(bank);
		end = bank_pfn_end(bank);

		if (*min > start)
			*min = start;
		if (*max_high < end)
			*max_high = end;
		if (bank->highmem)
			continue;
		if (*max_low < end)
			*max_low = end;
	}
}

/**
 * find_bootmap_pfn:为bootmap_pages申请物理空间
 * @ node: node id
 * @ mi: 内存信息结构
 * @ bootmap_pages: 位图占用的页数
 * @ note:在_end区域外找到满足bootmap_pages页数
 * 的bank内存区域，并返回满足条件的起始页号
 */
static unsigned int __init
find_bootmap_pfn(int node, struct meminfo *mi, unsigned int bootmap_pages)
{
	unsigned int start_pfn, i, bootmap_pfn;

	/*内核代码段结束*/
	start_pfn   = PAGE_ALIGN(__pa(_end)) >> PAGE_SHIFT;
	bootmap_pfn = 0;

	for_each_nodebank(i, mi, node) {
		struct membank *bank = &mi->bank[i];
		unsigned int start, end;

		start = bank_pfn_start(bank);
		end   = bank_pfn_end(bank);

		if (end < start_pfn)
			continue;

		if (start < start_pfn)
			start = start_pfn;

		if (end <= start)
			continue;

		/*start~ end之间的页数刚好满足bootmap_pages所
		 * 需要的页数
		*/
		if (end - start >= bootmap_pages) {
			bootmap_pfn = start; /*满足条件的起始页号*/
			break;
		}
	}

	if (bootmap_pfn == 0)
		BUG();

	return bootmap_pfn;
}

static int __init check_initrd(struct meminfo *mi)
{
	int initrd_node = -2;
#ifdef CONFIG_BLK_DEV_INITRD
	unsigned long end = phys_initrd_start + phys_initrd_size;

	/*
	 * Make sure that the initrd is within a valid area of
	 * memory.
	 */
	if (phys_initrd_size) {
		unsigned int i;

		initrd_node = -1;

		for (i = 0; i < mi->nr_banks; i++) {
			struct membank *bank = &mi->bank[i];
			if (bank_phys_start(bank) <= phys_initrd_start &&
			    end <= bank_phys_end(bank))
				initrd_node = bank->node;
		}
	}

	if (initrd_node == -1) {
		printk(KERN_ERR "INITRD: 0x%08lx+0x%08lx extends beyond "
		       "physical memory - disabling initrd\n",
		       phys_initrd_start, phys_initrd_size);
		phys_initrd_start = phys_initrd_size = 0;
	}
#endif

	return initrd_node;
}

/**
 * map_memory_bank:为bank内存组建立页表映射
 */
static inline void map_memory_bank(struct membank *bank)
{
#ifdef CONFIG_MMU
	struct map_desc map;

	map.pfn = bank_pfn_start(bank);
	map.virtual = __phys_to_virt(bank_phys_start(bank));
	map.length = bank_phys_size(bank);
	map.type = MT_MEMORY;  /*物理内存*/

	create_mapping(&map);
#endif
}

/**
 * bootmem_init_node:start_pfn ~ end_pfn之间的页框加入到bootmem内存分配器
 * @ node: node id
 * @ mi:
 * @ start_pfn: 起始页号
 * @ end_pfn: 结束页号
*/
static void __init bootmem_init_node(int node, struct meminfo *mi,
	unsigned long start_pfn, unsigned long end_pfn)
{
	unsigned long boot_pfn;
	unsigned int boot_pages;
	pg_data_t *pgdat;
	int i;

	/*
	 * Map the memory banks for this node.
	 * 为node中内核空间内存建立页表映射
	 */
	for_each_nodebank(i, mi, node) {
		struct membank *bank = &mi->bank[i];

		if (!bank->highmem)
			map_memory_bank(bank);
	}

	/*
	 * 计算出管理[start_pfn, end_pfn]的页框管理位图空间大小(单位:页)
	 */
	boot_pages = bootmem_bootmap_pages(end_pfn - start_pfn);
	boot_pfn = find_bootmap_pfn(node, mi, boot_pages);

	node_set_online(node);
	/*返回node结构*/
	pgdat = NODE_DATA(node);
	/**
	 * 将start_pfn ～end_pfn之间的页框加入到node的bootmem内存分配器中
	 * 即初始化node的bootmem内存分配器
	 */
	init_bootmem_node(pgdat, boot_pfn, start_pfn, end_pfn);

	/* 清空node中的所有bootmem分配器管理物理内存的位图
	 * 即初始化bootmem分配器的所有页框使用位图为空闲
	 */
	for_each_nodebank(i, mi, node) {
		struct membank *bank = &mi->bank[i];
		if (!bank->highmem)
			free_bootmem_node(pgdat, bank_phys_start(bank), bank_phys_size(bank));
	}

	/*
	 * 设置node中的bootmem的页框管理位图的页框位图
	 */
	reserve_bootmem_node(pgdat, boot_pfn << PAGE_SHIFT,
			     boot_pages << PAGE_SHIFT, BOOTMEM_DEFAULT);
}

/**
 * bootmem_reserve_initrd: 保留ramdisk的物理空间*/
static void __init bootmem_reserve_initrd(int node)
{
#ifdef CONFIG_BLK_DEV_INITRD
	pg_data_t *pgdat = NODE_DATA(node);
	int res;

	res = reserve_bootmem_node(pgdat, phys_initrd_start,
			     phys_initrd_size, BOOTMEM_EXCLUSIVE);

	/*计算得到ramdisk物理空间的起始与结束地址*/
	if (res == 0) {
		initrd_start = __phys_to_virt(phys_initrd_start);
		initrd_end = initrd_start + phys_initrd_size;
	} else {
		printk(KERN_ERR
			"INITRD: 0x%08lx+0x%08lx overlaps in-use "
			"memory region - disabling initrd\n",
			phys_initrd_start, phys_initrd_size);
	}
#endif
}

/**
 * bootmem_init_zone: 初始化node的zone管理结构
 * @ node: node id
 * @ mi: memory bank数据结构
*/
static void __init bootmem_init_zone(int node, struct meminfo *mi)
{
	unsigned long zone_size[MAX_NR_ZONES], zhole_size[MAX_NR_ZONES];
	unsigned long min, max_low, max_high;
	int i;

	/*查找node中的最小页号和最大页号*/
	find_node_limits(node, mi, &min, &max_low, &max_high);
	memset(zone_size, 0, sizeof(zone_size));

	/*
	 * 计算出每个区包含的页数
	 * MAX_NR_ZONES=2,只有两个区
	 */
	zone_size[0] = max_low - min; /*计算低端内存的页数*/
#ifdef CONFIG_HIGHMEM
	zone_size[ZONE_HIGHMEM] = max_high - max_low; /*计算高端内存的页数*/
#endif

	/*
	 * For each bank in this node, calculate the size of the holes.
	 *  holes = node_size - sum(bank_sizes_in_node)
	 *  计算出每一个node所含的holes的页数 假设：
	 *  该node中包含8个pages 即zone_size[idx] = zhole_size[idx] = 8,
	 *  其中，每一个bank的大小为2页，总共有两个banks,如下图所示：
	 *  s------s-------------e---------------------------s--------------e
	 *  |      |      |      |      |      |      |      |      |       |
	 *  |  0      1      2       3      4      5     6       7      8
	 *  |      |      |      |      |      |      |      |      |       |
	 *  |-------------|----------------------------------|--------------|
	 *         |    bank[0]  |                           |    bank[1]   |
	 *  min = 0, max_low = 8;
	 *  zhole_size = 9 - 2 * pages of bank[i] (i = 0, 1)
	 *  zhole_size = 9 - 4 = 5  
	 */
	memcpy(zhole_size, zone_size, sizeof(zhole_size));
	/*计算出区中包含Hole页数*/
	for_each_nodebank(i, mi, node) {
		int idx = 0;
#ifdef CONFIG_HIGHMEM
		if (mi->bank[i].highmem)
			idx = ZONE_HIGHMEM;
#endif
		zhole_size[idx] -= bank_pfn_size(&mi->bank[i]);
	}

	/*
	 * Adjust the sizes according to any special requirements for
	 * this machine type.
	 */
	arch_adjust_zones(node, zone_size, zhole_size);

	free_area_init_node(node, zone_size, min, zhole_size);
}

#ifndef CONFIG_SPARSEMEM
int pfn_valid(unsigned long pfn)
{
	struct meminfo *mi = &meminfo;
	unsigned int left = 0, right = mi->nr_banks;

	do {
		unsigned int mid = (right + left) / 2;
		struct membank *bank = &mi->bank[mid];

		if (pfn < bank_pfn_start(bank))
			right = mid;
		else if (pfn >= bank_pfn_end(bank))
			left = mid + 1;
		else
			return 1;
	} while (left < right);
	return 0;
}
EXPORT_SYMBOL(pfn_valid);

static void arm_memory_present(struct meminfo *mi, int node)
{
}
#else
static void arm_memory_present(struct meminfo *mi, int node)
{
	int i;
	for_each_nodebank(i, mi, node) {
		struct membank *bank = &mi->bank[i];
		memory_present(node, bank_pfn_start(bank), bank_pfn_end(bank));
	}
}
#endif

static int __init meminfo_cmp(const void *_a, const void *_b)
{
	const struct membank *a = _a, *b = _b;
	long cmp = bank_pfn_start(a) - bank_pfn_start(b);
	return cmp < 0 ? -1 : cmp > 0 ? 1 : 0;
}

void __init bootmem_init(void)
{
	struct meminfo *mi = &meminfo;
	unsigned long min, max_low, max_high;
	int node, initrd_node, i;


	printk("system ram : %d banks\n",mi->nr_banks);
	for (i = 0; i < mi->nr_banks; i++) {
		printk("mem bank[%d] node id : %d  start: 0x%x size :%d\n",
		 i, mi->bank[i].node,mi->bank[i].start, mi->bank[i].size);
	}

	sort(&mi->bank, mi->nr_banks, sizeof(mi->bank[0]), meminfo_cmp, NULL);

	/*
	 * Locate which node contains the ramdisk image, if any.
	 */
	initrd_node = check_initrd(mi);

	max_low = max_high = 0;

	/**
	 * 1.建立每一个Node的低端内存的页表映射
	 * 2.初始化每一个Node的bootmem分配器
	 * 3.初始化每一个node的保留页框的信息
	*/
	for_each_node(node) {
		unsigned long node_low, node_high;
		/*找到node物理内存中的最小页号，最大低端内存页号与高端内存页号*/
		find_node_limits(node, mi, &min, &node_low, &node_high);

		/*保存最大低端内存页帧号与高端内存页帧号*/
		if (node_low > max_low)
			max_low = node_low;
		if (node_high > max_high)
			max_high = node_high;

		/*
		 * If there is no memory in this node, ignore it.
		 * (We can't have nodes which have no lowmem)
		 */
		if (node_low == 0)
			continue;

		/*为node 的min ~node_low之间的低端内存页框建立页表
		 * 以及初始化node的bootmem内存分配器*/
		bootmem_init_node(node, mi, min, node_low);

		/*
		 * Reserve any special node zero regions.
		 */
		if (node == 0)
			reserve_node_zero(NODE_DATA(node));

		/*
		 * If the initrd is in this node, reserve its memory.
		 */
		if (node == initrd_node)
			bootmem_reserve_initrd(node);

		/*
		 * Sparsemem tries to allocate bootmem in memory_present(),
		 * so must be done after the fixed reservations
		 */
		arm_memory_present(mi, node);
	}

	/*
	 * sparse_init() needs the bootmem allocator up and running.
	 */
	sparse_init();

	/*初始化每一个node的区管理结构*/
	for_each_node(node)
		bootmem_init_zone(node, mi);

	/*设置高端内存的起始线性地址*/
	high_memory = __va((max_low << PAGE_SHIFT) - 1) + 1;

	/*
	 * This doesn't seem to be used by the Linux memory manager any
	 * more, but is used by ll_rw_block.  If we can get rid of it, we
	 * also get rid of some of the stuff above as well.
	 *
	 * Note: max_low_pfn and max_pfn reflect the number of _pages_ in
	 * the system, not the maximum PFN.
	 */
	/*最大低端内存的物理页数*/
	max_low_pfn = max_low - PHYS_PFN_OFFSET;
	/*所有内存的物理页数*/
	max_pfn = max_high - PHYS_PFN_OFFSET;
}

static inline int free_area(unsigned long pfn, unsigned long end, char *s)
{
	unsigned int pages = 0, size = (end - pfn) << (PAGE_SHIFT - 10);

	for (; pfn < end; pfn++) {
		struct page *page = pfn_to_page(pfn);
		ClearPageReserved(page);
		init_page_count(page);
		__free_page(page);
		pages++;
	}

	if (size && s)
		printk(KERN_INFO "Freeing %s memory: %dK\n", s, size);

	return pages;
}

/**
 * free_memmap: 清空start_pfn ~ end_pfn之间的bootmem的管理位图
 * @ node:node id
 * @ start_pfn: node中的起始页号
 * @ end_pfn:node中的结束页号
*/
static inline void
free_memmap(int node, unsigned long start_pfn, unsigned long end_pfn)
{
	struct page *start_pg, *end_pg;
	unsigned long pg, pgend;

	/*
	 * Convert start_pfn/end_pfn to a struct page pointer.
	 */
	start_pg = pfn_to_page(start_pfn - 1) + 1; /*为什么加1*/
	end_pg = pfn_to_page(end_pfn);

	/*
	 * Convert to physical addresses, and
	 * round start upwards and end downwards.
	 */
	pg = PAGE_ALIGN(__pa(start_pg));
	pgend = __pa(end_pg) & PAGE_MASK;

	/*
	 * If there are free pages between these,
	 * free the section of the memmap array.
	 */
	if (pg < pgend)
		free_bootmem_node(NODE_DATA(node), pg, pgend - pg);
}

/**
 * free_unused_memmap_node:清空node中bootmem管理位图
 * @ node: node id
 * @ mi:
 * The mem_map array can get very big.  Free the unused area of the memory map.
 */
static void __init free_unused_memmap_node(int node, struct meminfo *mi)
{
	unsigned long bank_start, prev_bank_end = 0;
	unsigned int i;

	/*
	 * [FIXME] This relies on each bank being in address order.  This
	 * may not be the case, especially if the user has provided the
	 * information on the command line.
	 */
	for_each_nodebank(i, mi, node) {
		struct membank *bank = &mi->bank[i];

		bank_start = bank_pfn_start(bank);
		if (bank_start < prev_bank_end) {
			printk(KERN_ERR "MEM: unordered memory banks.  "
				"Not freeing memmap.\n");
			break;
		}

		/*
		 * If we had a previous bank, and there is a space
		 * between the current bank and the previous, free it.
		 */
		if (prev_bank_end && prev_bank_end != bank_start)
			free_memmap(node, prev_bank_end, bank_start);

		prev_bank_end = bank_pfn_end(bank);
	}
}

/*
 * mem_init() marks the free areas in the mem_map and tells us how much
 * memory is free.  This is done after various parts of the system have
 * claimed their memory after the kernel image.
 */
void __init mem_init(void)
{
	unsigned int codesize, datasize, initsize;
	int i, node;

#ifndef CONFIG_DISCONTIGMEM
	max_mapnr   = pfn_to_page(max_pfn + PHYS_PFN_OFFSET) - mem_map;
#endif

	/* this will put all unused low memory onto the freelists */
	for_each_online_node(node) {
		pg_data_t *pgdat = NODE_DATA(node);

		free_unused_memmap_node(node, &meminfo);

		if (pgdat->node_spanned_pages != 0)
			totalram_pages += free_all_bootmem_node(pgdat);
	}

#ifdef CONFIG_SA1111
	/* now that our DMA memory is actually so designated, we can free it */
	totalram_pages += free_area(PHYS_PFN_OFFSET,
				    __phys_to_pfn(__pa(swapper_pg_dir)), NULL);
#endif

#ifdef CONFIG_HIGHMEM
	/* set highmem page free */
	for_each_online_node(node) {
		for_each_nodebank (i, &meminfo, node) {
			unsigned long start = bank_pfn_start(&meminfo.bank[i]);
			unsigned long end = bank_pfn_end(&meminfo.bank[i]);
			if (start >= max_low_pfn + PHYS_PFN_OFFSET)
				totalhigh_pages += free_area(start, end, NULL);
		}
	}
	totalram_pages += totalhigh_pages;
#endif

	/*
	 * Since our memory may not be contiguous, calculate the
	 * real number of pages we have in this system
	 */
	printk(KERN_INFO "Memory:");
	num_physpages = 0;
	for (i = 0; i < meminfo.nr_banks; i++) {
		num_physpages += bank_pfn_size(&meminfo.bank[i]);
		printk(" %ldMB", bank_phys_size(&meminfo.bank[i]) >> 20);
	}
	printk(" = %luMB total\n", num_physpages >> (20 - PAGE_SHIFT));

	codesize = _etext - _text;
	datasize = _end - _data;
	initsize = __init_end - __init_begin;

	printk(KERN_NOTICE "Memory: %luKB available (%dK code, "
		"%dK data, %dK init, %luK highmem)\n",
		nr_free_pages() << (PAGE_SHIFT-10), codesize >> 10,
		datasize >> 10, initsize >> 10,
		(unsigned long) (totalhigh_pages << (PAGE_SHIFT-10)));

	if (PAGE_SIZE >= 16384 && num_physpages <= 128) {
		extern int sysctl_overcommit_memory;
		/*
		 * On a machine this small we won't get
		 * anywhere without overcommit, so turn
		 * it on by default.
		 */
		sysctl_overcommit_memory = OVERCOMMIT_ALWAYS;
	}
}

void free_initmem(void)
{
#ifdef CONFIG_HAVE_TCM
	extern char *__tcm_start, *__tcm_end;

	totalram_pages += free_area(__phys_to_pfn(__pa(__tcm_start)),
				    __phys_to_pfn(__pa(__tcm_end)),
				    "TCM link");
#endif

	if (!machine_is_integrator() && !machine_is_cintegrator())
		totalram_pages += free_area(__phys_to_pfn(__pa(__init_begin)),
					    __phys_to_pfn(__pa(__init_end)),
					    "init");
}

#ifdef CONFIG_BLK_DEV_INITRD

static int keep_initrd;

void free_initrd_mem(unsigned long start, unsigned long end)
{
	if (!keep_initrd)
		totalram_pages += free_area(__phys_to_pfn(__pa(start)),
					    __phys_to_pfn(__pa(end)),
					    "initrd");
}

static int __init keepinitrd_setup(char *__unused)
{
	keep_initrd = 1;
	return 1;
}

__setup("keepinitrd", keepinitrd_setup);
#endif
