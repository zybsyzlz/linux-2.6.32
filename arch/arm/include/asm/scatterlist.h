#ifndef _ASMARM_SCATTERLIST_H
#define _ASMARM_SCATTERLIST_H

#include <asm/memory.h>
#include <asm/types.h>

struct scatterlist {
#ifdef CONFIG_DEBUG_SG
	unsigned long	sg_magic;
#endif
	unsigned long	page_link;  /*缓存区所对应的页地址*/
	unsigned int	offset;		/*页内偏移*/
	dma_addr_t	dma_address;	/* dma address			 */
	unsigned int	length;		/* 缓冲区的长度*/
};

/*
 * These macros should be used after a pci_map_sg call has been done
 * to get bus addresses of each of the SG entries and their lengths.
 * You should only work with the number of sg entries pci_map_sg
 * returns, or alternatively stop on the first sg_dma_len(sg) which
 * is 0.
 */
#define sg_dma_address(sg)      ((sg)->dma_address)
#define sg_dma_len(sg)          ((sg)->length)

#endif /* _ASMARM_SCATTERLIST_H */