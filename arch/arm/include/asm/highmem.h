#ifndef _ASM_HIGHMEM_H
#define _ASM_HIGHMEM_H

#include <asm/kmap_types.h>

/*高端内存永久性映射的内核线性地址空间的基地址*/
#define PKMAP_BASE		(PAGE_OFFSET - PMD_SIZE)
/*高端内存永久性映射的页表项个数,
 * 因此内核可以访问的高端内存的空间
 * 为LAST_PKMAP * PAGE_SIZE*/
#define LAST_PKMAP		PTRS_PER_PTE
#define LAST_PKMAP_MASK		(LAST_PKMAP - 1)
/*通过虚拟地址计算得到页表项的索引*/
#define PKMAP_NR(virt)		(((virt) - PKMAP_BASE) >> PAGE_SHIFT)
/*计算nr页号的高端内存永久性映射的线性地址*/
#define PKMAP_ADDR(nr)		(PKMAP_BASE + ((nr) << PAGE_SHIFT))

#define kmap_prot		PAGE_KERNEL

#define flush_cache_kmaps()	flush_cache_all()

extern pte_t *pkmap_page_table;

#define ARCH_NEEDS_KMAP_HIGH_GET

extern void *kmap_high(struct page *page);
extern void *kmap_high_get(struct page *page);
extern void kunmap_high(struct page *page);

extern void *kmap(struct page *page);
extern void kunmap(struct page *page);
extern void *kmap_atomic(struct page *page, enum km_type type);
extern void kunmap_atomic(void *kvaddr, enum km_type type);
extern void *kmap_atomic_pfn(unsigned long pfn, enum km_type type);
extern struct page *kmap_atomic_to_page(const void *ptr);

#endif
