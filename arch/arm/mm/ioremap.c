/*
 *  linux/arch/arm/mm/ioremap.c
 *
 * Re-map IO memory to kernel address space so that we can access it.
 *
 * (C) Copyright 1995 1996 Linus Torvalds
 *
 * Hacked for ARM by Phil Blundell <philb@gnu.org>
 * Hacked to allow all architectures to build, and various cleanups
 * by Russell King
 *
 * This allows a driver to remap an arbitrary region of bus memory into
 * virtual space.  One should *only* use readl, writel, memcpy_toio and
 * so on with such remapped areas.
 *
 * Because the ARM only has a 32-bit address space we can't address the
 * whole of the (physical) PCI space at once.  PCI huge-mode addressing
 * allows us to circumvent this restriction by splitting PCI space into
 * two 2GB chunks and mapping only one at a time into processor memory.
 * We use MMU protection domains to trap any attempt to access the bank
 * that is not currently mapped.  (This isn't fully implemented yet.)
 */
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/io.h>

#include <asm/cputype.h>
#include <asm/cacheflush.h>
#include <asm/mmu_context.h>
#include <asm/pgalloc.h>
#include <asm/tlbflush.h>
#include <asm/sizes.h>

#include <asm/mach/map.h>
#include "mm.h"

/*
 * Used by ioremap() and iounmap() code to mark (super)section-mapped
 * I/O regions in vm_struct->flags field.
 */
#define VM_ARM_SECTION_MAPPING	0x80000000

/*remap_area_pte:映射pmd的页表项*/
static int remap_area_pte(pmd_t *pmd, unsigned long addr, unsigned long end,
			  unsigned long phys_addr, const struct mem_type *type)
{
	pgprot_t prot = __pgprot(type->prot_pte);
	pte_t *pte;

	/*给pmd申请页表项空间*/
	pte = pte_alloc_kernel(pmd, addr);
	if (!pte)
		return -ENOMEM;

	do {
		if (!pte_none(*pte))
			goto bad;

		/*填充页表项,使其指向映射的一页物理内存地址处*/
		set_pte_ext(pte, pfn_pte(phys_addr >> PAGE_SHIFT, prot), 0);
		phys_addr += PAGE_SIZE; /*移动物理内存地址到下一个物理页框地址处*/
	} while (pte++, addr += PAGE_SIZE, addr != end);
	return 0;

 bad:
	printk(KERN_CRIT "remap_area_pte: page already exists\n");
	BUG();
}

/*remap_area_pmd:映射pgd的页中间目录项*/
static inline int remap_area_pmd(pgd_t *pgd, unsigned long addr,
				 unsigned long end, unsigned long phys_addr,
				 const struct mem_type *type)
{
	unsigned long next;
	pmd_t *pmd;
	int ret = 0;

	/*给pgd申请页中间目录项空间*/
	pmd = pmd_alloc(&init_mm, pgd, addr);
	if (!pmd)
		return -ENOMEM;

	do {
		/*计算出下一个pmd的地址*/
		next = pmd_addr_end(addr, end);
		ret = remap_area_pte(pmd, addr, next, phys_addr, type);
		if (ret)
			return ret;
		phys_addr += next - addr; /*移动到下一个pmd管理的物理内存的起始地址处*/
	} while (pmd++, addr = next, addr != end);
	return ret;
}

/*remap_area_pages:pfn的物理空间与[start,end]之间的虚拟地址建立页表的映射关系*/
static int remap_area_pages(unsigned long start, unsigned long pfn,
			    size_t size, const struct mem_type *type)
{
	unsigned long addr = start;
	unsigned long next, end = start + size;
	unsigned long phys_addr = __pfn_to_phys(pfn);
	pgd_t *pgd;
	int err = 0;

	BUG_ON(addr >= end);
	/*找到addr所属的页全局目录项指针*/
	pgd = pgd_offset_k(addr);
	do {
		/*下一个页目录项的地址*/
		next = pgd_addr_end(addr, end);
		err = remap_area_pmd(pgd, addr, next, phys_addr, type);
		if (err)
			break;
		phys_addr += next - addr; /*移动到下一个pgd管理的物理内存起始地址处*/
	} while (pgd++, addr = next, addr != end);

	return err;
}

int ioremap_page(unsigned long virt, unsigned long phys,
		 const struct mem_type *mtype)
{
	return remap_area_pages(virt, __phys_to_pfn(phys), PAGE_SIZE, mtype);
}
EXPORT_SYMBOL(ioremap_page);

/*__check_kvm_seq:类似于顺序锁*/
void __check_kvm_seq(struct mm_struct *mm)
{
	unsigned int seq;

	do {
		seq = init_mm.context.kvm_seq;
		memcpy(pgd_offset(mm, VMALLOC_START),
		       pgd_offset_k(VMALLOC_START),
		       sizeof(pgd_t) * (pgd_index(VMALLOC_END) -
					pgd_index(VMALLOC_START)));
		mm->context.kvm_seq = seq;
	} while (seq != init_mm.context.kvm_seq);
}

#ifndef CONFIG_SMP
/*
 * Section support is unsafe on SMP - If you iounmap and ioremap a region,
 * the other CPUs will not see this change until their next context switch.
 * Meanwhile, (eg) if an interrupt comes in on one of those other CPUs
 * which requires the new ioremap'd region to be referenced, the CPU will
 * reference the _old_ region.
 *
 * Note that get_vm_area() allocates a guard 4K page, so we need to mask
 * the size back to 1MB aligned or we will overflow in the loop below.
 */
/*unmap_area_sections:取消virt虚拟内存区域的映射*/
static void unmap_area_sections(unsigned long virt, unsigned long size)
{
	unsigned long addr = virt, end = virt + (size & ~(SZ_1M - 1));
	pgd_t *pgd;

	flush_cache_vunmap(addr, end);
	pgd = pgd_offset_k(addr);
	do {
		pmd_t pmd, *pmdp = pmd_offset(pgd, addr);

		pmd = *pmdp;
		if (!pmd_none(pmd)) {
			/*
			 * Clear the PMD from the page table, and
			 * increment the kvm sequence so others
			 * notice this change.
			 *
			 * Note: this is still racy on SMP machines.
			 */
			pmd_clear(pmdp);
			init_mm.context.kvm_seq++;

			/*
			 * Free the page table, if there was one.
			 */
			if ((pmd_val(pmd) & PMD_TYPE_MASK) == PMD_TYPE_TABLE)
				pte_free_kernel(&init_mm, pmd_page_vaddr(pmd));
		}

		addr += PGDIR_SIZE;
		pgd++;
	} while (addr < end);

	/*
	 * Ensure that the active_mm is up to date - we want to
	 * catch any use-after-iounmap cases.
	 */
	if (current->active_mm->context.kvm_seq != init_mm.context.kvm_seq)
		__check_kvm_seq(current->active_mm);

	flush_tlb_kernel_range(virt, end);
}

/*remap_area_sections:虚拟内存的进行段式映射*/
static int
remap_area_sections(unsigned long virt, unsigned long pfn,
		    size_t size, const struct mem_type *type)
{
	unsigned long addr = virt, end = virt + size;
	pgd_t *pgd;

	/*
	 * Remove and free any PTE-based mapping, and
	 * sync the current kernel mapping.
	 */
	unmap_area_sections(virt, size);

	/*一个pmd映射2MB的物理空间*/
	pgd = pgd_offset_k(addr);
	do {
		pmd_t *pmd = pmd_offset(pgd, addr);

		pmd[0] = __pmd(__pfn_to_phys(pfn) | type->prot_sect);
		pfn += SZ_1M >> PAGE_SHIFT;
		pmd[1] = __pmd(__pfn_to_phys(pfn) | type->prot_sect);
		pfn += SZ_1M >> PAGE_SHIFT;
		flush_pmd_entry(pmd);

		addr += PGDIR_SIZE;
		pgd++;
	} while (addr < end);

	return 0;
}

static int
remap_area_supersections(unsigned long virt, unsigned long pfn,
			 size_t size, const struct mem_type *type)
{
	unsigned long addr = virt, end = virt + size;
	pgd_t *pgd;

	/*
	 * Remove and free any PTE-based mapping, and
	 * sync the current kernel mapping.
	 */
	unmap_area_sections(virt, size);

	pgd = pgd_offset_k(virt);
	do {
		unsigned long super_pmd_val, i;

		super_pmd_val = __pfn_to_phys(pfn) | type->prot_sect |
				PMD_SECT_SUPER;
		super_pmd_val |= ((pfn >> (32 - PAGE_SHIFT)) & 0xf) << 20;

		for (i = 0; i < 8; i++) {
			pmd_t *pmd = pmd_offset(pgd, addr);

			pmd[0] = __pmd(super_pmd_val);
			pmd[1] = __pmd(super_pmd_val);
			flush_pmd_entry(pmd);

			addr += PGDIR_SIZE;
			pgd++;
		}

		pfn += SUPERSECTION_SIZE >> PAGE_SHIFT;
	} while (addr < end);

	return 0;
}
#endif


/*__arm_ioremap_pfn:映射内核虚拟地址空间的页管理结构
 * @ pfn:物理页号,内核虚拟地址空间被映射的物理空间的起始页号
 * @ offset:是pfn的页对齐的边界地址
 * @ size:映射物理空间的大小
 * 注:当内核需要访问不属于自己线性地址空间的内存时，需要映射到内核
 *    的虚拟地址空间中
*/
void __iomem *
__arm_ioremap_pfn(unsigned long pfn, unsigned long offset, size_t size,
		  unsigned int mtype)
{
	const struct mem_type *type;
	int err;
	unsigned long addr;
 	struct vm_struct * area;

	/*
	 * High mappings must be supersection aligned
	 */
	if (pfn >= 0x100000 && (__pfn_to_phys(pfn) & ~SUPERSECTION_MASK))
		return NULL;

	type = get_mem_type(mtype);
	if (!type)
		return NULL;

	/*
	 * Page align the mapping size, taking account of any offset.
	 */
	size = PAGE_ALIGN(offset + size);

	/*step:1找到size大小的内核虚拟地址空间*/
 	area = get_vm_area(size, VM_IOREMAP);
 	if (!area)
 		return NULL;
 	addr = (unsigned long)area->addr;

	/*step2:将物理空间映射到内核虚拟地址空间
	 * 通过内核页表管理完成*/
#ifndef CONFIG_SMP
	if (DOMAIN_IO == 0 &&
	    (((cpu_architecture() >= CPU_ARCH_ARMv6) && (get_cr() & CR_XP)) ||
	       cpu_is_xsc3()) && pfn >= 0x100000 &&
	       !((__pfn_to_phys(pfn) | size | addr) & ~SUPERSECTION_MASK)) {
		area->flags |= VM_ARM_SECTION_MAPPING;
		err = remap_area_supersections(addr, pfn, size, type);
	} else if (!((__pfn_to_phys(pfn) | size | addr) & ~PMD_MASK)) {
		area->flags |= VM_ARM_SECTION_MAPPING;
		err = remap_area_sections(addr, pfn, size, type);
	} else
#endif
		err = remap_area_pages(addr, pfn, size, type);

	if (err) {
 		vunmap((void *)addr);
 		return NULL;
 	}

	flush_cache_vmap(addr, addr + size);
	return (void __iomem *) (offset + addr);
}
EXPORT_SYMBOL(__arm_ioremap_pfn);

/*__arm_ioremap:映射size大小的物理内存到内核虚拟地址空间*/
void __iomem *
__arm_ioremap(unsigned long phys_addr, size_t size, unsigned int mtype)
{
	unsigned long last_addr;
 	unsigned long offset = phys_addr & ~PAGE_MASK;
 	unsigned long pfn = __phys_to_pfn(phys_addr);

 	/*
 	 * Don't allow wraparound or zero size
	 */
	last_addr = phys_addr + size - 1;
	if (!size || last_addr < phys_addr)
		return NULL;

 	return __arm_ioremap_pfn(pfn, offset, size, mtype);
}
EXPORT_SYMBOL(__arm_ioremap);

void __iounmap(volatile void __iomem *io_addr)
{
	void *addr = (void *)(PAGE_MASK & (unsigned long)io_addr);
#ifndef CONFIG_SMP
	struct vm_struct **p, *tmp;

	/*
	 * If this is a section based mapping we need to handle it
	 * specially as the VM subsystem does not know how to handle
	 * such a beast. We need the lock here b/c we need to clear
	 * all the mappings before the area can be reclaimed
	 * by someone else.
	 */
	write_lock(&vmlist_lock);
	for (p = &vmlist ; (tmp = *p) ; p = &tmp->next) {
		if ((tmp->flags & VM_IOREMAP) && (tmp->addr == addr)) {
			if (tmp->flags & VM_ARM_SECTION_MAPPING) {
				unmap_area_sections((unsigned long)tmp->addr,
						    tmp->size);
			}
			break;
		}
	}
	write_unlock(&vmlist_lock);
#endif

	/*删除addr虚拟内存空间*/
	vunmap(addr);
}
EXPORT_SYMBOL(__iounmap);
