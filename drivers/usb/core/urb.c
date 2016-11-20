#include <linux/module.h>
#include <linux/string.h>
#include <linux/bitops.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/log2.h>
#include <linux/usb.h>
#include <linux/wait.h>
#include "hcd.h"
#define to_urb(d) container_of(d, struct urb, kref)

/**
 * urb_destroy:销毁urb传输结构
 */
static void urb_destroy(struct kref *kref)
{
	struct urb *urb = to_urb(kref);

	if (urb->transfer_flags & URB_FREE_BUFFER)
		kfree(urb->transfer_buffer);

	kfree(urb);
}

/**
 * usb_init_urb:初始化urb 
 */
void usb_init_urb(struct urb *urb)
{
	if (urb) {
		memset(urb, 0, sizeof(*urb));
		kref_init(&urb->kref);
		INIT_LIST_HEAD(&urb->anchor_list);
	}
}
EXPORT_SYMBOL_GPL(usb_init_urb);

/**
 * usb_alloc_urb:申请urb传输结构
 * @ iso_packets: 等时传输的数据包个数，只有等时传输使用
 * @ mem_flags: 内存申请标记
 */
struct urb *usb_alloc_urb(int iso_packets, gfp_t mem_flags)
{
	struct urb *urb;

	urb = kmalloc(sizeof(struct urb) +
		iso_packets * sizeof(struct usb_iso_packet_descriptor),
		mem_flags);
	if (!urb) {
		printk(KERN_ERR "alloc_urb: kmalloc failed\n");
		return NULL;
	}
	usb_init_urb(urb);
	return urb;
}
EXPORT_SYMBOL_GPL(usb_alloc_urb);

/**
 * usb_free_urb:释放urb传输结构
 */
void usb_free_urb(struct urb *urb)
{
	if (urb)
		kref_put(&urb->kref, urb_destroy);
}
EXPORT_SYMBOL_GPL(usb_free_urb);

/**
 * usb_get_urb - increments the reference count of the urb
 * @urb: pointer to the urb to modify, may be NULL
 *
 * This must be  called whenever a urb is transferred from a device driver to a
 * host controller driver.  This allows proper reference counting to happen
 * for urbs.
 *
 * A pointer to the urb with the incremented reference counter is returned.
 */
struct urb *usb_get_urb(struct urb *urb)
{
	if (urb)
		kref_get(&urb->kref);
	return urb;
}
EXPORT_SYMBOL_GPL(usb_get_urb);

/**
 * usb_anchor_urb :跟踪被传输的urb
 * @urb: pointer to the urb to anchor
 * @anchor: pointer to the anchor
 *
 * This can be called to have access to URBs which are to be executed
 * without bothering to track them by sysyem
 */
void usb_anchor_urb(struct urb *urb, struct usb_anchor *anchor)
{
	unsigned long flags;

	spin_lock_irqsave(&anchor->lock, flags);
	usb_get_urb(urb);
	list_add_tail(&urb->anchor_list, &anchor->urb_list);
	urb->anchor = anchor;

	if (unlikely(anchor->poisoned)) {
		atomic_inc(&urb->reject);
	}

	spin_unlock_irqrestore(&anchor->lock, flags);
}
EXPORT_SYMBOL_GPL(usb_anchor_urb);

/**
 * usb_unanchor_urb - unanchors an URB
 * @urb: pointer to the urb to anchor
 *
 * Call this to stop the system keeping track of this URB
 */
void usb_unanchor_urb(struct urb *urb)
{
	unsigned long flags;
	struct usb_anchor *anchor;

	if (!urb)
		return;

	anchor = urb->anchor;
	if (!anchor)
		return;

	spin_lock_irqsave(&anchor->lock, flags);
	if (unlikely(anchor != urb->anchor)) {
		/* we've lost the race to another thread */
		spin_unlock_irqrestore(&anchor->lock, flags);
		return;
	}
	urb->anchor = NULL;
	list_del(&urb->anchor_list); /*remove urb from anchor_list*/
	spin_unlock_irqrestore(&anchor->lock, flags);
	usb_put_urb(urb);
	if (list_empty(&anchor->urb_list))
		wake_up(&anchor->wait);
}
EXPORT_SYMBOL_GPL(usb_unanchor_urb);


/**
 * usb_submit_urb:向usb设备提交urb数据传输结构
 * @ urb:待提交的urb传输结构
 */
int usb_submit_urb(struct urb *urb, gfp_t mem_flags)
{
	int				xfertype, max;
	struct usb_device		*dev;
	struct usb_host_endpoint	*ep;
	int				is_out;

	if (!urb || urb->hcpriv || !urb->complete)
		return -EINVAL;
	dev = urb->dev;
	if ((!dev) || (dev->state < USB_STATE_UNAUTHENTICATED))
		return -ENODEV;

	/*
	 * 通过pipe获得usb设备的endpoint
	 */
	ep = (usb_pipein(urb->pipe) ? dev->ep_in : dev->ep_out)
			[usb_pipeendpoint(urb->pipe)];
	if (!ep)
		return -ENOENT;

	urb->ep = ep;
	urb->status = -EINPROGRESS;
	urb->actual_length = 0;

	/* Lots of sanity checks, so HCDs can rely on clean data
	 * and don't need to duplicate tests
	 * 根据endpoint的描述符获得传输的方向
	 */
	xfertype = usb_endpoint_type(&ep->desc);
	if (xfertype == USB_ENDPOINT_XFER_CONTROL) {
		struct usb_ctrlrequest *setup =
				(struct usb_ctrlrequest *) urb->setup_packet;

		if (!setup)
			return -ENOEXEC;
		is_out = !(setup->bRequestType & USB_DIR_IN) ||
				!setup->wLength;
	} else {
		is_out = usb_endpoint_dir_out(&ep->desc);
	}

	/* Cache the direction for later use
	 * 确定urb的传输方向*/
	urb->transfer_flags = (urb->transfer_flags & ~URB_DIR_MASK) |
			(is_out ? URB_DIR_OUT : URB_DIR_IN);

	/*usb设备必须通过config之后，才可使用,除了endpoint0*/
	if (xfertype != USB_ENDPOINT_XFER_CONTROL &&
			dev->state < USB_STATE_CONFIGURED)
		return -ENODEV;

	/*获得endpoint的最大传输字节数*/
	max = le16_to_cpu(ep->desc.wMaxPacketSize);
	if (max <= 0) {
		dev_dbg(&dev->dev,
			"bogus endpoint ep%d%s in %s (bad maxpacket %d)\n",
			usb_endpoint_num(&ep->desc), is_out ? "out" : "in",
			__func__, max);
		return -EMSGSIZE;
	}

	/* periodic transfers limit size per frame/uframe,
	 * but drivers only control those sizes for ISO.
	 * while we're checking, initialize return status.
	 */
	if (xfertype == USB_ENDPOINT_XFER_ISOC) {
		int	n, len;

		/* FIXME SuperSpeed isoc endpoints have up to 16 bursts */
		/* "high bandwidth" mode, 1-3 packets/uframe? */
		if (dev->speed == USB_SPEED_HIGH) {
			int	mult = 1 + ((max >> 11) & 0x03);
			max &= 0x07ff;
			max *= mult;
		}

		if (urb->number_of_packets <= 0)
			return -EINVAL;
		for (n = 0; n < urb->number_of_packets; n++) {
			len = urb->iso_frame_desc[n].length;
			if (len < 0 || len > max)
				return -EMSGSIZE;
			urb->iso_frame_desc[n].status = -EXDEV;
			urb->iso_frame_desc[n].actual_length = 0;
		}
	}

	/* the I/O buffer must be mapped/unmapped, except when length=0 */
	if (urb->transfer_buffer_length > INT_MAX)
		return -EMSGSIZE;

#ifdef DEBUG
	/* stuff that drivers shouldn't do, but which shouldn't
	 * cause problems in HCDs if they get it wrong.
	 */
	{
	unsigned int	orig_flags = urb->transfer_flags;
	unsigned int	allowed;

	/* enforce simple/standard policy */
	allowed = (URB_NO_TRANSFER_DMA_MAP | URB_NO_SETUP_DMA_MAP |
			URB_NO_INTERRUPT | URB_DIR_MASK | URB_FREE_BUFFER);
	switch (xfertype) {
	case USB_ENDPOINT_XFER_BULK:
		if (is_out)
			allowed |= URB_ZERO_PACKET;
		/* FALLTHROUGH */
	case USB_ENDPOINT_XFER_CONTROL:
		allowed |= URB_NO_FSBR;	/* only affects UHCI */
		/* FALLTHROUGH */
	default:			/* all non-iso endpoints */
		if (!is_out)
			allowed |= URB_SHORT_NOT_OK;
		break;
	case USB_ENDPOINT_XFER_ISOC:
		allowed |= URB_ISO_ASAP;
		break;
	}
	urb->transfer_flags &= allowed;

	/* fail if submitter gave bogus flags */
	if (urb->transfer_flags != orig_flags) {
		dev_err(&dev->dev, "BOGUS urb flags, %x --> %x\n",
			orig_flags, urb->transfer_flags);
		return -EINVAL;
	}
	}
#endif
	/*计算数据帧长度*/
	switch (xfertype) {
	case USB_ENDPOINT_XFER_ISOC:
	case USB_ENDPOINT_XFER_INT:
		/* too small? */
		if (urb->interval <= 0)
			return -EINVAL;
		/* too big? */
		switch (dev->speed) {
		case USB_SPEED_SUPER:	/* units are 125us */
			/* Handle up to 2^(16-1) microframes */
			if (urb->interval > (1 << 15))
				return -EINVAL;
			max = 1 << 15;
		case USB_SPEED_HIGH:	/* units are microframes */
			/* NOTE usb handles 2^15 */
			if (urb->interval > (1024 * 8))
				urb->interval = 1024 * 8;
			max = 1024 * 8;
			break;
		case USB_SPEED_FULL:	/* units are frames/msec */
		case USB_SPEED_LOW:
			if (xfertype == USB_ENDPOINT_XFER_INT) {
				if (urb->interval > 255)
					return -EINVAL;
				/* NOTE ohci only handles up to 32 */
				max = 128;
			} else {
				if (urb->interval > 1024)
					urb->interval = 1024;
				/* NOTE usb and ohci handle up to 2^15 */
				max = 1024;
			}
			break;
		default:
			return -EINVAL;
		}
		/* Round down to a power of 2, no more than max */
		urb->interval = min(max, 1 << ilog2(urb->interval));
	}

	return usb_hcd_submit_urb(urb, mem_flags);
}
EXPORT_SYMBOL_GPL(usb_submit_urb);

/**
 * usb_unlink_urb:非阻塞取消urb的传输
 */
int usb_unlink_urb(struct urb *urb)
{
	if (!urb)
		return -EINVAL;
	if (!urb->dev)
		return -ENODEV;
	if (!urb->ep)
		return -EIDRM;
	return usb_hcd_unlink_urb(urb, -ECONNRESET);
}
EXPORT_SYMBOL_GPL(usb_unlink_urb);

/**
 * usb_kill_urb:从传输队列中删除urb
 */
void usb_kill_urb(struct urb *urb)
{
	might_sleep();
	if (!(urb && urb->dev && urb->ep))
		return;
	atomic_inc(&urb->reject);

	/*取消urb的传输*/
	usb_hcd_unlink_urb(urb, -ENOENT);
	/*等待该urb为空闲时返回*/
	wait_event(usb_kill_urb_queue, atomic_read(&urb->use_count) == 0);

	atomic_dec(&urb->reject);
}
EXPORT_SYMBOL_GPL(usb_kill_urb);

/**
 * usb_poison_urb - reliably kill a transfer and prevent further use of an URB
 * @urb: pointer to URB describing a previously submitted request,
 *	may be NULL
 */
void usb_poison_urb(struct urb *urb)
{
	might_sleep();
	if (!(urb && urb->dev && urb->ep))
		return;
	atomic_inc(&urb->reject);

	usb_hcd_unlink_urb(urb, -ENOENT);
	wait_event(usb_kill_urb_queue, atomic_read(&urb->use_count) == 0);
}
EXPORT_SYMBOL_GPL(usb_poison_urb);

void usb_unpoison_urb(struct urb *urb)
{
	if (!urb)
		return;

	atomic_dec(&urb->reject);
}
EXPORT_SYMBOL_GPL(usb_unpoison_urb);

/**
 * usb_kill_anchored_urbs: 反向删除所有的加入到anchor中的urb结构
 */
void usb_kill_anchored_urbs(struct usb_anchor *anchor)
{
	struct urb *victim;

	spin_lock_irq(&anchor->lock);
	while (!list_empty(&anchor->urb_list)) {
		victim = list_entry(anchor->urb_list.prev, struct urb,
				    anchor_list);
		/* we must make sure the URB isn't freed before we kill it*/
		usb_get_urb(victim);
		spin_unlock_irq(&anchor->lock);
		/* this will unanchor the URB */
		usb_kill_urb(victim);
		usb_put_urb(victim);
		spin_lock_irq(&anchor->lock);
	}
	spin_unlock_irq(&anchor->lock);
}
EXPORT_SYMBOL_GPL(usb_kill_anchored_urbs);


/**
 * usb_poison_anchored_urbs - cease all traffic from an anchor
 * @anchor: anchor the requests are bound to
 */
void usb_poison_anchored_urbs(struct usb_anchor *anchor)
{
	struct urb *victim;

	spin_lock_irq(&anchor->lock);
	anchor->poisoned = 1;
	while (!list_empty(&anchor->urb_list)) {
		victim = list_entry(anchor->urb_list.prev, struct urb,
				    anchor_list);
		/* we must make sure the URB isn't freed before we kill it*/
		usb_get_urb(victim);
		spin_unlock_irq(&anchor->lock);
		/* this will unanchor the URB */
		usb_poison_urb(victim);
		usb_put_urb(victim);
		spin_lock_irq(&anchor->lock);
	}
	spin_unlock_irq(&anchor->lock);
}
EXPORT_SYMBOL_GPL(usb_poison_anchored_urbs);

/**
 * usb_unpoison_anchored_urbs - let an anchor be used successfully again
 * @anchor: anchor the requests are bound to
 *
 * Reverses the effect of usb_poison_anchored_urbs
 * the anchor can be used normally after it returns
 */
void usb_unpoison_anchored_urbs(struct usb_anchor *anchor)
{
	unsigned long flags;
	struct urb *lazarus;

	spin_lock_irqsave(&anchor->lock, flags);
	list_for_each_entry(lazarus, &anchor->urb_list, anchor_list) {
		usb_unpoison_urb(lazarus);
	}
	anchor->poisoned = 0;
	spin_unlock_irqrestore(&anchor->lock, flags);
}
EXPORT_SYMBOL_GPL(usb_unpoison_anchored_urbs);
/**
 * usb_unlink_anchored_urbs - asynchronously cancel transfer requests en masse
 * @anchor: anchor the requests are bound to
 *
 * this allows all outstanding URBs to be unlinked starting
 * from the back of the queue. This function is asynchronous.
 * The unlinking is just tiggered. It may happen after this
 * function has returned.
 *
 * This routine should not be called by a driver after its disconnect
 * method has returned.
 */
void usb_unlink_anchored_urbs(struct usb_anchor *anchor)
{
	struct urb *victim;
	unsigned long flags;

	spin_lock_irqsave(&anchor->lock, flags);
	while (!list_empty(&anchor->urb_list)) {
		victim = list_entry(anchor->urb_list.prev, struct urb,
				    anchor_list);
		usb_get_urb(victim);
		spin_unlock_irqrestore(&anchor->lock, flags);
		/* this will unanchor the URB */
		usb_unlink_urb(victim);
		usb_put_urb(victim);
		spin_lock_irqsave(&anchor->lock, flags);
	}
	spin_unlock_irqrestore(&anchor->lock, flags);
}
EXPORT_SYMBOL_GPL(usb_unlink_anchored_urbs);

/**
 * usb_wait_anchor_empty_timeout - wait for an anchor to be unused
 * @anchor: the anchor you want to become unused
 * @timeout: how long you are willing to wait in milliseconds
 *
 * Call this is you want to be sure all an anchor's
 * URBs have finished
 */
int usb_wait_anchor_empty_timeout(struct usb_anchor *anchor,
				  unsigned int timeout)
{
	return wait_event_timeout(anchor->wait, list_empty(&anchor->urb_list),
				  msecs_to_jiffies(timeout));
}
EXPORT_SYMBOL_GPL(usb_wait_anchor_empty_timeout);

/**
 * usb_get_from_anchor - get an anchor's oldest urb
 * @anchor: the anchor whose urb you want
 *
 * this will take the oldest urb from an anchor,
 * unanchor and return it
 */
struct urb *usb_get_from_anchor(struct usb_anchor *anchor)
{
	struct urb *victim;
	unsigned long flags;

	spin_lock_irqsave(&anchor->lock, flags);
	if (!list_empty(&anchor->urb_list)) {
		victim = list_entry(anchor->urb_list.next, struct urb,
				    anchor_list);
		usb_get_urb(victim);
		spin_unlock_irqrestore(&anchor->lock, flags);
		usb_unanchor_urb(victim);
	} else {
		spin_unlock_irqrestore(&anchor->lock, flags);
		victim = NULL;
	}

	return victim;
}

EXPORT_SYMBOL_GPL(usb_get_from_anchor);

/**
 * usb_scuttle_anchored_urbs - unanchor all an anchor's urbs
 * @anchor: the anchor whose urbs you want to unanchor
 *
 * use this to get rid of all an anchor's urbs
 */
void usb_scuttle_anchored_urbs(struct usb_anchor *anchor)
{
	struct urb *victim;
	unsigned long flags;

	spin_lock_irqsave(&anchor->lock, flags);
	while (!list_empty(&anchor->urb_list)) {
		victim = list_entry(anchor->urb_list.prev, struct urb,
				    anchor_list);
		usb_get_urb(victim);
		spin_unlock_irqrestore(&anchor->lock, flags);
		/* this may free the URB */
		usb_unanchor_urb(victim);
		usb_put_urb(victim);
		spin_lock_irqsave(&anchor->lock, flags);
	}
	spin_unlock_irqrestore(&anchor->lock, flags);
}

EXPORT_SYMBOL_GPL(usb_scuttle_anchored_urbs);

/**
 * usb_anchor_empty - is an anchor empty
 * @anchor: the anchor you want to query
 *
 * returns 1 if the anchor has no urbs associated with it
 */
int usb_anchor_empty(struct usb_anchor *anchor)
{
	return list_empty(&anchor->urb_list);
}

EXPORT_SYMBOL_GPL(usb_anchor_empty);

