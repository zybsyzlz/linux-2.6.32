/*
 * OHCI HCD (Host Controller Driver) for USB.
 *
 * (C) Copyright 1999 Roman Weissgaerber <weissg@vienna.at>
 * (C) Copyright 2000-2002 David Brownell <dbrownell@users.sourceforge.net>
 *
 * This file is licenced under the GPL.
 */

static void ohci_hcd_init (struct ohci_hcd *ohci)
{
	ohci->next_statechange = jiffies;
	spin_lock_init (&ohci->lock);
	INIT_LIST_HEAD (&ohci->pending);
}

/**
 * ohci_mem_init:ochi主机的内存初始化
 * @ ochi:主机结构
*/
static int ohci_mem_init (struct ohci_hcd *ohci)
{
	ohci->td_cache = dma_pool_create ("ohci_td",
		ohci_to_hcd(ohci)->self.controller,
		sizeof (struct td),
		32 /* byte alignment */,
		0 /* no page-crossing issues */);
	if (!ohci->td_cache)
		return -ENOMEM;
	ohci->ed_cache = dma_pool_create ("ohci_ed",
		ohci_to_hcd(ohci)->self.controller,
		sizeof (struct ed),
		16 /* byte alignment */,
		0 /* no page-crossing issues */);
	if (!ohci->ed_cache) {
		dma_pool_destroy (ohci->td_cache);
		return -ENOMEM;
	}
	return 0;
}
/**
 * ohci_mem_cleanup : 释放ohci主机的ED TD 缓存区内存
*/
static void ohci_mem_cleanup (struct ohci_hcd *ohci)
{
	if (ohci->td_cache) {
		dma_pool_destroy (ohci->td_cache);
		ohci->td_cache = NULL;
	}
	if (ohci->ed_cache) {
		dma_pool_destroy (ohci->ed_cache);
		ohci->ed_cache = NULL;
	}
}

/* ohci "done list" processing needs this mapping */
static inline struct td *
dma_to_td (struct ohci_hcd *hc, dma_addr_t td_dma)
{
	struct td *td;

	td_dma &= TD_MASK;  /*32 字节对齐*/
	td = hc->td_hash [TD_HASH_FUNC(td_dma)];
	while (td && td->td_dma != td_dma)
		td = td->td_hash;
	return td;
}

/**
 * td_alloc: 从主机的td_cache DMA缓存中分配一个td的内存结构
 * @ hc:ohci主机结构
 * @ mem_flags:内存分配的标记
*/
static struct td *td_alloc (struct ohci_hcd *hc, gfp_t mem_flags)
{
	dma_addr_t	dma;
	struct td	*td;

	/*从td_chache 的DMA缓存池中分配一个td的内存块*/
	td = dma_pool_alloc (hc->td_cache, mem_flags, &dma);
	if (td) {
		/* in case hc fetches it, make it look dead */
		memset (td, 0, sizeof *td);
		td->hwNextTD = cpu_to_hc32 (hc, dma);
		/*将DMA返回的总线地址填入到td的dma结构中*/
		td->td_dma = dma;
		/* hashed in td_fill */
	}
	return td;
}

/**
 * td_free:删除td,并释放其结构
*/
static void td_free (struct ohci_hcd *hc, struct td *td)
{
	struct td **prev = &hc->td_hash [TD_HASH_FUNC (td->td_dma)];

	while (*prev && *prev != td)
		prev = &(*prev)->td_hash;
	if (*prev)
		*prev = td->td_hash;  /*删除td所指的结构*/
	else if ((td->hwINFO & cpu_to_hc32(hc, TD_DONE)) != 0)
		ohci_dbg (hc, "no hash for td %p\n", td);
	/*将td结构返回给ed_cache DMA缓存池*/
	dma_pool_free (hc->td_cache, td, td->td_dma);
}

/**
 * ed_alloc:申请一个ed结构并初始化 
 * @ hc:ohci主机结构
 * @ mem_flags:内存分配的标记
*/
static struct ed *ed_alloc (struct ohci_hcd *hc, gfp_t mem_flags)
{
	dma_addr_t	dma;
	struct ed	*ed;

	/*从主机的ed_cache DMA缓存中分配一个ed的内存结构*/
	ed = dma_pool_alloc (hc->ed_cache, mem_flags, &dma);
	if (ed) {
		memset (ed, 0, sizeof (*ed));
		INIT_LIST_HEAD (&ed->td_list);
		/*将DMA返回的总线地址填入到ed的dma结构中*/
		ed->dma = dma;
	}
	return ed;
}
/**
 * ed_free:释放ed结构
 * @hc :主机结构
 * @ed :待释放的ed结构
*/
static void ed_free (struct ohci_hcd *hc, struct ed *ed)
{
	/*将ed结构返回给ed_cache DMA缓存池*/
	dma_pool_free (hc->ed_cache, ed, ed->dma);
}

