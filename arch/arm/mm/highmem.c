/*
 * arch/arm/mm/highmem.c -- ARM highmem support
 *
 * Author:	Nicolas Pitre
 * Created:	september 8, 2008
 * Copyright:	Marvell Semiconductors Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/highmem.h>
#include <linux/interrupt.h>
#include <asm/fixmap.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>
#include "mm.h"

/**
 * kmap: 建立永久性高端内存映射
 * @ page:需要映射的高端内存页框
 * @ return:返回页框映射后的内核线性地址
*/
void *kmap(struct page *page)
{
	might_sleep();
	if (!PageHighMem(page))
		return page_address(page);
	return kmap_high(page);
}
EXPORT_SYMBOL(kmap);

/**
 * kunmap:取消高端内存页框page的映射
 */
void kunmap(struct page *page)
{
	BUG_ON(in_interrupt());
	if (!PageHighMem(page))
		return;
	kunmap_high(page);
}
EXPORT_SYMBOL(kunmap);

/**
 * kmap_atomic: 建立临时高端内存映射
 * @ page:需要映射的高端内存页框
 * @ type:指定固定映射线性地址的下标索引
 * @ return:返回高端内存页框映射后的线性地址
*/
void *kmap_atomic(struct page *page, enum km_type type)
{
	unsigned int idx;
	unsigned long vaddr;
	void *kmap;

	pagefault_disable();
	if (!PageHighMem(page))
		return page_address(page);

	debug_kmap_atomic(type);

	kmap = kmap_high_get(page);
	if (kmap)
		return kmap;

	/*通过type以及CPU编号计算出临时映射的线性地址
	 * 并且将该线性地址作为page映射后的线性地址*/
	idx = type + KM_TYPE_NR * smp_processor_id();
	vaddr = __fix_to_virt(FIX_KMAP_BEGIN + idx);
#ifdef CONFIG_DEBUG_HIGHMEM
	/*
	 * With debugging enabled, kunmap_atomic forces that entry to 0.
	 * Make sure it was indeed properly unmapped.
	 */
	BUG_ON(!pte_none(*(TOP_PTE(vaddr))));
#endif
	/*将临时映射的页表项放置在顶端的页目录的页表项之中
	 * 因为固定映射的所有页表都放置在线性空间的第4个GB
	 * 的最后的位置*/
	set_pte_ext(TOP_PTE(vaddr), mk_pte(page, kmap_prot), 0);
	/*
	 * When debugging is off, kunmap_atomic leaves the previous mapping
	 * in place, so this TLB flush ensures the TLB is updated with the
	 * new mapping.
	 */
	local_flush_tlb_kernel_page(vaddr);

	return (void *)vaddr;
}
EXPORT_SYMBOL(kmap_atomic);

/**
 * kunmap_atomic:取消kvaddr的临时映射
 * @ kvaddr:高端内存页框映射的内核线性地址
 * @ type:映射的类型
*/
void kunmap_atomic(void *kvaddr, enum km_type type)
{
	/*vaddr地址页对齐*/
	unsigned long vaddr = (unsigned long) kvaddr & PAGE_MASK;
	/*计算出type类型的被映射的线性地址*/
	unsigned int idx = type + KM_TYPE_NR * smp_processor_id();

	if (kvaddr >= (void *)FIXADDR_START) {
		__cpuc_flush_dcache_page((void *)vaddr);
#ifdef CONFIG_DEBUG_HIGHMEM
		BUG_ON(vaddr != __fix_to_virt(FIX_KMAP_BEGIN + idx));
		set_pte_ext(TOP_PTE(vaddr), __pte(0), 0);
		local_flush_tlb_kernel_page(vaddr);
#else
		(void) idx;  /* to kill a warning */
#endif
	} else if (vaddr >= PKMAP_ADDR(0) && vaddr < PKMAP_ADDR(LAST_PKMAP)) {
		/* this address was obtained through kmap_high_get() */
		kunmap_high(pte_page(pkmap_page_table[PKMAP_NR(vaddr)]));
	}
	pagefault_enable();
}
EXPORT_SYMBOL(kunmap_atomic);

/**
 * kmap_atomic_pfn:为pfn所指的页框建立临时性内核映射
 * @ pfn:页框号
 * @ type:建立的映射类型
 */
void *kmap_atomic_pfn(unsigned long pfn, enum km_type type)
{
	unsigned int idx;
	unsigned long vaddr;

	pagefault_disable();

	idx = type + KM_TYPE_NR * smp_processor_id();
	vaddr = __fix_to_virt(FIX_KMAP_BEGIN + idx);
#ifdef CONFIG_DEBUG_HIGHMEM
	BUG_ON(!pte_none(*(TOP_PTE(vaddr))));
#endif
	set_pte_ext(TOP_PTE(vaddr), pfn_pte(pfn, kmap_prot), 0);
	local_flush_tlb_kernel_page(vaddr);

	return (void *)vaddr;
}

/**
 * kmap_atomic_to_page:返回ptr线性地址所对应的物理页框
 * @ ptr:内核线性地址
 */
struct page *kmap_atomic_to_page(const void *ptr)
{
	unsigned long vaddr = (unsigned long)ptr;
	pte_t *pte;

	if (vaddr < FIXADDR_START)
		return virt_to_page(ptr);

	pte = TOP_PTE(vaddr);
	return pte_page(*pte);
}
