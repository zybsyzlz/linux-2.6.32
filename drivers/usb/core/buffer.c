/*
 * DMA memory management for framework level HCD code (hc_driver)
 *
 * This implementation plugs in through generic "usb_bus" level methods,
 * and should work with all USB controllers, regardles of bus type.
 * 本文件功能：usb 子系统使用dma缓存区的管理
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/mm.h>
#include <asm/io.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/usb.h>
#include "hcd.h"


/*
 * DMA-Coherent Buffers
 */

/* FIXME tune these based on pool statistics ... */
static const size_t	pool_max [HCD_BUFFER_POOLS] = {
	/* platforms without dma-friendly caches might need to
	 * prevent cacheline sharing...
	 */
	32,
	128,
	512,
	PAGE_SIZE / 2
	/* bigger --> allocate pages */
};



/**
 * hcd_buffer_create: 创建hcd的dma缓存池
 */
int hcd_buffer_create(struct usb_hcd *hcd)
{
	char		name[16];
	int 		i, size;

	/*如果主机不支持dma传输，则直接返回，不需要申请dma缓存区*/
	if (!hcd->self.controller->dma_mask &&
	    !(hcd->driver->flags & HCD_LOCAL_MEM))
		return 0;

	for (i = 0; i < HCD_BUFFER_POOLS; i++) {
		size = pool_max[i];
		if (!size)
			continue;
		/*创建dma 缓存池*/
		snprintf(name, sizeof name, "buffer-%d", size);
		hcd->pool[i] = dma_pool_create(name, hcd->self.controller,
				size, size, 0);
		if (!hcd->pool [i]) {
			hcd_buffer_destroy(hcd);
			return -ENOMEM;
		}
	}
	return 0;
}

/**
 * hcd_buffer_destroy:销毁主机创建的DMA缓存池
 */
void hcd_buffer_destroy(struct usb_hcd *hcd)
{
	int i;

	for (i = 0; i < HCD_BUFFER_POOLS; i++) {
		struct dma_pool *pool = hcd->pool[i];
		if (pool) {
			dma_pool_destroy(pool);
			hcd->pool[i] = NULL;
		}
	}
}


/* sometimes alloc/free could use kmalloc with GFP_DMA, for
 * better sharing and to leverage mm/slab.c intelligence.
 */
/*
 * hcd_buffer_alloc:申请size大小的缓存区
 * @ bus:
 * @ size:
 * @ mem_flags:
 * @ dma:
 * @流程步：
 *	1.如果驱动支持dma的传输，则申请dma缓存池
 *	2.否则，进行一般的内存申请
 *	3.返回申请的内存的虚拟地址
*/
void *hcd_buffer_alloc(struct usb_bus *bus, size_t size,
				gfp_t mem_flags, dma_addr_t *dma)
{
	struct usb_hcd		*hcd = bus_to_hcd(bus);
	int 			i;

	/* some USB hosts just use PIO */
	if (!bus->controller->dma_mask &&
	    !(hcd->driver->flags & HCD_LOCAL_MEM)) {
		*dma = ~(dma_addr_t) 0;
		return kmalloc(size, mem_flags);
	}

	for (i = 0; i < HCD_BUFFER_POOLS; i++) {
		if (size <= pool_max [i])
			return dma_pool_alloc(hcd->pool [i], mem_flags, dma);
	}
	return dma_alloc_coherent(hcd->self.controller, size, dma, mem_flags);
}

/*hcd_buffer_free:释放size大小的缓存区*/
void hcd_buffer_free(struct usb_bus *bus, size_t size, void *addr, 
				dma_addr_t dma)
{
	struct usb_hcd	*hcd = bus_to_hcd(bus);
	int	i;

	if (!addr)
		return;

	/*如果不是DMA的缓存*/
	if (!bus->controller->dma_mask &&
	    !(hcd->driver->flags & HCD_LOCAL_MEM)) {
		kfree(addr);
		return;
	}

	for (i = 0; i < HCD_BUFFER_POOLS; i++) {
		if (size <= pool_max [i]) {
			dma_pool_free(hcd->pool [i], addr, dma);
			return;
		}
	}
	dma_free_coherent(hcd->self.controller, size, addr, dma);
}
