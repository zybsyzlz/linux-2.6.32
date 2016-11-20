/*
 * (C) Copyright Linus Torvalds 1999
 * (C) Copyright Johannes Erdfelt 1999-2001
 * (C) Copyright Andreas Gal 1999
 * (C) Copyright Gregory P. Smith 1999
 * (C) Copyright Deti Fliegl 1999
 * (C) Copyright Randy Dunlap 2000
 * (C) Copyright David Brownell 2000-2002
 * 
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 * 回弹缓冲区的作用：
 * 　先前的Linux kernel 要求通往存储设备的数据缓存必须放在物理RAM的低端内存区域，
 *   即使是应用程序可以同时使用高端内存和低端内存也存在同样状况。这样，来自低端
 *   内存区域数据缓存的I/O请求可以直接进行内存存取操作。但是，当应用程序发出一个
 *   I/O请求，其中包含位于高端内存的数据缓存时，kernel将强制在低端内存中分配一个
 *   临时数据缓存，并将位于高端内存的应用程序缓存数据复制到此处，这个数据缓存相
 *   当于一个 bounce buffer。
 *   例如一些老设备只能访问16M以下的内存，但DMA的目的地址却在16M以上时，就需要在
 *   设备能访问16M范围内设置一个buffer作为跳转。这种额外的数据拷贝被称为“bounce buffering”
 *   虽然　bounce buffer 会明显地降低I/O密集的数据库应用的性能，因为大量分配的bounce buffers 
 *   会占用许多内存，而且bounce buffer 的复制会增加系统内存总线的负荷,但是有时候没有其它方法
 *   可以替代
*/

#include <linux/module.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/completion.h>
#include <linux/utsname.h>
#include <linux/mm.h>
#include <asm/io.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/mutex.h>
#include <asm/irq.h>
#include <asm/byteorder.h>
#include <asm/unaligned.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>

#include <linux/usb.h>

#include "usb.h"
#include "hcd.h"
#include "hub.h"


/*-------------------------------------------------------------------------*/

/*
 * USB Host Controller Driver framework
 *
 * Plugs into usbcore (usb_bus) and lets HCDs share code, minimizing
 * HCD-specific behaviors/bugs.
 *
 * This does error checks, tracks devices and urbs, and delegates to a
 * "hc_driver" only for code (and data) that really needs to know about
 * hardware differences.  That includes root hub registers, i/o queues,
 * and so on ... but as little else as possible.
 *
 * Shared code includes most of the "root hub" code (these are emulated,
 * though each HC's hardware works differently) and PCI glue, plus request
 * tracking overhead.  The HCD code should only block on spinlocks or on
 * hardware handshaking; blocking on software events (such as other kernel
 * threads releasing resources, or completing actions) is all generic.
 *
 * Happens the USB 2.0 spec says this would be invisible inside the "USBD",
 * and includes mostly a "HCDI" (HCD Interface) along with some APIs used
 * only by the hub driver ... and that neither should be seen or used by
 * usb client device drivers.
 *
 * Contributors of ideas or unattributed patches include: David Brownell,
 * Roman Weissgaerber, Rory Bolt, Greg Kroah-Hartman, ...
 *
 * HISTORY:
 * 2002-02-21	Pull in most of the usb_bus support from usb.c; some
 *		associated cleanup.  "usb_hcd" still != "usb_bus".
 * 2001-12-12	Initial patch version for Linux 2.5.1 kernel.
 */

/*-------------------------------------------------------------------------*/

/* Keep track of which host controller drivers are loaded */
unsigned long usb_hcds_loaded;
EXPORT_SYMBOL_GPL(usb_hcds_loaded);

/*管理注册的usb总线链表*/
LIST_HEAD (usb_bus_list);
EXPORT_SYMBOL_GPL (usb_bus_list);

/*管理usb 总线bus的bus number位图*/
#define USB_MAXBUS		64
struct usb_busmap {
	unsigned long busmap [USB_MAXBUS / (8*sizeof (unsigned long))];
};
static struct usb_busmap busmap;

/* used when updating list of hcds */
DEFINE_MUTEX(usb_bus_list_lock);	/* exported only for usbfs */
EXPORT_SYMBOL_GPL (usb_bus_list_lock);

/* used for controlling access to virtual root hubs */
static DEFINE_SPINLOCK(hcd_root_hub_lock);

/* used when updating an endpoint's URB list */
static DEFINE_SPINLOCK(hcd_urb_list_lock);

/* used to protect against unlinking URBs after the device is gone */
static DEFINE_SPINLOCK(hcd_urb_unlink_lock);

/* wait queue for synchronous unlinks */
DECLARE_WAIT_QUEUE_HEAD(usb_kill_urb_queue);

/**
 * is_root_hub:判断udev usb 设备是否是一个root hub
 * 注:usb hub也属于usb设备结构
*/
static inline int is_root_hub(struct usb_device *udev)
{
	return (udev->parent == NULL);
}

/*-------------------------------------------------------------------------*/

/*
 * Sharable chunks of root hub code.
 */

/*-------------------------------------------------------------------------*/

#define KERNEL_REL	((LINUX_VERSION_CODE >> 16) & 0x0ff)
#define KERNEL_VER	((LINUX_VERSION_CODE >> 8) & 0x0ff)

/* usb 3.0 root hub device descriptor */
static const u8 usb3_rh_dev_descriptor[18] = {
	0x12,       /*  __u8  bLength; */
	0x01,       /*  __u8  bDescriptorType; Device */
	0x00, 0x03, /*  __le16 bcdUSB; v3.0 */

	0x09,	    /*  __u8  bDeviceClass; HUB_CLASSCODE */
	0x00,	    /*  __u8  bDeviceSubClass; */
	0x03,       /*  __u8  bDeviceProtocol; USB 3.0 hub */
	0x09,       /*  __u8  bMaxPacketSize0; 2^9 = 512 Bytes */

	0x6b, 0x1d, /*  __le16 idVendor; Linux Foundation */
	0x02, 0x00, /*  __le16 idProduct; device 0x0002 */
	KERNEL_VER, KERNEL_REL, /*  __le16 bcdDevice */

	0x03,       /*  __u8  iManufacturer; */
	0x02,       /*  __u8  iProduct; */
	0x01,       /*  __u8  iSerialNumber; */
	0x01        /*  __u8  bNumConfigurations; */
};

/* usb 2.0 root hub device descriptor */
static const u8 usb2_rh_dev_descriptor [18] = {
	0x12,       /*  __u8  bLength; */
	0x01,       /*  __u8  bDescriptorType; Device */
	0x00, 0x02, /*  __le16 bcdUSB; v2.0 */

	0x09,	    /*  __u8  bDeviceClass; HUB_CLASSCODE */
	0x00,	    /*  __u8  bDeviceSubClass; */
	0x00,       /*  __u8  bDeviceProtocol; [ usb 2.0 no TT ] */
	0x40,       /*  __u8  bMaxPacketSize0; 64 Bytes */

	0x6b, 0x1d, /*  __le16 idVendor; Linux Foundation */
	0x02, 0x00, /*  __le16 idProduct; device 0x0002 */
	KERNEL_VER, KERNEL_REL, /*  __le16 bcdDevice */

	0x03,       /*  __u8  iManufacturer; */
	0x02,       /*  __u8  iProduct; */
	0x01,       /*  __u8  iSerialNumber; */
	0x01        /*  __u8  bNumConfigurations; */
};

/* no usb 2.0 root hub "device qualifier" descriptor: one speed only */

/* usb 1.1 root hub device descriptor */
static const u8 usb11_rh_dev_descriptor [18] = {
	0x12,       /*  __u8  bLength; */
	0x01,       /*  __u8  bDescriptorType; Device */
	0x10, 0x01, /*  __le16 bcdUSB; v1.1 */

	0x09,	    /*  __u8  bDeviceClass; HUB_CLASSCODE */
	0x00,	    /*  __u8  bDeviceSubClass; */
	0x00,       /*  __u8  bDeviceProtocol; [ low/full speeds only ] */
	0x40,       /*  __u8  bMaxPacketSize0; 64 Bytes */

	0x6b, 0x1d, /*  __le16 idVendor; Linux Foundation */
	0x01, 0x00, /*  __le16 idProduct; device 0x0001 */
	KERNEL_VER, KERNEL_REL, /*  __le16 bcdDevice */

	0x03,       /*  __u8  iManufacturer; */
	0x02,       /*  __u8  iProduct; */
	0x01,       /*  __u8  iSerialNumber; */
	0x01        /*  __u8  bNumConfigurations; */
};


/*-------------------------------------------------------------------------*/

/* Configuration descriptors for our root hubs */

static const u8 fs_rh_config_descriptor [] = {

	/* one configuration */
	0x09,       /*  __u8  bLength; */
	0x02,       /*  __u8  bDescriptorType; Configuration */
	0x19, 0x00, /*  __le16 wTotalLength; */
	0x01,       /*  __u8  bNumInterfaces; (1) */
	0x01,       /*  __u8  bConfigurationValue; */
	0x00,       /*  __u8  iConfiguration; */
	0xc0,       /*  __u8  bmAttributes; 
				 Bit 7: must be set,
				     6: Self-powered,
				     5: Remote wakeup,
				     4..0: resvd */
	0x00,       /*  __u8  MaxPower; */
      
	/* USB 1.1:
	 * USB 2.0, single TT organization (mandatory):
	 *	one interface, protocol 0
	 *
	 * USB 2.0, multiple TT organization (optional):
	 *	two interfaces, protocols 1 (like single TT)
	 *	and 2 (multiple TT mode) ... config is
	 *	sometimes settable
	 *	NOT IMPLEMENTED
	 */

	/* one interface */
	0x09,       /*  __u8  if_bLength; */
	0x04,       /*  __u8  if_bDescriptorType; Interface */
	0x00,       /*  __u8  if_bInterfaceNumber; */
	0x00,       /*  __u8  if_bAlternateSetting; */
	0x01,       /*  __u8  if_bNumEndpoints; */
	0x09,       /*  __u8  if_bInterfaceClass; HUB_CLASSCODE */
	0x00,       /*  __u8  if_bInterfaceSubClass; */
	0x00,       /*  __u8  if_bInterfaceProtocol; [usb1.1 or single tt] */
	0x00,       /*  __u8  if_iInterface; */
     
	/* one endpoint (status change endpoint) */
	0x07,       /*  __u8  ep_bLength; */
	0x05,       /*  __u8  ep_bDescriptorType; Endpoint */
	0x81,       /*  __u8  ep_bEndpointAddress; IN Endpoint 1 */
 	0x03,       /*  __u8  ep_bmAttributes; Interrupt */
 	0x02, 0x00, /*  __le16 ep_wMaxPacketSize; 1 + (MAX_ROOT_PORTS / 8) */
	0xff        /*  __u8  ep_bInterval; (255ms -- usb 2.0 spec) */
};

static const u8 hs_rh_config_descriptor [] = {

	/* one configuration */
	0x09,       /*  __u8  bLength; */
	0x02,       /*  __u8  bDescriptorType; Configuration */
	0x19, 0x00, /*  __le16 wTotalLength; */
	0x01,       /*  __u8  bNumInterfaces; (1) */
	0x01,       /*  __u8  bConfigurationValue; */
	0x00,       /*  __u8  iConfiguration; */
	0xc0,       /*  __u8  bmAttributes; 
				 Bit 7: must be set,
				     6: Self-powered,
				     5: Remote wakeup,
				     4..0: resvd */
	0x00,       /*  __u8  MaxPower; */
      
	/* USB 1.1:
	 * USB 2.0, single TT organization (mandatory):
	 *	one interface, protocol 0
	 *
	 * USB 2.0, multiple TT organization (optional):
	 *	two interfaces, protocols 1 (like single TT)
	 *	and 2 (multiple TT mode) ... config is
	 *	sometimes settable
	 *	NOT IMPLEMENTED
	 */

	/* one interface */
	0x09,       /*  __u8  if_bLength; */
	0x04,       /*  __u8  if_bDescriptorType; Interface */
	0x00,       /*  __u8  if_bInterfaceNumber; */
	0x00,       /*  __u8  if_bAlternateSetting; */
	0x01,       /*  __u8  if_bNumEndpoints; */
	0x09,       /*  __u8  if_bInterfaceClass; HUB_CLASSCODE */
	0x00,       /*  __u8  if_bInterfaceSubClass; */
	0x00,       /*  __u8  if_bInterfaceProtocol; [usb1.1 or single tt] */
	0x00,       /*  __u8  if_iInterface; */
     
	/* one endpoint (status change endpoint) */
	0x07,       /*  __u8  ep_bLength; */
	0x05,       /*  __u8  ep_bDescriptorType; Endpoint */
	0x81,       /*  __u8  ep_bEndpointAddress; IN Endpoint 1 */
 	0x03,       /*  __u8  ep_bmAttributes; Interrupt */
		    /* __le16 ep_wMaxPacketSize; 1 + (MAX_ROOT_PORTS / 8)
		     * see hub.c:hub_configure() for details. */
	(USB_MAXCHILDREN + 1 + 7) / 8, 0x00,
	0x0c        /*  __u8  ep_bInterval; (256ms -- usb 2.0 spec) */
};

static const u8 ss_rh_config_descriptor[] = {
	/* one configuration */
	0x09,       /*  __u8  bLength; */
	0x02,       /*  __u8  bDescriptorType; Configuration */
	0x19, 0x00, /*  __le16 wTotalLength; FIXME */
	0x01,       /*  __u8  bNumInterfaces; (1) */
	0x01,       /*  __u8  bConfigurationValue; */
	0x00,       /*  __u8  iConfiguration; */
	0xc0,       /*  __u8  bmAttributes;
				 Bit 7: must be set,
				     6: Self-powered,
				     5: Remote wakeup,
				     4..0: resvd */
	0x00,       /*  __u8  MaxPower; */

	/* one interface */
	0x09,       /*  __u8  if_bLength; */
	0x04,       /*  __u8  if_bDescriptorType; Interface */
	0x00,       /*  __u8  if_bInterfaceNumber; */
	0x00,       /*  __u8  if_bAlternateSetting; */
	0x01,       /*  __u8  if_bNumEndpoints; */
	0x09,       /*  __u8  if_bInterfaceClass; HUB_CLASSCODE */
	0x00,       /*  __u8  if_bInterfaceSubClass; */
	0x00,       /*  __u8  if_bInterfaceProtocol; */
	0x00,       /*  __u8  if_iInterface; */

	/* one endpoint (status change endpoint) */
	0x07,       /*  __u8  ep_bLength; */
	0x05,       /*  __u8  ep_bDescriptorType; Endpoint */
	0x81,       /*  __u8  ep_bEndpointAddress; IN Endpoint 1 */
	0x03,       /*  __u8  ep_bmAttributes; Interrupt */
		    /* __le16 ep_wMaxPacketSize; 1 + (MAX_ROOT_PORTS / 8)
		     * see hub.c:hub_configure() for details. */
	(USB_MAXCHILDREN + 1 + 7) / 8, 0x00,
	0x0c        /*  __u8  ep_bInterval; (256ms -- usb 2.0 spec) */
	/*
	 * All 3.0 hubs should have an endpoint companion descriptor,
	 * but we're ignoring that for now.  FIXME?
	 */
};

/*-------------------------------------------------------------------------*/

/**
 * ascii2desc:ASCII码转化为UTF-16LE的字符串描述符
 * @s: Null-terminated ASCII (actually ISO-8859-1) string
 * @buf:保存转换后的字符串(header + UTF-16LE)
 * @len: Length (in bytes; may be odd) of descriptor buffer.
 * 注: USB String descriptors最多只能包含126个字符,多余的输入字符被截断
 */
static unsigned
ascii2desc(char const *s, u8 *buf, unsigned len)
{
	unsigned n, t = 2 + 2*strlen(s);

	if (t > 254)
		t = 254;	/* Longest possible UTF string descriptor */
	if (len > t)
		len = t;

	t += USB_DT_STRING << 8;	/* Now t is first 16 bits to store */

	n = len;
	while (n--) {
		*buf++ = t;
		if (!n--)
			break;
		*buf++ = t >> 8;
		t = (unsigned char)*s++;
	}
	return len;
}

/**
 * rh_string:返回root hub的String描述符信息
 * @id: the string ID number (0: langids, 1: serial #, 2: product, 3: vendor)
 * @hcd: 
 * @data:保存root hub的String描述符信息 
 * @len: data数组的长度 
 */
static unsigned
rh_string(int id, struct usb_hcd const *hcd, u8 *data, unsigned len)
{
	char buf[100];
	char const *s;
	static char const langids[4] = {4, USB_DT_STRING, 0x09, 0x04};

	// language ids
	switch (id) {
	case 0:
		/* Array of LANGID codes (0x0409 is MSFT-speak for "en-us") */
		/* See http://www.usb.org/developers/docs/USB_LANGIDs.pdf */
		if (len > 4)
			len = 4;
		memcpy(data, langids, len);
		return len;
	case 1:
		/* Serial number */
		s = hcd->self.bus_name;
		break;
	case 2:
		/* Product name */
		s = hcd->product_desc;
		break;
	case 3:
		/* Manufacturer */
		snprintf (buf, sizeof buf, "%s %s %s", init_utsname()->sysname,
			init_utsname()->release, hcd->driver->description);
		s = buf;
		break;
	default:
		/* Can't happen; caller guarantees it */
		return 0;
	}

	return ascii2desc(s, data, len);
}

/**
 * rh_call_control:root hub 标准请求传输控制接口
 * @ hcd: usb主机结构
 * @ urb: 控制传输urb描述结构
*/
static int rh_call_control (struct usb_hcd *hcd, struct urb *urb)
{
	struct usb_ctrlrequest *cmd;
 	u16		typeReq, wValue, wIndex, wLength;
	u8		*ubuf = urb->transfer_buffer;
	u8		tbuf [sizeof (struct usb_hub_descriptor)]
		__attribute__((aligned(4)));
	const u8	*bufp = tbuf;
	unsigned	len = 0;
	int		status;
	u8		patch_wakeup = 0;
	u8		patch_protocol = 0;

	might_sleep();

	spin_lock_irq(&hcd_root_hub_lock);
	status = usb_hcd_link_urb_to_ep(hcd, urb);
	spin_unlock_irq(&hcd_root_hub_lock);
	if (status)
		return status;
	urb->hcpriv = hcd;	/* Indicate it's queued */

	/*取出root hub的请求命令字*/
	cmd = (struct usb_ctrlrequest *) urb->setup_packet;
	/*请求类型*/
	typeReq  = (cmd->bRequestType << 8) | cmd->bRequest;
	wValue   = le16_to_cpu (cmd->wValue);
	wIndex   = le16_to_cpu (cmd->wIndex);
	/*cmd第二阶段传输的附加数据的长度*/
	wLength  = le16_to_cpu (cmd->wLength);

	if (wLength > urb->transfer_buffer_length)
		goto error;

	urb->actual_length = 0;
	switch (typeReq) {
	case DeviceRequest | USB_REQ_GET_STATUS:
		tbuf [0] = (device_may_wakeup(&hcd->self.root_hub->dev)
					<< USB_DEVICE_REMOTE_WAKEUP)
				| (1 << USB_DEVICE_SELF_POWERED);
		tbuf [1] = 0;
		len = 2;
		break;
	case DeviceOutRequest | USB_REQ_CLEAR_FEATURE:
		if (wValue == USB_DEVICE_REMOTE_WAKEUP)
			device_set_wakeup_enable(&hcd->self.root_hub->dev, 0);
		else
			goto error;
		break;
	case DeviceOutRequest | USB_REQ_SET_FEATURE:
		if (device_can_wakeup(&hcd->self.root_hub->dev)
				&& wValue == USB_DEVICE_REMOTE_WAKEUP)
			device_set_wakeup_enable(&hcd->self.root_hub->dev, 1);
		else
			goto error;
		break;
	case DeviceRequest | USB_REQ_GET_CONFIGURATION:
		tbuf [0] = 1;
		len = 1;
			/* FALLTHROUGH */
	case DeviceOutRequest | USB_REQ_SET_CONFIGURATION:
		break;
	case DeviceRequest | USB_REQ_GET_DESCRIPTOR:
		switch (wValue & 0xff00) {
		case USB_DT_DEVICE << 8:
			switch (hcd->driver->flags & HCD_MASK) {
			case HCD_USB3:
				bufp = usb3_rh_dev_descriptor;
				break;
			case HCD_USB2:
				bufp = usb2_rh_dev_descriptor;
				break;
			case HCD_USB11:
				bufp = usb11_rh_dev_descriptor;
				break;
			default:
				goto error;
			}
			len = 18;
			if (hcd->has_tt)
				patch_protocol = 1;
			break;
		case USB_DT_CONFIG << 8:
			switch (hcd->driver->flags & HCD_MASK) {
			case HCD_USB3:
				bufp = ss_rh_config_descriptor;
				len = sizeof ss_rh_config_descriptor;
				break;
			case HCD_USB2:
				bufp = hs_rh_config_descriptor;
				len = sizeof hs_rh_config_descriptor;
				break;
			case HCD_USB11:
				bufp = fs_rh_config_descriptor;
				len = sizeof fs_rh_config_descriptor;
				break;
			default:
				goto error;
			}
			if (device_can_wakeup(&hcd->self.root_hub->dev))
				patch_wakeup = 1;
			break;
		case USB_DT_STRING << 8:
			if ((wValue & 0xff) < 4)
				urb->actual_length = rh_string(wValue & 0xff,
						hcd, ubuf, wLength);
			else /* unsupported IDs --> "protocol stall" */
				goto error;
			break;
		default:
			goto error;
		}
		break;
	case DeviceRequest | USB_REQ_GET_INTERFACE:
		tbuf [0] = 0;
		len = 1;
			/* FALLTHROUGH */
	case DeviceOutRequest | USB_REQ_SET_INTERFACE:
		break;
	case DeviceOutRequest | USB_REQ_SET_ADDRESS:
		// wValue == urb->dev->devaddr
		dev_dbg (hcd->self.controller, "root hub device address %d\n",
			wValue);
		break;

	/* INTERFACE REQUESTS (no defined feature/status flags) */

	/* ENDPOINT REQUESTS */

	case EndpointRequest | USB_REQ_GET_STATUS:
		// ENDPOINT_HALT flag
		tbuf [0] = 0;
		tbuf [1] = 0;
		len = 2;
			/* FALLTHROUGH */
	case EndpointOutRequest | USB_REQ_CLEAR_FEATURE:
	case EndpointOutRequest | USB_REQ_SET_FEATURE:
		dev_dbg (hcd->self.controller, "no endpoint features yet\n");
		break;

	/* CLASS REQUESTS (and errors) */

	default:
		/* non-generic request */
		switch (typeReq) {
		case GetHubStatus:
		case GetPortStatus:
			len = 4;
			break;
		case GetHubDescriptor:
			len = sizeof (struct usb_hub_descriptor);
			break;
		}
		status = hcd->driver->hub_control (hcd,
			typeReq, wValue, wIndex,
			tbuf, wLength);
		break;
error:
		/* "protocol stall" on error */
		status = -EPIPE;
	}

	if (status) {
		len = 0;
		if (status != -EPIPE) {
			dev_dbg (hcd->self.controller,
				"CTRL: TypeReq=0x%x val=0x%x "
				"idx=0x%x len=%d ==> %d\n",
				typeReq, wValue, wIndex,
				wLength, status);
		}
	}
	if (len) {
		if (urb->transfer_buffer_length < len)
			len = urb->transfer_buffer_length;
		urb->actual_length = len;
		/*拷贝root hub的描述符信息到ubuf*/
		memcpy (ubuf, bufp, len);

		/* report whether RH hardware supports remote wakeup */
		if (patch_wakeup &&
				len > offsetof (struct usb_config_descriptor,
						bmAttributes))
			((struct usb_config_descriptor *)ubuf)->bmAttributes
				|= USB_CONFIG_ATT_WAKEUP;

		/* report whether RH hardware has an integrated TT */
		if (patch_protocol &&
				len > offsetof(struct usb_device_descriptor,
						bDeviceProtocol))
			((struct usb_device_descriptor *) ubuf)->
					bDeviceProtocol = 1;
	}

	/*urb传输完成,从ep队列中删除*/
	spin_lock_irq(&hcd_root_hub_lock);
	usb_hcd_unlink_urb_from_ep(hcd, urb);

	/* This peculiar use of spinlocks echoes what real HC drivers do.
	 * Avoiding calls to local_irq_disable/enable makes the code
	 * RT-friendly.
	 */
	spin_unlock(&hcd_root_hub_lock);
	usb_hcd_giveback_urb(hcd, urb, status);
	spin_lock(&hcd_root_hub_lock);

	spin_unlock_irq(&hcd_root_hub_lock);
	return 0;
}

/*
 * usb_hcd_poll_rh_status:root hub的周期性中断传输
 *  @ root hub事件产生时,调用此接口
 *  @ 定时器周期性轮寻root hub状态时调用此接口
 */
void usb_hcd_poll_rh_status(struct usb_hcd *hcd)
{
	struct urb	*urb;
	int		length;
	unsigned long	flags;
	char		buffer[6];	/* Any root hubs with > 31 ports? */

	if (unlikely(!hcd->rh_registered))
		return;
	if (!hcd->uses_new_polling && !hcd->status_urb)
		return;

	/*获得root hub 的状态信息*/
	length = hcd->driver->hub_status_data(hcd, buffer);
	if (length > 0) {

		/* try to complete the status urb */
		spin_lock_irqsave(&hcd_root_hub_lock, flags);
		urb = hcd->status_urb; 
		if (urb) {
			/*状态传输urb?,当usb设备插入时，root hub的urb传输*/
			hcd->poll_pending = 0;
			hcd->status_urb = NULL;
			urb->actual_length = length;
			/*root hub的状态数据拷贝到transfer_buffer中*/
			memcpy(urb->transfer_buffer, buffer, length);

			usb_hcd_unlink_urb_from_ep(hcd, urb);
			spin_unlock(&hcd_root_hub_lock);
			usb_hcd_giveback_urb(hcd, urb, 0);
			spin_lock(&hcd_root_hub_lock);
		} else {
			length = 0;
			hcd->poll_pending = 1;
		}
		spin_unlock_irqrestore(&hcd_root_hub_lock, flags);
	}

	/* The USB 2.0 spec says 256 ms.  This is close enough and won't
	 * exceed that limit if HZ is 100. The math is more clunky than
	 * maybe expected, this is to make sure that all timers for USB devices
	 * fire at the same time to give the CPU a break inbetween */
	if (hcd->uses_new_polling ? hcd->poll_rh :
			(length == 0 && hcd->status_urb != NULL))
		mod_timer (&hcd->rh_timer, (jiffies/(HZ/4) + 1) * (HZ/4));
}
EXPORT_SYMBOL_GPL(usb_hcd_poll_rh_status);

/**
 * root hub周期性论寻状态信息定时器回调函数
 */
static void rh_timer_func (unsigned long _hcd)
{
	usb_hcd_poll_rh_status((struct usb_hcd *) _hcd);
}

/**
 * rh_queue_status:urb加入到hcd的status urb队列中并修改root hub的轮寻时间
 */
static int rh_queue_status (struct usb_hcd *hcd, struct urb *urb)
{
	int		retval;
	unsigned long	flags;
	unsigned	len = 1 + (urb->dev->maxchild / 8);

	spin_lock_irqsave (&hcd_root_hub_lock, flags);
	if (hcd->status_urb || urb->transfer_buffer_length < len) {
		dev_dbg (hcd->self.controller, "not queuing rh status urb\n");
		retval = -EINVAL;
		goto done;
	}

	retval = usb_hcd_link_urb_to_ep(hcd, urb);
	if (retval)
		goto done;

	hcd->status_urb = urb;
	urb->hcpriv = hcd;	/* indicate it's queued */
	if (!hcd->uses_new_polling)
		mod_timer(&hcd->rh_timer, (jiffies/(HZ/4) + 1) * (HZ/4));

	/* If a status change has already occurred, report it ASAP */
	else if (hcd->poll_pending)
		mod_timer(&hcd->rh_timer, jiffies);
	retval = 0;
 done:
	spin_unlock_irqrestore (&hcd_root_hub_lock, flags);
	return retval;
}

/**
 * rh_urb_enqueue:root hub urb的传输控制接口
 */
static int rh_urb_enqueue (struct usb_hcd *hcd, struct urb *urb)
{
	/*root hub上游端口*/
	if (usb_endpoint_xfer_int(&urb->ep->desc)) {
		return rh_queue_status (hcd, urb);
	}

	if (usb_endpoint_xfer_control(&urb->ep->desc))
		return rh_call_control (hcd, urb);
	return -EINVAL;
}


/**
 * usb_rh_urb_dequeue:root hub取消urb的传输
 * @hcd:usb 主机结构
 * @urb:待取消的urb
 * @status:取消状态码
*/
static int usb_rh_urb_dequeue(struct usb_hcd *hcd, struct urb *urb, int status)
{
	unsigned long	flags;
	int		rc;

	spin_lock_irqsave(&hcd_root_hub_lock, flags);
	rc = usb_hcd_check_unlink_urb(hcd, urb, status);
	if (rc)
		goto done;

	if (usb_endpoint_num(&urb->ep->desc) == 0) /* Control URB */
		goto done;	/*如果是控制传输，则do nothing why?*/

	if (!hcd->uses_new_polling)
		del_timer (&hcd->rh_timer);

	/*非状态urb也do nothing why?*/
	if (urb != hcd->status_urb)
		goto done;

	if (urb == hcd->status_urb) {
	/*如果是状态urb则将urb返回给root hub*/
		hcd->status_urb = NULL;
		usb_hcd_unlink_urb_from_ep(hcd, urb);

		spin_unlock(&hcd_root_hub_lock);
		usb_hcd_giveback_urb(hcd, urb, status);
		spin_lock(&hcd_root_hub_lock);
	}
 done:
	spin_unlock_irqrestore(&hcd_root_hub_lock, flags);
	return rc;
}

/*
 * Show & store the current value of authorized_default
 */
static ssize_t usb_host_authorized_default_show(struct device *dev,
						struct device_attribute *attr,
						char *buf)
{
	struct usb_device *rh_usb_dev = to_usb_device(dev);
	struct usb_bus *usb_bus = rh_usb_dev->bus;
	struct usb_hcd *usb_hcd;

	if (usb_bus == NULL)	/* FIXME: not sure if this case is possible */
		return -ENODEV;
	usb_hcd = bus_to_hcd(usb_bus);
	return snprintf(buf, PAGE_SIZE, "%u\n", usb_hcd->authorized_default);
}

static ssize_t usb_host_authorized_default_store(struct device *dev,
						 struct device_attribute *attr,
						 const char *buf, size_t size)
{
	ssize_t result;
	unsigned val;
	struct usb_device *rh_usb_dev = to_usb_device(dev);
	struct usb_bus *usb_bus = rh_usb_dev->bus;
	struct usb_hcd *usb_hcd;

	if (usb_bus == NULL)	/* FIXME: not sure if this case is possible */
		return -ENODEV;
	usb_hcd = bus_to_hcd(usb_bus);
	result = sscanf(buf, "%u\n", &val);
	if (result == 1) {
		usb_hcd->authorized_default = val? 1 : 0;
		result = size;
	}
	else
		result = -EINVAL;
	return result;
}

static DEVICE_ATTR(authorized_default, 0644,
	    usb_host_authorized_default_show,
	    usb_host_authorized_default_store);


/* Group all the USB bus attributes */
static struct attribute *usb_bus_attrs[] = {
		&dev_attr_authorized_default.attr,
		NULL,
};

static struct attribute_group usb_bus_attr_group = {
	.name = NULL,	/* we want them in the same directory */
	.attrs = usb_bus_attrs,
};

/**
 * usb_bus_init :初始化usb bus设备结构
 */
static void usb_bus_init (struct usb_bus *bus)
{
	memset (&bus->devmap, 0, sizeof(struct usb_devmap));

	bus->devnum_next = 1;

	bus->root_hub = NULL;
	bus->busnum = -1;
	bus->bandwidth_allocated = 0;
	bus->bandwidth_int_reqs  = 0;
	bus->bandwidth_isoc_reqs = 0;

	INIT_LIST_HEAD (&bus->bus_list);
}

/**
 * usb_register_bus : 向usb_bus_list 加入usb_bus节点
 */
static int usb_register_bus(struct usb_bus *bus)
{
	int result = -E2BIG;
	int busnum;

	mutex_lock(&usb_bus_list_lock);
	/*从busmap.busmap位图中获得一个总线编号*/
	busnum = find_next_zero_bit (busmap.busmap, USB_MAXBUS, 1);
	if (busnum >= USB_MAXBUS) {
		printk (KERN_ERR "%s: too many buses\n", usbcore_name);
		goto error_find_busnum;
	}
	/*设置bus的总线编号*/
	set_bit (busnum, busmap.busmap);
	bus->busnum = busnum; 

	/* Add it to the local list of buses */
	list_add (&bus->bus_list, &usb_bus_list);
	mutex_unlock(&usb_bus_list_lock);

	usb_notify_add_bus(bus);

	dev_info (bus->controller, "new USB bus registered, assigned bus "
		  "number %d\n", bus->busnum);
	return 0;

error_find_busnum:
	mutex_unlock(&usb_bus_list_lock);
	return result;
}

/**
 * usb_deregister_bus:从usb_bus_list 删除usb_bus节点
 */
static void usb_deregister_bus (struct usb_bus *bus)
{
	dev_info (bus->controller, "USB bus %d deregistered\n", bus->busnum);

	/*
	 * NOTE: make sure that all the devices are removed by the
	 * controller code, as well as having it call this when cleaning
	 * itself up
	 */
	mutex_lock(&usb_bus_list_lock);
	list_del (&bus->bus_list);
	mutex_unlock(&usb_bus_list_lock);

	usb_notify_remove_bus(bus);

	clear_bit (bus->busnum, busmap.busmap);
}

/**
 * register_root_hub:注册hub设备
 */
static int register_root_hub(struct usb_hcd *hcd)
{
	struct device *parent_dev = hcd->self.controller;
	struct usb_device *usb_dev = hcd->self.root_hub;
	const int devnum = 1; /*root hub的设备号总是为1*/
	int retval;

	/*初始化root hub设备*/
	usb_dev->devnum = devnum;
	usb_dev->bus->devnum_next = devnum + 1;
	/*清空root hub的USB设备管理位图*/
	memset (&usb_dev->bus->devmap.devicemap, 0,
			sizeof usb_dev->bus->devmap.devicemap);
	/*设置root hub设备的位图*/
	set_bit (devnum, usb_dev->bus->devmap.devicemap);
	/*设置root hub设备的状态*/
	usb_set_device_state(usb_dev, USB_STATE_ADDRESS);

	mutex_lock(&usb_bus_list_lock);

	usb_dev->ep0.desc.wMaxPacketSize = cpu_to_le16(64);
	/*获得root-hub的描述信息*/
	retval = usb_get_device_descriptor(usb_dev, USB_DT_DEVICE_SIZE);
	if (retval != sizeof usb_dev->descriptor) {
		mutex_unlock(&usb_bus_list_lock);
		dev_dbg (parent_dev, "can't read %s device descriptor %d\n",
				dev_name(&usb_dev->dev), retval);
		return (retval < 0) ? retval : -EMSGSIZE;
	}

	/*注册root-hub*/
	retval = usb_add_device(usb_dev);
	if (retval) {
		dev_err (parent_dev, "can't register root hub for %s, %d\n",
				dev_name(&usb_dev->dev), retval);
	}
	mutex_unlock(&usb_bus_list_lock);

	if (retval == 0) {
		spin_lock_irq (&hcd_root_hub_lock);
		hcd->rh_registered = 1;
		spin_unlock_irq (&hcd_root_hub_lock);

		/* Did the HC die before the root hub was registered? */
		if (hcd->state == HC_STATE_HALT)
			usb_hc_died (hcd);	/* This time clean up */
	}

	return retval;
}

/**
 * usb_calc_bus_time :计算周期性传输bytecount字节需要的总线时间(ns)
 * @speed:usb设备的传输速度类型 
 * @is_input:是否为输入传输标记 
 * @isoc:是否为等时传输标记 
 * @bytecount:周期性传输的字节数
 * @Returns approximate bus time in nanoseconds for a periodic transaction.
 * See USB 2.0 spec section 5.11.3; only periodic transfers need to be
 * scheduled in software, this function is only used for such scheduling.
 */
long usb_calc_bus_time (int speed, int is_input, int isoc, int bytecount)
{
	unsigned long	tmp;

	switch (speed) {
	case USB_SPEED_LOW: 	/* INTR only */
		if (is_input) {
			tmp = (67667L * (31L + 10L * BitTime (bytecount))) / 1000L;
			return (64060L + (2 * BW_HUB_LS_SETUP) + BW_HOST_DELAY + tmp);
		} else {
			tmp = (66700L * (31L + 10L * BitTime (bytecount))) / 1000L;
			return (64107L + (2 * BW_HUB_LS_SETUP) + BW_HOST_DELAY + tmp);
		}
	case USB_SPEED_FULL:	/* ISOC or INTR */
		if (isoc) {
			tmp = (8354L * (31L + 10L * BitTime (bytecount))) / 1000L;
			return (((is_input) ? 7268L : 6265L) + BW_HOST_DELAY + tmp);
		} else {
			tmp = (8354L * (31L + 10L * BitTime (bytecount))) / 1000L;
			return (9107L + BW_HOST_DELAY + tmp);
		}
	case USB_SPEED_HIGH:	/* ISOC or INTR */
		// FIXME adjust for input vs output
		if (isoc)
			tmp = HS_NSECS_ISO (bytecount);
		else
			tmp = HS_NSECS (bytecount);
		return tmp;
	default:
		pr_debug ("%s: bogus device speed!\n", usbcore_name);
		return -1;
	}
}
EXPORT_SYMBOL_GPL(usb_calc_bus_time);
/**
 * usb_hcd_link_urb_to_ep:向USB设备端口urb传输队列加入urb
 * @hcd: usb主机结构 
 * @urb: 传输的urb结构 
 */
int usb_hcd_link_urb_to_ep(struct usb_hcd *hcd, struct urb *urb)
{
	int		rc = 0;

	spin_lock(&hcd_urb_list_lock);

	/*检查是否允许被提交给ep*/
	if (unlikely(atomic_read(&urb->reject))) {
		rc = -EPERM;
		goto done;
	}
	/*主机未使能*/
	if (unlikely(!urb->ep->enabled)) {
		rc = -ENOENT;
		goto done;
	}
	/*usb设备未初始化*/
	if (unlikely(!urb->dev->can_submit)) {
		rc = -EHOSTUNREACH;
		goto done;
	}
	/*将urb加入到urb制定的ep的urb传输队列中*/
	if (hcd->state == HC_STATE_RUNNING 
			|| hcd->state == HC_STATE_RESUMING){
		urb->unlinked = 0;  /*表示当前的urb已经连入到主机的ep的urb队列中*/
		/*该urb加入到ep的传输队列中*/
		list_add_tail(&urb->urb_list, &urb->ep->urb_list);

	} else
		rc = -ESHUTDOWN;

 done:
	spin_unlock(&hcd_urb_list_lock);
	return rc;
}
EXPORT_SYMBOL_GPL(usb_hcd_link_urb_to_ep);

/**
 * usb_hcd_check_unlink_urb:设置urb的传输错误码
 * @hcd: usb 主机结构 
 * @urb: 待检查的urb 
 * @status错误状态码 
 *	-EIDRM: @urb was not submitted or has already completed.
 *		The completion function may not have been called yet.
 *	-EBUSY: @urb has already been unlinked.
 */
int usb_hcd_check_unlink_urb(struct usb_hcd *hcd, struct urb *urb,
		int status)
{
	struct list_head	*tmp;

	/*在ep的urb链表中查找输入参数的urb链表节点*/
	list_for_each(tmp, &urb->ep->urb_list) {
		if (tmp == &urb->urb_list)
			break;
	}
	if (tmp != &urb->urb_list)
		return -EIDRM;

	/* Any status except -EINPROGRESS means something already started to
	 * unlink this URB from the hardware.  So there's no more work to do.
	 * 已经是未连入状态*/
	if (urb->unlinked)
		return -EBUSY;
	/*通过以上的检查,urb此时的状态为urb->unlinked = 0,
	 * 因此将取消传输的状态码保存在urb->unlinked中*/
	urb->unlinked = status;

	/* IRQ setup can easily be broken so that USB controllers
	 * never get completion IRQs ... maybe even the ones we need to
	 * finish unlinking the initial failed usb_set_address()
	 * or device descriptor fetch.
	 */
	if (!test_bit(HCD_FLAG_SAW_IRQ, &hcd->flags) &&
			!is_root_hub(urb->dev)) {
		dev_warn(hcd->self.controller, "Unlink after no-IRQ?  "
			"Controller is probably using the wrong IRQ.\n");
		set_bit(HCD_FLAG_SAW_IRQ, &hcd->flags);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(usb_hcd_check_unlink_urb);

/**
 * usb_hcd_unlink_urb_from_ep: 从ep传输队列中删除urb节点
 */
void usb_hcd_unlink_urb_from_ep(struct usb_hcd *hcd, struct urb *urb)
{
	/* clear all state linking urb to this dev (and hcd) */
	spin_lock(&hcd_urb_list_lock);
	list_del_init(&urb->urb_list);
	spin_unlock(&hcd_urb_list_lock);
}
EXPORT_SYMBOL_GPL(usb_hcd_unlink_urb_from_ep);

/*
 * Some usb host controllers can only perform dma using a small SRAM area.
 * The usb core itself is however optimized for host controllers that can dma
 * using regular system memory - like pci devices doing bus mastering.
 *
 * To support host controllers with limited dma capabilites we provide dma
 * bounce buffers. This feature can be enabled using the HCD_LOCAL_MEM flag.
 * For this to work properly the host controller code must first use the
 * function dma_declare_coherent_memory() to point out which memory area
 * that should be used for dma allocations.
 *
 * The HCD_LOCAL_MEM flag then tells the usb code to allocate all data for
 * dma using dma_alloc_coherent() which in turn allocates from the memory
 * area pointed out with dma_declare_coherent_memory().
 *
 * So, to summarize...
 *
 * - We need "local" memory, canonical example being
 *   a small SRAM on a discrete controller being the
 *   only memory that the controller can read ...
 *   (a) "normal" kernel memory is no good, and
 *   (b) there's not enough to share
 *
 * - The only *portable* hook for such stuff in the
 *   DMA framework is dma_declare_coherent_memory()
 *
 * - So we use that, even though the primary requirement
 *   is that the memory be "local" (hence addressible
 *   by that device), not "coherent".
 *
 */

/**
 * hcd_alloc_coherent:申请回弹缓存区
 */
static int hcd_alloc_coherent(struct usb_bus *bus,
			      gfp_t mem_flags, dma_addr_t *dma_handle,
			      void **vaddr_handle, size_t size,
			      enum dma_data_direction dir)
{
	unsigned char *vaddr;

	vaddr = hcd_buffer_alloc(bus, size + sizeof(vaddr),
				 mem_flags, dma_handle);
	if (!vaddr)
		return -ENOMEM;

	/*
	 * Store the virtual address of the buffer at the end
	 * of the allocated dma buffer. The size of the buffer
	 * may be uneven so use unaligned functions instead
	 * of just rounding up. It makes sense to optimize for
	 * memory footprint over access speed since the amount
	 * of memory available for dma may be limited.
	 * 将*vaddr_handle的值存储在vaddr+size位置处,并且对齐于sizeof(*(vaddr + size))
	 */
	put_unaligned((unsigned long)*vaddr_handle,
		      (unsigned long *)(vaddr + size));

	/*将vaddr_handle的内容拷贝到新申请的DMAbuffer中
	 * 目的：实现回弹dma buffer*/
	if (dir == DMA_TO_DEVICE)
		memcpy(vaddr, *vaddr_handle, size);

	/*保存buffer的虚拟地址*/
	*vaddr_handle = vaddr;
	return 0;
}

/*
 * hcd_free_coherent:释放申请的回弹缓冲区
 * @ bus:
 * @ dma_handle:
 * @ vaddr_handle:
 * @ size:
 * @ dir:
 * @ note:
 * bounce buffer的实现和作用
*/
static void hcd_free_coherent(struct usb_bus *bus, dma_addr_t *dma_handle,
			      void **vaddr_handle, size_t size,
			      enum dma_data_direction dir)
{
	unsigned char *vaddr = *vaddr_handle;

	/*从buffer的末端,获得虚拟地址*/
	vaddr = (void *)get_unaligned((unsigned long *)(vaddr + size));

	/*将bounce buffer缓存的数据重新复制到vaddr虚拟地址处*/
	if (dir == DMA_FROM_DEVICE)
		memcpy(vaddr, *vaddr_handle, size);

	hcd_buffer_free(bus, size + sizeof(vaddr), *vaddr_handle, *dma_handle);

	*vaddr_handle = vaddr;
	*dma_handle = 0;
}

/**
 * map_urb_for_dma: urb进行dma的操作地址映射
 * @ hcd:
 * @ urb:
 * @ mem_flags:
*/
static int map_urb_for_dma(struct usb_hcd *hcd, struct urb *urb,
			   gfp_t mem_flags)
{
	enum dma_data_direction dir;
	int ret = 0;

	/* Map the URB's buffers for DMA access.
	 * Lower level HCD code should use *_dma exclusively,
	 * unless it uses pio or talks to another transport,
	 * or uses the provided scatter gather list for bulk.
	 */
	if (is_root_hub(urb->dev))
		return 0;

	/*如果是控制传输,则为steup数据包建立缓存区*/
	if (usb_endpoint_xfer_control(&urb->ep->desc)
	    && !(urb->transfer_flags & URB_NO_SETUP_DMA_MAP)) {
		if (hcd->self.uses_dma)
			urb->setup_dma = dma_map_single(
					hcd->self.controller,
					urb->setup_packet,
					sizeof(struct usb_ctrlrequest),
					DMA_TO_DEVICE);
		else if (hcd->driver->flags & HCD_LOCAL_MEM){
			/*建立回弹缓存区解决访问高端地址的问题*/
			ret = hcd_alloc_coherent(
					urb->dev->bus, mem_flags,
					&urb->setup_dma,
					(void **)&urb->setup_packet,
					sizeof(struct usb_ctrlrequest),
					DMA_TO_DEVICE);
			if (ret)
				goto exit;
		}
	}

	/*给传输transfer_buffer数据包建立缓存区*/
	dir = usb_urb_dir_in(urb) ? DMA_FROM_DEVICE : DMA_TO_DEVICE;
	if (ret == 0 && urb->transfer_buffer_length != 0
	    && !(urb->transfer_flags & URB_NO_TRANSFER_DMA_MAP)) {
		if (hcd->self.uses_dma)
			urb->transfer_dma = dma_map_single (
					hcd->self.controller,
					urb->transfer_buffer,
					urb->transfer_buffer_length,
					dir);
		else if (hcd->driver->flags & HCD_LOCAL_MEM) {
			ret = hcd_alloc_coherent(
					urb->dev->bus, mem_flags,
					&urb->transfer_dma,
					&urb->transfer_buffer,
					urb->transfer_buffer_length,
					dir);
            if (ret)
				goto ctl_error;
		}
	}
exit:
	return 0;

ctl_error:
	if (usb_endpoint_xfer_control(&urb->ep->desc) &&
		!(urb->transfer_flags & URB_NO_SETUP_DMA_MAP))
		hcd_free_coherent(urb->dev->bus,
			&urb->setup_dma,
			(void **)&urb->setup_packet,
			sizeof(struct usb_ctrlrequest),
			DMA_TO_DEVICE);
	return ret;

}

/**
 * unmap_urb_for_dma: 取消urb传输的dma地址映射
 * @ hcd:
 * @ urb:
 * @ mem_flags:
*/
static void unmap_urb_for_dma(struct usb_hcd *hcd, struct urb *urb)
{
	enum dma_data_direction dir;

	if (is_root_hub(urb->dev))
		return;

	if (usb_endpoint_xfer_control(&urb->ep->desc)
	    && !(urb->transfer_flags & URB_NO_SETUP_DMA_MAP)) {
		if (hcd->self.uses_dma)
			dma_unmap_single(hcd->self.controller, urb->setup_dma,
					sizeof(struct usb_ctrlrequest),
					DMA_TO_DEVICE);
		else if (hcd->driver->flags & HCD_LOCAL_MEM)
			hcd_free_coherent(urb->dev->bus, &urb->setup_dma,
					(void **)&urb->setup_packet,
					sizeof(struct usb_ctrlrequest),
					DMA_TO_DEVICE);
	}

	dir = usb_urb_dir_in(urb) ? DMA_FROM_DEVICE : DMA_TO_DEVICE;
	if (urb->transfer_buffer_length != 0
	    && !(urb->transfer_flags & URB_NO_TRANSFER_DMA_MAP)) {
		if (hcd->self.uses_dma)
			dma_unmap_single(hcd->self.controller,
					urb->transfer_dma,
					urb->transfer_buffer_length,
					dir);
		else if (hcd->driver->flags & HCD_LOCAL_MEM)
			hcd_free_coherent(urb->dev->bus, &urb->transfer_dma,
					&urb->transfer_buffer,
					urb->transfer_buffer_length,
					dir);
	}
}

/**
 * usb_hcd_submit_urb :向usb 主机提交urb传输结构
 * @ urb:待传输的urb结构
 */
int usb_hcd_submit_urb (struct urb *urb, gfp_t mem_flags)
{
	int			status;
	struct usb_hcd		*hcd = bus_to_hcd(urb->dev->bus);

	/* increment urb's reference count as part of giving it to the HCD
	 * (which will control it).  HCD guarantees that it either returns
	 * an error or calls giveback(), but not both.
	 */
	usb_get_urb(urb);
	atomic_inc(&urb->use_count);
	atomic_inc(&urb->dev->urbnum);
	usbmon_urb_submit(&hcd->self, urb);

	/* NOTE requirements on root-hub callers (usbfs and the hub
	 * driver, for now):  URBs' urb->transfer_buffer must be
	 * valid and usb_buffer_{sync,unmap}() not be needed, since
	 * they could clobber root hub response data.  Also, control
	 * URBs must be submitted in process context with interrupts
	 * enabled.
	 * urb进行dma传输地址映射
	 */
	status = map_urb_for_dma(hcd, urb, mem_flags);
	if (unlikely(status)) {
		usbmon_urb_submit_error(&hcd->self, urb, status);
		goto error;
	}

	if (is_root_hub(urb->dev)) {
		status = rh_urb_enqueue(hcd, urb);
	}
	else
		status = hcd->driver->urb_enqueue(hcd, urb, mem_flags);

	if (unlikely(status)) {
		usbmon_urb_submit_error(&hcd->self, urb, status);
		unmap_urb_for_dma(hcd, urb);
 error:
		urb->hcpriv = NULL;
		INIT_LIST_HEAD(&urb->urb_list);
		atomic_dec(&urb->use_count);
		atomic_dec(&urb->dev->urbnum);
		if (atomic_read(&urb->reject))
			wake_up(&usb_kill_urb_queue);
		usb_put_urb(urb);
	}
	return status;
}

/**
 * unlink1:取消urb的传输
 * @ hcd: usb主机结构
 * @ urb:待取消的urb
 * @ status:取消状态码
*/
static int unlink1(struct usb_hcd *hcd, struct urb *urb, int status)
{
	int		value;

	if (is_root_hub(urb->dev))
		value = usb_rh_urb_dequeue(hcd, urb, status);
	else 
		value = hcd->driver->urb_dequeue(hcd, urb, status);
	
	return value;
}

/*
 *usb_hcd_unlink_urb:将urb从主机的传输队列中删除
 *@ urb:待删除的urb节点
 *@ status:urb删除urb状态码
 */
int usb_hcd_unlink_urb (struct urb *urb, int status)
{
	struct usb_hcd		*hcd;
	int			retval = -EIDRM;
	unsigned long		flags;

	/*注:只取消urb->use_count = 0的urb
	 * 当urb被加入到主机之后,urb->use_count = 0
	 * 因此,对于没有加入到主机的urb是不进行unlinked操作的
	 */
	spin_lock_irqsave(&hcd_urb_unlink_lock, flags);
	if (atomic_read(&urb->use_count) > 0) {
		retval = 0;
		usb_get_dev(urb->dev);
	}
	spin_unlock_irqrestore(&hcd_urb_unlink_lock, flags);
	if (retval == 0) {
		hcd = bus_to_hcd(urb->dev->bus);
		retval = unlink1(hcd, urb, status);
		usb_put_dev(urb->dev);
	}

	if (retval == 0)
		retval = -EINPROGRESS; /*表示删除成功*/
	else if (retval != -EIDRM && retval != -EBUSY)
		dev_dbg(&urb->dev->dev, "hcd_unlink_urb %p fail %d\n",
				urb, retval);
	return retval;
}

/**
 * usb_hcd_giveback_urb: usb主机将urb返回给usb设备驱动(也就是回调urb->complete接口)
 * @hcd: usb主机结构 
 * @urb: 待返回给设备驱动的urb 
 * @status:完成状态码 
 * Context:中断上下文中调用 
 *
 * 注:urb起初是由设备驱动程序填充的，包括其complete接口,此时称将urb返回给设备驱动，
 * 也就是调用urb->completion接口.
 */
void usb_hcd_giveback_urb(struct usb_hcd *hcd, struct urb *urb, int status)
{
	urb->hcpriv = NULL;
	/*如果urb是被取消的，则获得它的取消操作码*/
	if (unlikely(urb->unlinked))
		status = urb->unlinked;  
	/*未收到应答或者传输的数据长度不是期望值时的错误*/
	if (unlikely((urb->transfer_flags & URB_SHORT_NOT_OK) &&
		urb->actual_length < urb->transfer_buffer_length &&
			!status))
		status = -EREMOTEIO;  

	/*取消urb的dma映射*/
	unmap_urb_for_dma(hcd, urb);
	usbmon_urb_complete(&hcd->self, urb, status);
	usb_unanchor_urb(urb);

	/*调用urb完成回调函数*/
	urb->status = status;
	urb->complete (urb);
	atomic_dec (&urb->use_count);
	if (unlikely(atomic_read(&urb->reject)))
		wake_up (&usb_kill_urb_queue);
	usb_put_urb (urb);
}
EXPORT_SYMBOL_GPL(usb_hcd_giveback_urb);

/**
 * usb_hcd_flush_endpoint:取消加入到ep的urb传输队列并删除
 * @ udev:
 * @ ep:endpoint
 */
void usb_hcd_flush_endpoint(struct usb_device *udev,
		struct usb_host_endpoint *ep)
{
	struct usb_hcd		*hcd;
	struct urb		*urb;

	if (!ep)
		return;
	might_sleep();
	hcd = bus_to_hcd(udev->bus);

	/* No more submits can occur */
	spin_lock_irq(&hcd_urb_list_lock);
rescan:
	/*取消已经加入到ep传输队列中的urb的传输*/
	list_for_each_entry (urb, &ep->urb_list, urb_list) {
		int	is_in;

		/*如果urb已经被取消*/
		if (urb->unlinked)
			continue;
		usb_get_urb (urb);
		is_in = usb_urb_dir_in(urb);
		spin_unlock(&hcd_urb_list_lock);

		/*取消urb在主机传输队列中的传输*/
		unlink1(hcd, urb, -ESHUTDOWN);
		usb_put_urb (urb);
		/* list contents may have changed */
		spin_lock(&hcd_urb_list_lock);
		goto rescan;
	}
	spin_unlock_irq(&hcd_urb_list_lock);

	/*删除ep的urb传输队列中的urb结构*/
	while (!list_empty (&ep->urb_list)) {
		spin_lock_irq(&hcd_urb_list_lock);

		/* The list may have changed while we acquired the spinlock */
		urb = NULL;
		if (!list_empty (&ep->urb_list)) {
			urb = list_entry (ep->urb_list.prev, struct urb,
					urb_list);
			usb_get_urb (urb);
		}
		spin_unlock_irq(&hcd_urb_list_lock);

		if (urb) {
			usb_kill_urb (urb);
			usb_put_urb (urb);
		}
	}
}

/**
 * usb_hcd_check_bandwidth:检查usb设备新的configuration或者新的interface的usb总线的传输带宽的有效性
 */
int usb_hcd_check_bandwidth(struct usb_device *udev,
		struct usb_host_config *new_config,
		struct usb_interface *new_intf)
{
	int num_intfs, i, j;
	struct usb_interface_cache *intf_cache;
	struct usb_host_interface *alt = 0;
	int ret = 0;
	struct usb_hcd *hcd;
	struct usb_host_endpoint *ep;

	hcd = bus_to_hcd(udev->bus);
	if (!hcd->driver->check_bandwidth)
		return 0;

	/* Configuration is being removed - set configuration 0 */
	if (!new_config && !new_intf) {
		for (i = 1; i < 16; ++i) {
			ep = udev->ep_out[i];
			if (ep)
				hcd->driver->drop_endpoint(hcd, udev, ep);
			ep = udev->ep_in[i];
			if (ep)
				hcd->driver->drop_endpoint(hcd, udev, ep);
		}
		hcd->driver->check_bandwidth(hcd, udev);
		return 0;
	}
	/* Check if the HCD says there's enough bandwidth.  Enable all endpoints
	 * each interface's alt setting 0 and ask the HCD to check the bandwidth
	 * of the bus.  There will always be bandwidth for endpoint 0, so it's
	 * ok to exclude it.
	 */
	if (new_config) {
		num_intfs = new_config->desc.bNumInterfaces;
		/* Remove endpoints (except endpoint 0, which is always on the
		 * schedule) from the old config from the schedule
		 */
		for (i = 1; i < 16; ++i) {
			ep = udev->ep_out[i];
			if (ep) {
				ret = hcd->driver->drop_endpoint(hcd, udev, ep);
				if (ret < 0)
					goto reset;
			}
			ep = udev->ep_in[i];
			if (ep) {
				ret = hcd->driver->drop_endpoint(hcd, udev, ep);
				if (ret < 0)
					goto reset;
			}
		}
		for (i = 0; i < num_intfs; ++i) {

			/* Dig the endpoints for alt setting 0 out of the
			 * interface cache for this interface
			 */
			intf_cache = new_config->intf_cache[i];
			for (j = 0; j < intf_cache->num_altsetting; j++) {
				if (intf_cache->altsetting[j].desc.bAlternateSetting == 0)
					alt = &intf_cache->altsetting[j];
			}
			if (!alt) {
				printk(KERN_DEBUG "Did not find alt setting 0 for intf %d\n", i);
				continue;
			}
			for (j = 0; j < alt->desc.bNumEndpoints; j++) {
				ret = hcd->driver->add_endpoint(hcd, udev, &alt->endpoint[j]);
				if (ret < 0)
					goto reset;
			}
		}
	}
	ret = hcd->driver->check_bandwidth(hcd, udev);
reset:
	if (ret < 0)
		hcd->driver->reset_bandwidth(hcd, udev);
	return ret;
}

/**
 * usb_hcd_disable_endpoint:禁止USB设备ep端口
 */
void usb_hcd_disable_endpoint(struct usb_device *udev,
		struct usb_host_endpoint *ep)
{
	struct usb_hcd		*hcd;

	might_sleep();
	hcd = bus_to_hcd(udev->bus);
	if (hcd->driver->endpoint_disable)
		hcd->driver->endpoint_disable(hcd, ep);
}

/**
 * usb_hcd_reset_endpoint:复位USB设备端口
 */
void usb_hcd_reset_endpoint(struct usb_device *udev,
			    struct usb_host_endpoint *ep)
{
	struct usb_hcd *hcd = bus_to_hcd(udev->bus);

	if (hcd->driver->endpoint_reset)
		hcd->driver->endpoint_reset(hcd, ep);
	else {
		int epnum = usb_endpoint_num(&ep->desc);
		int is_out = usb_endpoint_dir_out(&ep->desc);
		int is_control = usb_endpoint_xfer_control(&ep->desc);

		usb_settoggle(udev, epnum, is_out, 0);
		if (is_control)
			usb_settoggle(udev, epnum, !is_out, 0);
	}
}

/* Protect against drivers that try to unlink URBs after the device
 * is gone, by waiting until all unlinks for @udev are finished.
 * Since we don't currently track URBs by device, simply wait until
 * nothing is running in the locked region of usb_hcd_unlink_urb().
 */
void usb_hcd_synchronize_unlinks(struct usb_device *udev)
{
	spin_lock_irq(&hcd_urb_unlink_lock);
	spin_unlock_irq(&hcd_urb_unlink_lock);
}


/**
 * usb_hcd_get_frame_number:返回数据传输帧编号
 */
int usb_hcd_get_frame_number(struct usb_device *udev)
{
	struct usb_hcd	*hcd = bus_to_hcd(udev->bus);

	if (!HC_IS_RUNNING (hcd->state))
		return -ESHUTDOWN;
	return hcd->driver->get_frame_number(hcd);
}

#ifdef	CONFIG_PM

/**
 * hcd_bus_suspend:挂起主机的root hub设备
 * @ rhdev: root hub 设备
 * @ msg:无用 
*/
int hcd_bus_suspend(struct usb_device *rhdev, pm_message_t msg)
{
	struct usb_hcd	*hcd = container_of(rhdev->bus, struct usb_hcd, self);
	int		status;
	int		old_state = hcd->state;

	dev_dbg(&rhdev->dev, "bus %s%s\n",
			(msg.event & PM_EVENT_AUTO ? "auto-" : ""), "suspend");
	if (!hcd->driver->bus_suspend) {
		status = -ENOENT;
	} else {
		hcd->state = HC_STATE_QUIESCING;
		status = hcd->driver->bus_suspend(hcd);
	}
	if (status == 0) {
		usb_set_device_state(rhdev, USB_STATE_SUSPENDED);
		hcd->state = HC_STATE_SUSPENDED;
	} else {
		hcd->state = old_state;
		dev_dbg(&rhdev->dev, "bus %s fail, err %d\n",
				"suspend", status);
	}
	return status;
}

/**
 * hcd_bus_resume:唤醒USB主机的root hub设备
 * @ rhdev: root hub 设备
 * @ msg:无用 
*/
int hcd_bus_resume(struct usb_device *rhdev, pm_message_t msg)
{
	struct usb_hcd	*hcd = container_of(rhdev->bus, struct usb_hcd, self);
	int		status;
	int		old_state = hcd->state;

	dev_dbg(&rhdev->dev, "usb %s%s\n",
			(msg.event & PM_EVENT_AUTO ? "auto-" : ""), "resume");
	if (!hcd->driver->bus_resume)
		return -ENOENT;
	if (hcd->state == HC_STATE_RUNNING)
		return 0;

	hcd->state = HC_STATE_RESUMING;
	status = hcd->driver->bus_resume(hcd);
	if (status == 0) {
		/* TRSMRCY = 10 msec */
		msleep(10);
		usb_set_device_state(rhdev, rhdev->actconfig
				? USB_STATE_CONFIGURED
				: USB_STATE_ADDRESS);
		hcd->state = HC_STATE_RUNNING;
	} else {
		hcd->state = old_state;
		dev_dbg(&rhdev->dev, "bus %s fail, err %d\n",
				"resume", status);
		if (status != -ESHUTDOWN)
			usb_hc_died(hcd);
	}
	return status;
}

/**
 * hcd_resume_work:usb主机唤醒root hub工作队列回调函数
 */
static void hcd_resume_work(struct work_struct *work)
{
	struct usb_hcd *hcd = container_of(work, struct usb_hcd, wakeup_work);
	struct usb_device *udev = hcd->self.root_hub;

	usb_lock_device(udev);
	usb_mark_last_busy(udev);
	usb_external_resume_device(udev, PMSG_REMOTE_RESUME);
	usb_unlock_device(udev);
}

/**
 * usb_hcd_resume_root_hub 唤醒usb主机的root hub 
 */
void usb_hcd_resume_root_hub (struct usb_hcd *hcd)
{
	unsigned long flags;

	spin_lock_irqsave (&hcd_root_hub_lock, flags);
	if (hcd->rh_registered)
		queue_work(ksuspend_usb_wq, &hcd->wakeup_work);
	spin_unlock_irqrestore (&hcd_root_hub_lock, flags);
}
EXPORT_SYMBOL_GPL(usb_hcd_resume_root_hub);

#endif

#ifdef	CONFIG_USB_OTG

/**
 * usb_bus_start_enum - start immediate enumeration (for OTG)
 * @bus: the bus (must use hcd framework)
 * @port_num: 1-based number of port; usually bus->otg_port
 * Context: in_interrupt()
 *
 * Starts enumeration, with an immediate reset followed later by
 * khubd identifying and possibly configuring the device.
 * This is needed by OTG controller drivers, where it helps meet
 * HNP protocol timing requirements for starting a port reset.
 */
int usb_bus_start_enum(struct usb_bus *bus, unsigned port_num)
{
	struct usb_hcd		*hcd;
	int			status = -EOPNOTSUPP;

	/* NOTE: since HNP can't start by grabbing the bus's address0_sem,
	 * boards with root hubs hooked up to internal devices (instead of
	 * just the OTG port) may need more attention to resetting...
	 */
	hcd = container_of (bus, struct usb_hcd, self);
	if (port_num && hcd->driver->start_port_reset)
		status = hcd->driver->start_port_reset(hcd, port_num);

	/* run khubd shortly after (first) root port reset finishes;
	 * it may issue others, until at least 50 msecs have passed.
	 */
	if (status == 0)
		mod_timer(&hcd->rh_timer, jiffies + msecs_to_jiffies(10));
	return status;
}
EXPORT_SYMBOL_GPL(usb_bus_start_enum);

#endif

/**
 * usb_hcd_irq :usb主机中断处理程序统一接口
 */
irqreturn_t usb_hcd_irq (int irq, void *__hcd)
{
	struct usb_hcd		*hcd = __hcd;
	unsigned long		flags;
	irqreturn_t		rc = IRQ_NONE;

	/* IRQF_DISABLED doesn't work correctly with shared IRQs
	 * when the first handler doesn't use it.  So let's just
	 * assume it's never used.
	 */
	local_irq_save(flags);

	/*这段逻辑拆分开应该更合理*/
	if (unlikely(hcd->state == HC_STATE_HALT ||
		     !test_bit(HCD_FLAG_HW_ACCESSIBLE, &hcd->flags))) {
		goto exit;
	}

	if (hcd->driver->irq(hcd) == IRQ_NONE) {
		goto exit;
	} else {
		set_bit(HCD_FLAG_SAW_IRQ, &hcd->flags);
		if (unlikely(hcd->state == HC_STATE_HALT))
			usb_hc_died(hcd);
		rc = IRQ_HANDLED;
	}
exit:
	local_irq_restore(flags);
	return rc;
}


/**
 * usb_hc_died :usb主机意外关闭
 *
 */
void usb_hc_died (struct usb_hcd *hcd)
{
	unsigned long flags;

	dev_err (hcd->self.controller, "HC died; cleaning up\n");

	spin_lock_irqsave (&hcd_root_hub_lock, flags);
	if (hcd->rh_registered) {
		hcd->poll_rh = 0;

		/*向hub 内核线程报告此事件*/
		usb_set_device_state (hcd->self.root_hub,
				USB_STATE_NOTATTACHED);
		usb_kick_khubd (hcd->self.root_hub);
	}
	spin_unlock_irqrestore (&hcd_root_hub_lock, flags);
}
EXPORT_SYMBOL_GPL (usb_hc_died);


/**
 * usb_create_hcd : 创建并初始化一个hcd结构
 * @ driver: hcd所属的驱动
 * @ dev: 设备模型
 * @ bus_name:主机的总线名称
 */
struct usb_hcd *usb_create_hcd (const struct hc_driver *driver,
		struct device *dev, const char *bus_name)
{
	struct usb_hcd *hcd;

	hcd = kzalloc(sizeof(*hcd) + driver->hcd_priv_size, GFP_KERNEL);
	if (!hcd) {
		dev_dbg (dev, "hcd alloc failed\n");
		return NULL;
	}
	dev_set_drvdata(dev, hcd);
	kref_init(&hcd->kref);

	usb_bus_init(&hcd->self);
	/*主机的bus属性,表明其也是一个设备*/
	hcd->self.controller = dev;
	hcd->self.bus_name = bus_name;
	/*主机是否可以进行dma传输*/
	hcd->self.uses_dma = (dev->dma_mask != NULL);

	/*创建root hub轮训的定时器*/
	init_timer(&hcd->rh_timer);
	hcd->rh_timer.function = rh_timer_func;
	hcd->rh_timer.data = (unsigned long) hcd;
#ifdef CONFIG_PM
	INIT_WORK(&hcd->wakeup_work, hcd_resume_work);
#endif

	/*hcd主机绑定驱动*/
	hcd->driver = driver;
	hcd->product_desc = (driver->product_desc) ? driver->product_desc :
			"USB Host Controller";
	return hcd;
}
EXPORT_SYMBOL_GPL(usb_create_hcd);

/**
 * hcd_release:释放hcd结构
 */
static void hcd_release (struct kref *kref)
{
	struct usb_hcd *hcd = container_of (kref, struct usb_hcd, kref);

	kfree(hcd);
}

/**
 * usb_get_hcd:增加hcd的引用计数
 */
struct usb_hcd *usb_get_hcd (struct usb_hcd *hcd)
{
	if (hcd)
		kref_get (&hcd->kref);
	return hcd;
}
EXPORT_SYMBOL_GPL(usb_get_hcd);

/**
 * usb_put_hcd:递减主机引用计数
 */
void usb_put_hcd (struct usb_hcd *hcd)
{
	if (hcd)
		kref_put (&hcd->kref, hcd_release);
}
EXPORT_SYMBOL_GPL(usb_put_hcd);

/**
 * usb_add_hcd : 向usb总线注册一个usb主机和它的root hub设备
 * @ hcd:       主机结构
 * @ irqnum:    主机所拥有的中断号
 * @ irqflags:  中断标记
 */
int usb_add_hcd(struct usb_hcd *hcd,
		unsigned int irqnum, unsigned long irqflags)
{
	int retval;
	struct usb_device *rhdev;

	dev_info(hcd->self.controller, "%s\n", hcd->product_desc);

	hcd->authorized_default = hcd->wireless? 0 : 1;
	set_bit(HCD_FLAG_HW_ACCESSIBLE, &hcd->flags);

	/* HC is in reset state, but accessible.  Now do the one-time init,
	 * bottom up so that hcds can customize the root hubs before khubd
	 * starts talking to them.  (Note, bus id is assigned early too.)
	 */
	if ((retval = hcd_buffer_create(hcd)) != 0) {
		dev_dbg(hcd->self.controller, "pool alloc failed\n");
		return retval;
	}

	if ((retval = usb_register_bus(&hcd->self)) < 0)
		goto err_register_bus;

	/* 为hcd申请root hub USB设备rhdev->parent = NULL)*/ 
	if ((rhdev = usb_alloc_dev(NULL, &hcd->self, 0)) == NULL) {
		dev_err(hcd->self.controller, "unable to allocate root hub\n");
		retval = -ENOMEM;
		goto err_allocate_root_hub;
	}

	switch (hcd->driver->flags & HCD_MASK) {
	case HCD_USB11:
		rhdev->speed = USB_SPEED_FULL;
		break;
	case HCD_USB2:
		rhdev->speed = USB_SPEED_HIGH;
		break;
	case HCD_USB3:
		rhdev->speed = USB_SPEED_SUPER;
		break;
	default:
		goto err_allocate_root_hub;
	}
	hcd->self.root_hub = rhdev; /*主机的root hub device*/

	/* wakeup flag init defaults to "everything works" for root hubs,
	 * but drivers can override it in reset() if needed, along with
	 * recording the overall controller's system wakeup capability.
	 */
	device_init_wakeup(&rhdev->dev, 1);

	/* "reset" is misnamed; its role is now one-time init. the controller
	 * should already have been reset (and boot firmware kicked off etc).
	 */
	if (hcd->driver->reset && (retval = hcd->driver->reset(hcd)) < 0) {
		dev_err(hcd->self.controller, "can't setup\n");
		goto err_hcd_driver_setup;
	}

	/* NOTE: root hub and controller capabilities may not be the same */
	if (device_can_wakeup(hcd->self.controller)
			&& device_can_wakeup(&hcd->self.root_hub->dev))
		dev_dbg(hcd->self.controller, "supports USB remote wakeup\n");

	/* enable irqs just before we start the controller */
	if (hcd->driver->irq) {

		/* IRQF_DISABLED doesn't work as advertised when used together
		 * with IRQF_SHARED. As usb_hcd_irq() will always disable
		 * interrupts we can remove it here.
		 */
		if (irqflags & IRQF_SHARED)
			irqflags &= ~IRQF_DISABLED;

		snprintf(hcd->irq_descr, sizeof(hcd->irq_descr), "%s:usb%d",
				hcd->driver->description, hcd->self.busnum);
		if ((retval = request_irq(irqnum, &usb_hcd_irq, irqflags,
				hcd->irq_descr, hcd)) != 0) {
			dev_err(hcd->self.controller,
					"request interrupt %d failed\n", irqnum);
			goto err_request_irq;
		}
		hcd->irq = irqnum;
		dev_info(hcd->self.controller, "irq %d, %s 0x%08llx\n", irqnum,
				(hcd->driver->flags & HCD_MEMORY) ?
					"io mem" : "io base",
					(unsigned long long)hcd->rsrc_start);
	} else {
		hcd->irq = -1;
		if (hcd->rsrc_start)
			dev_info(hcd->self.controller, "%s 0x%08llx\n",
					(hcd->driver->flags & HCD_MEMORY) ?
					"io mem" : "io base",
					(unsigned long long)hcd->rsrc_start);
	}

	/*启动主机*/
	if ((retval = hcd->driver->start(hcd)) < 0) {
		dev_err(hcd->self.controller, "startup error %d\n", retval);
		goto err_hcd_driver_start;
	}

	/*主机启动后，则配置root hub的信息*/ 
	rhdev->bus_mA = min(500u, hcd->power_budget);
	/*注册主机的root hub 设备*/
	if ((retval = register_root_hub(hcd)) != 0)
		goto err_register_root_hub;

	retval = sysfs_create_group(&rhdev->dev.kobj, &usb_bus_attr_group);
	if (retval < 0) {
		printk(KERN_ERR "Cannot register USB bus sysfs attributes: %d\n",
		       retval);
		goto error_create_attr_group;
	}
	/*root hub 处于轮训状态*/
	if (hcd->uses_new_polling && hcd->poll_rh)
		usb_hcd_poll_rh_status(hcd);
	return retval;

error_create_attr_group:
	mutex_lock(&usb_bus_list_lock);
	usb_disconnect(&hcd->self.root_hub);
	mutex_unlock(&usb_bus_list_lock);
err_register_root_hub:
	hcd->driver->stop(hcd);
err_hcd_driver_start:
	if (hcd->irq >= 0)
		free_irq(irqnum, hcd);
err_request_irq:
err_hcd_driver_setup:
	hcd->self.root_hub = NULL;
	usb_put_dev(rhdev);
err_allocate_root_hub:
	usb_deregister_bus(&hcd->self);
err_register_bus:
	hcd_buffer_destroy(hcd);
	return retval;
} 
EXPORT_SYMBOL_GPL(usb_add_hcd);

/**
 * usb_remove_hcd:usb总线删除hcd主机 
 */
void usb_remove_hcd(struct usb_hcd *hcd)
{
	dev_info(hcd->self.controller, "remove, state %x\n", hcd->state);

	if (HC_IS_RUNNING (hcd->state))
		hcd->state = HC_STATE_QUIESCING;

	dev_dbg(hcd->self.controller, "roothub graceful disconnect\n");
	spin_lock_irq (&hcd_root_hub_lock);
	hcd->rh_registered = 0;
	spin_unlock_irq (&hcd_root_hub_lock);

#ifdef CONFIG_PM
	cancel_work_sync(&hcd->wakeup_work);
#endif

	sysfs_remove_group(&hcd->self.root_hub->dev.kobj, &usb_bus_attr_group);
	mutex_lock(&usb_bus_list_lock);
	usb_disconnect(&hcd->self.root_hub);
	mutex_unlock(&usb_bus_list_lock);

	hcd->driver->stop(hcd);
	hcd->state = HC_STATE_HALT;

	hcd->poll_rh = 0;
	del_timer_sync(&hcd->rh_timer);

	if (hcd->irq >= 0)
		free_irq(hcd->irq, hcd);
	usb_deregister_bus(&hcd->self);
	hcd_buffer_destroy(hcd);
}
EXPORT_SYMBOL_GPL(usb_remove_hcd);

void
usb_hcd_platform_shutdown(struct platform_device* dev)
{
	struct usb_hcd *hcd = platform_get_drvdata(dev);

	if (hcd->driver->shutdown)
		hcd->driver->shutdown(hcd);
}
EXPORT_SYMBOL_GPL(usb_hcd_platform_shutdown);

/*-------------------------------------------------------------------------*/

#if defined(CONFIG_USB_MON) || defined(CONFIG_USB_MON_MODULE)

struct usb_mon_operations *mon_ops;

/*
 * The registration is unlocked.
 * We do it this way because we do not want to lock in hot paths.
 *
 * Notice that the code is minimally error-proof. Because usbmon needs
 * symbols from usbcore, usbcore gets referenced and cannot be unloaded first.
 */
 
int usb_mon_register (struct usb_mon_operations *ops)
{

	if (mon_ops)
		return -EBUSY;

	mon_ops = ops;
	mb();
	return 0;
}
EXPORT_SYMBOL_GPL (usb_mon_register);

void usb_mon_deregister (void)
{

	if (mon_ops == NULL) {
		printk(KERN_ERR "USB: monitor was not registered\n");
		return;
	}
	mon_ops = NULL;
	mb();
}
EXPORT_SYMBOL_GPL (usb_mon_deregister);

#endif /* CONFIG_USB_MON || CONFIG_USB_MON_MODULE */
