/*
 * 本文件的功能：获得并设置usb设备描述符结构的处理
 * 1. configuration:  信息处理
 * 2. interface:      信息处理
 * 3. endpoint:       信息处理
*/
#include <linux/usb.h>
#include <linux/usb/ch9.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <asm/byteorder.h>
#include "usb.h"
#include "hcd.h"

#define USB_MAXALTSETTING		128	/* Hard limit */
#define USB_MAXENDPOINTS		30	/* Hard limit */

#define USB_MAXCONFIG			8	/* Arbitrary limit */


static inline const char *plural(int n)
{
	return (n == 1 ? "" : "s");
}

/*
 * find_next_descriptor_more: 找到指定描述符类型的指针位置
 * @ num_skipped:记录跳过的描述符的个数
*/
static int find_next_descriptor_more(unsigned char *buffer, int size,
    int dt1, int dt2, int dt3, int *num_skipped)
{
	struct usb_descriptor_header *h;
	int n = 0;
	unsigned char *buffer0 = buffer;

	/* Find the next descriptor of type dt1 or dt2 or dt3 */
	while (size > 0) {
		h = (struct usb_descriptor_header *) buffer;
		if (h->bDescriptorType == dt1 || h->bDescriptorType == dt2 ||
				h->bDescriptorType == dt3)
			break;
		buffer += h->bLength;
		size -= h->bLength;
		++n;
	}

	/* Store the number of descriptors skipped and return the
	 * number of bytes skipped */
	if (num_skipped)
		*num_skipped = n;
	return buffer - buffer0;
}

/*
 * find_next_descriptor: 找到指定描述符类型的指针位置
 * @ num_skipped:记录跳过的描述符的个数
 * @ return:返回dt1 或者dt2类型描述符在buffer中的偏移量
*/
static int find_next_descriptor(unsigned char *buffer, int size,
    int dt1, int dt2, int *num_skipped)
{
	struct usb_descriptor_header *h;
	int n = 0;
	unsigned char *buffer0 = buffer;

	/* Find the next descriptor of type dt1 or dt2 */
	while (size > 0) {
		h = (struct usb_descriptor_header *) buffer;
		if (h->bDescriptorType == dt1 || h->bDescriptorType == dt2)
			break;
		buffer += h->bLength;
		size -= h->bLength;
		++n;
	}

	/* Store the number of descriptors skipped and return the
	 * number of bytes skipped */
	if (num_skipped)
		*num_skipped = n;
	/*返回描述符指针位置*/
	return buffer - buffer0;
}

static int usb_parse_ss_endpoint_companion(struct device *ddev, int cfgno,
		int inum, int asnum, struct usb_host_endpoint *ep,
		int num_ep, unsigned char *buffer, int size)
{
	unsigned char *buffer_start = buffer;
	struct usb_ss_ep_comp_descriptor	*desc;
	int retval;
	int num_skipped;
	int max_tx;
	int i;

	desc = (struct usb_ss_ep_comp_descriptor *) buffer;
	if (desc->bDescriptorType != USB_DT_SS_ENDPOINT_COMP) {
		dev_warn(ddev, "No SuperSpeed endpoint companion for config %d "
				" interface %d altsetting %d ep %d: "
				"using minimum values\n",
				cfgno, inum, asnum, ep->desc.bEndpointAddress);
		/*
		 * The next descriptor is for an Endpoint or Interface,
		 * no extra descriptors to copy into the companion structure,
		 * and we didn't eat up any of the buffer.
		 */
		return 0;
	}
	memcpy(&ep->ss_ep_comp->desc, desc, USB_DT_SS_EP_COMP_SIZE);
	desc = &ep->ss_ep_comp->desc;
	buffer += desc->bLength;
	size -= desc->bLength;

	/* Eat up the other descriptors we don't care about */
	ep->ss_ep_comp->extra = buffer;
	i = find_next_descriptor(buffer, size, USB_DT_ENDPOINT,
			USB_DT_INTERFACE, &num_skipped);
	ep->ss_ep_comp->extralen = i;
	buffer += i;
	size -= i;
	retval = buffer - buffer_start;
	if (num_skipped > 0)
		dev_dbg(ddev, "skipped %d descriptor%s after %s\n",
				num_skipped, plural(num_skipped),
				"SuperSpeed endpoint companion");

	/* Check the various values */
	if (usb_endpoint_xfer_control(&ep->desc) && desc->bMaxBurst != 0) {
		dev_warn(ddev, "Control endpoint with bMaxBurst = %d in "
				"config %d interface %d altsetting %d ep %d: "
				"setting to zero\n", desc->bMaxBurst,
				cfgno, inum, asnum, ep->desc.bEndpointAddress);
		desc->bMaxBurst = 0;
	}
	if (desc->bMaxBurst > 15) {
		dev_warn(ddev, "Endpoint with bMaxBurst = %d in "
				"config %d interface %d altsetting %d ep %d: "
				"setting to 15\n", desc->bMaxBurst,
				cfgno, inum, asnum, ep->desc.bEndpointAddress);
		desc->bMaxBurst = 15;
	}
	if ((usb_endpoint_xfer_control(&ep->desc) || usb_endpoint_xfer_int(&ep->desc))
			&& desc->bmAttributes != 0) {
		dev_warn(ddev, "%s endpoint with bmAttributes = %d in "
				"config %d interface %d altsetting %d ep %d: "
				"setting to zero\n",
				usb_endpoint_xfer_control(&ep->desc) ? "Control" : "Bulk",
				desc->bmAttributes,
				cfgno, inum, asnum, ep->desc.bEndpointAddress);
		desc->bmAttributes = 0;
	}
	if (usb_endpoint_xfer_bulk(&ep->desc) && desc->bmAttributes > 16) {
		dev_warn(ddev, "Bulk endpoint with more than 65536 streams in "
				"config %d interface %d altsetting %d ep %d: "
				"setting to max\n",
				cfgno, inum, asnum, ep->desc.bEndpointAddress);
		desc->bmAttributes = 16;
	}
	if (usb_endpoint_xfer_isoc(&ep->desc) && desc->bmAttributes > 2) {
		dev_warn(ddev, "Isoc endpoint has Mult of %d in "
				"config %d interface %d altsetting %d ep %d: "
				"setting to 3\n", desc->bmAttributes + 1,
				cfgno, inum, asnum, ep->desc.bEndpointAddress);
		desc->bmAttributes = 2;
	}
	if (usb_endpoint_xfer_isoc(&ep->desc)) {
		max_tx = ep->desc.wMaxPacketSize * (desc->bMaxBurst + 1) *
			(desc->bmAttributes + 1);
	} else if (usb_endpoint_xfer_int(&ep->desc)) {
		max_tx = ep->desc.wMaxPacketSize * (desc->bMaxBurst + 1);
	} else {
		goto valid;
	}
	if (desc->wBytesPerInterval > max_tx) {
		dev_warn(ddev, "%s endpoint with wBytesPerInterval of %d in "
				"config %d interface %d altsetting %d ep %d: "
				"setting to %d\n",
				usb_endpoint_xfer_isoc(&ep->desc) ? "Isoc" : "Int",
				desc->wBytesPerInterval,
				cfgno, inum, asnum, ep->desc.bEndpointAddress,
				max_tx);
		desc->wBytesPerInterval = max_tx;
	}
valid:
	return retval;
}

/**
 * usb_parse_endpoint:分析endpoint的信息
 * @ ddev:
 * @ cfgno: config number
 * @ inum:  interface number
 * @ asum:
 * @ ifp:保存分析到的endpoint信息
 * @ num_ep: endpoint number
 * @ buffer: endpoint描述符缓存
 * @ size:　 buffer size
*/
static int usb_parse_endpoint(struct device *ddev, int cfgno, int inum,
    int asnum, struct usb_host_interface *ifp, int num_ep,
    unsigned char *buffer, int size)
{
	unsigned char *buffer0 = buffer;
	struct usb_endpoint_descriptor *d;
	struct usb_host_endpoint *endpoint;
	int n, i, j, retval;

	d = (struct usb_endpoint_descriptor *) buffer;
	buffer += d->bLength;
	size -= d->bLength;

	/*判断endpoint描述符长度的有效性*/
	if (d->bLength >= USB_DT_ENDPOINT_AUDIO_SIZE)
		n = USB_DT_ENDPOINT_AUDIO_SIZE;
	else if (d->bLength >= USB_DT_ENDPOINT_SIZE)
		n = USB_DT_ENDPOINT_SIZE;
	else {
		dev_warn(ddev, "config %d interface %d altsetting %d has an "
		    "invalid endpoint descriptor of length %d, skipping\n",
		    cfgno, inum, asnum, d->bLength);
		goto skip_to_next_endpoint_or_interface_descriptor;
	}

	/*endpoint的 endpoint number*/
	i = d->bEndpointAddress & ~USB_ENDPOINT_DIR_MASK;
	if (i >= 16 || i == 0) {
		dev_warn(ddev, "config %d interface %d altsetting %d has an "
		    "invalid endpoint with address 0x%X, skipping\n",
		    cfgno, inum, asnum, d->bEndpointAddress);
		goto skip_to_next_endpoint_or_interface_descriptor;
	}

	/* Only store as many endpoints as we have room for */
	if (ifp->desc.bNumEndpoints >= num_ep)
		goto skip_to_next_endpoint_or_interface_descriptor;

	endpoint = &ifp->endpoint[ifp->desc.bNumEndpoints];
	++ifp->desc.bNumEndpoints;

	/*拷贝endpoint描述符信息到endpoint中*/
	memcpy(&endpoint->desc, d, n);
	INIT_LIST_HEAD(&endpoint->urb_list);

	/* Fix up bInterval values outside the legal range. Use 32 ms if no
	 * proper value can be guessed. */
	i = 0;		/* i = min, j = max, n = default */
	j = 255;
	if (usb_endpoint_xfer_int(d)) { /*中断传输的endpoint*/
		i = 1;
		/*计算中断传输时的帧间隔时间*/
		switch (to_usb_device(ddev)->speed) {
		case USB_SPEED_SUPER:
		case USB_SPEED_HIGH:
			/* Many device manufacturers are using full-speed
			 * bInterval values in high-speed interrupt endpoint
			 * descriptors. Try to fix those and fall back to a
			 * 32 ms default value otherwise. 
			 */
			n = fls(d->bInterval*8);
			if (n == 0)
				n = 9;	/* 32 ms = 2^(9-1) uframes */
			j = 16;
			break;
		default:		/* USB_SPEED_FULL or _LOW */
			/* For low-speed, 10 ms is the official minimum.
			 * But some "overclocked" devices might want faster
			 * polling so we'll allow it. */
			n = 32;
			break;
		}
	} else if (usb_endpoint_xfer_isoc(d)) {
		i = 1;
		j = 16;
		if (to_usb_device(ddev)->speed == USB_SPEED_HIGH)
			n = 9;  /* 32 ms = 2^(9-1) uframes */
		else 
			n = 6;  /* 32 ms = 2^(6-1) frames */
	}
	if (d->bInterval < i || d->bInterval > j) {
		dev_warn(ddev, "config %d interface %d altsetting %d "
		    "endpoint 0x%X has an invalid bInterval %d, "
		    "changing to %d\n",
		    cfgno, inum, asnum,
		    d->bEndpointAddress, d->bInterval, n);
		endpoint->desc.bInterval = n;
	}

	/* Some buggy low-speed devices have Bulk endpoints, which is
	 * explicitly forbidden by the USB spec.  In an attempt to make
	 * them usable, we will try treating them as Interrupt endpoints.
	 * 低速的usb批量传输endpoint，改变为interrupt endpoint
	 */
	if (to_usb_device(ddev)->speed == USB_SPEED_LOW &&
			usb_endpoint_xfer_bulk(d)) {
		dev_warn(ddev, "config %d interface %d altsetting %d "
		    "endpoint 0x%X is Bulk; changing to Interrupt\n",
		    cfgno, inum, asnum, d->bEndpointAddress);
		endpoint->desc.bmAttributes = USB_ENDPOINT_XFER_INT;
		endpoint->desc.bInterval = 1;
		if (le16_to_cpu(endpoint->desc.wMaxPacketSize) > 8)
			endpoint->desc.wMaxPacketSize = cpu_to_le16(8);
	}

	/*
	 * Some buggy high speed devices have bulk endpoints using
	 * maxpacket sizes other than 512.  High speed HCDs may not
	 * be able to handle that particular bug, so let's warn...
	 */
	if (to_usb_device(ddev)->speed == USB_SPEED_HIGH
			&& usb_endpoint_xfer_bulk(d)) {
		unsigned maxp;

		maxp = le16_to_cpu(endpoint->desc.wMaxPacketSize) & 0x07ff;
		if (maxp != 512)
			dev_warn(ddev, "config %d interface %d altsetting %d "
				"bulk endpoint 0x%X has invalid maxpacket %d\n",
				cfgno, inum, asnum, d->bEndpointAddress,
				maxp);
	}
	/* Allocate room for and parse any SS endpoint companion descriptors */
	if (to_usb_device(ddev)->speed == USB_SPEED_SUPER) {
		endpoint->extra = buffer;
		i = find_next_descriptor_more(buffer, size, USB_DT_SS_ENDPOINT_COMP,
				USB_DT_ENDPOINT, USB_DT_INTERFACE, &n);
		endpoint->extralen = i;
		buffer += i;
		size -= i;

		/* Allocate space for the SS endpoint companion descriptor */
		endpoint->ss_ep_comp = kzalloc(sizeof(struct usb_host_ss_ep_comp),
				GFP_KERNEL);
		if (!endpoint->ss_ep_comp)
			return -ENOMEM;

		/* Fill in some default values (may be overwritten later) */
		endpoint->ss_ep_comp->desc.bLength = USB_DT_SS_EP_COMP_SIZE;
		endpoint->ss_ep_comp->desc.bDescriptorType = USB_DT_SS_ENDPOINT_COMP;
		endpoint->ss_ep_comp->desc.bMaxBurst = 0;
		/*
		 * Leave bmAttributes as zero, which will mean no streams for
		 * bulk, and isoc won't support multiple bursts of packets.
		 * With bursts of only one packet, and a Mult of 1, the max
		 * amount of data moved per endpoint service interval is one
		 * packet.
		 */
		if (usb_endpoint_xfer_isoc(&endpoint->desc) ||
				usb_endpoint_xfer_int(&endpoint->desc))
			endpoint->ss_ep_comp->desc.wBytesPerInterval =
				endpoint->desc.wMaxPacketSize;

		if (size > 0) {
			retval = usb_parse_ss_endpoint_companion(ddev, cfgno,
					inum, asnum, endpoint, num_ep, buffer,
					size);
			if (retval >= 0) {
				buffer += retval;
				retval = buffer - buffer0;
			}
		} else {
			dev_warn(ddev, "config %d interface %d altsetting %d "
				"endpoint 0x%X has no "
				"SuperSpeed companion descriptor\n",
				cfgno, inum, asnum, d->bEndpointAddress);
			retval = buffer - buffer0;
		}
	} else {
		/* Skip over any Class Specific or Vendor Specific descriptors;
		 * find the next endpoint or interface descriptor */
		endpoint->extra = buffer;
		i = find_next_descriptor(buffer, size, USB_DT_ENDPOINT,
				USB_DT_INTERFACE, &n);
		endpoint->extralen = i;
		retval = buffer - buffer0 + i;
	}
	if (n > 0)
		dev_dbg(ddev, "skipped %d descriptor%s after %s\n",
		    n, plural(n), "endpoint");
	return retval;

skip_to_next_endpoint_or_interface_descriptor:
	i = find_next_descriptor(buffer, size, USB_DT_ENDPOINT,
	    USB_DT_INTERFACE, NULL);
	return buffer - buffer0 + i;
}

void usb_release_interface_cache(struct kref *ref)
{
	struct usb_interface_cache *intfc = ref_to_usb_interface_cache(ref);
	int j;

	for (j = 0; j < intfc->num_altsetting; j++) {
		struct usb_host_interface *alt = &intfc->altsetting[j];

		kfree(alt->endpoint);
		kfree(alt->string);
	}
	kfree(intfc);
}
/**
 * usb_parse_interface: 分析usb设备的interface信息
 * @ cfgno: configuration number
 * @ config:configuration infomation
 * @ buffer: 包含interface 信息的指针
 * @ size: buffer length
 * @ inums[]: interface number 缓存
 * @ nalts[]: interface altsettings缓存 
 */
static int usb_parse_interface(struct device *ddev, int cfgno,
    struct usb_host_config *config, unsigned char *buffer, int size,
    u8 inums[], u8 nalts[])
{
	unsigned char *buffer0 = buffer;
	struct usb_interface_descriptor	*d;
	int inum, asnum;
	struct usb_interface_cache *intfc;
	struct usb_host_interface *alt;
	int i, n;
	int len, retval;
	int num_ep, num_ep_orig;

	d = (struct usb_interface_descriptor *) buffer;
	buffer += d->bLength;
	size -= d->bLength;

	if (d->bLength < USB_DT_INTERFACE_SIZE)
		goto skip_to_next_interface_descriptor;

	/*找interface编号所对应的intf_cache缓存结构*/
	intfc = NULL;
	inum = d->bInterfaceNumber;
	for (i = 0; i < config->desc.bNumInterfaces; ++i) {
		if (inums[i] == inum) {
			intfc = config->intf_cache[i];
			break;
		}
	}
	if (!intfc || intfc->num_altsetting >= nalts[i])
		goto skip_to_next_interface_descriptor;

	/*检查是否有重复的alts*/
	asnum = d->bAlternateSetting;
	for ((i = 0, alt = &intfc->altsetting[0]);
	      i < intfc->num_altsetting;
	     (++i, ++alt)) {
		if (alt->desc.bAlternateSetting == asnum) {
			dev_warn(ddev, "Duplicate descriptor for config %d "
			    "interface %d altsetting %d, skipping\n",
			    cfgno, inum, asnum);
			goto skip_to_next_interface_descriptor;
		}
	}

	++intfc->num_altsetting;
	memcpy(&alt->desc, d, USB_DT_INTERFACE_SIZE);

	/*获得alt中第一个endpoint的位置*/
	alt->extra = buffer;
	i = find_next_descriptor(buffer, size, USB_DT_ENDPOINT,
	    USB_DT_INTERFACE, &n);
	alt->extralen = i;
	if (n > 0)
		dev_dbg(ddev, "skipped %d descriptor%s after %s\n",
		    n, plural(n), "interface");
	buffer += i;
	size -= i;

	num_ep = num_ep_orig = alt->desc.bNumEndpoints;
	alt->desc.bNumEndpoints = 0;		/* Use as a counter */
	if (num_ep > USB_MAXENDPOINTS) {
		dev_warn(ddev, "too many endpoints for config %d interface %d "
		    "altsetting %d: %d, using maximum allowed: %d\n",
		    cfgno, inum, asnum, num_ep, USB_MAXENDPOINTS);
		num_ep = USB_MAXENDPOINTS;
	}

	/*给alt的接口分配endpoint的内存空间*/
	if (num_ep > 0) {
		/* Can't allocate 0 bytes */
		len = sizeof(struct usb_host_endpoint) * num_ep;
		alt->endpoint = kzalloc(len, GFP_KERNEL);
		if (!alt->endpoint)
			return -ENOMEM;
	}

	/*分析alt 接口中所有的endpoints的描述符信息*/
	n = 0;
	while (size > 0) {
		if (((struct usb_descriptor_header *) buffer)->bDescriptorType
		     == USB_DT_INTERFACE)
			break;
		retval = usb_parse_endpoint(ddev, cfgno, inum, asnum, alt,
		    num_ep, buffer, size);
		if (retval < 0)
			return retval;
		++n;

		buffer += retval;
		size -= retval;
	}

	if (n != num_ep_orig)
		dev_warn(ddev, "config %d interface %d altsetting %d has %d "
		    "endpoint descriptor%s, different from the interface "
		    "descriptor's value: %d\n",
		    cfgno, inum, asnum, n, plural(n), num_ep_orig);
	return buffer - buffer0;

skip_to_next_interface_descriptor:
	i = find_next_descriptor(buffer, size, USB_DT_INTERFACE,
	    USB_DT_INTERFACE, NULL);
	return buffer - buffer0 + i;
}
/*
 * usb_parse_configuration: 分析usb设备的configuration信息
 * @ cfgidx: configuration的编号
 * @ buffer: 包含configuration信息指针
 * @ size : configuration信息长度
 * @ config：保存分析得到的configuration信息
*/
static int usb_parse_configuration(struct device *ddev, int cfgidx,
    struct usb_host_config *config, unsigned char *buffer, int size)
{
	unsigned char *buffer0 = buffer;
	int cfgno;
	int nintf, nintf_orig;
	int i, j, n;
	struct usb_interface_cache *intfc;
	unsigned char *buffer2;
	int size2;
	struct usb_descriptor_header *header;
	int len, retval;
	/*inums数组保存每一个config的interface的编号
	 *nalts数组保存每一个config的interface的alts的编号*/
	u8 inums[USB_MAXINTERFACES], nalts[USB_MAXINTERFACES];
	unsigned iad_num = 0;

	/*复制configuration描述符信息到config的desc*/
	memcpy(&config->desc, buffer, USB_DT_CONFIG_SIZE);
	if (config->desc.bDescriptorType != USB_DT_CONFIG ||
	    config->desc.bLength < USB_DT_CONFIG_SIZE) {
		dev_err(ddev, "invalid descriptor for config index %d: "
		    "type = 0x%X, length = %d\n", cfgidx,
		    config->desc.bDescriptorType, config->desc.bLength);
		return -EINVAL;
	}
	/*configuration number value*/
	cfgno = config->desc.bConfigurationValue;
    /*skip over configuration struct info and pointer to configuration
	 content*/
	buffer += config->desc.bLength;
	size -= config->desc.bLength;

	/*获得config包含的interface数量*/
	nintf = nintf_orig = config->desc.bNumInterfaces;
	if (nintf > USB_MAXINTERFACES) {
		dev_warn(ddev, "config %d has too many interfaces: %d, "
		    "using maximum allowed: %d\n",
		    cfgno, nintf, USB_MAXINTERFACES);
		nintf = USB_MAXINTERFACES;
	}

	 /*分析usb设备的interface的每一个interface的alts数量信息*/
	n = 0;
	for ((buffer2 = buffer, size2 = size);
	      size2 > 0;
	     (buffer2 += header->bLength, size2 -= header->bLength)) {

		if (size2 < sizeof(struct usb_descriptor_header)) {
			dev_warn(ddev, "config %d descriptor has %d excess "
			    "byte%s, ignoring\n",
			    cfgno, size2, plural(size2));
			break;
		}

		/*获得interface描述符的前两个字段的内容，前两个字段
		 *包含的了interface描述符的类型和该描述符的长度信息*/
		header = (struct usb_descriptor_header *) buffer2;
		if ((header->bLength > size2) || (header->bLength < 2)) {
			dev_warn(ddev, "config %d has an invalid descriptor "
			    "of length %d, skipping remainder of the config\n",
			    cfgno, header->bLength);
			break;
		}

		if (header->bDescriptorType == USB_DT_INTERFACE) {
			struct usb_interface_descriptor *d;
			int inum;

			d = (struct usb_interface_descriptor *) header;
			if (d->bLength < USB_DT_INTERFACE_SIZE) {
				dev_warn(ddev, "config %d has an invalid "
				    "interface descriptor of length %d, "
				    "skipping\n", cfgno, d->bLength);
				continue;
			}

			/*interface的编号*/
			inum = d->bInterfaceNumber;
			if (inum >= nintf_orig)
				dev_warn(ddev, "config %d has an invalid "
				    "interface number: %d but max is %d\n",
				    cfgno, inum, nintf_orig - 1);

			/* Have we already encountered this interface?
			 * Count its altsettings */
			for (i = 0; i < n; ++i) {
				if (inums[i] == inum)
					break;
			}
			/*i < n 表示找到已经缓存过的interface number vaule
			 * 同一个interface有不同alts interface信息，因此记录
			 * 同一个interface的alts的数量*/
			if (i < n) {
				if (nalts[i] < 255)
					++nalts[i]; /*interface settings info is changed*/
			} else if (n < USB_MAXINTERFACES) {
				inums[n] = inum;
				nalts[n] = 1;
				++n;
			}

		} else if (header->bDescriptorType ==
				USB_DT_INTERFACE_ASSOCIATION) {
			if (iad_num == USB_MAXIADS) {
				dev_warn(ddev, "found more Interface "
					       "Association Descriptors "
					       "than allocated for in "
					       "configuration %d\n", cfgno);
			} else {
				config->intf_assoc[iad_num] =
					(struct usb_interface_assoc_descriptor
					*)header;
				iad_num++;
			}

		} else if (header->bDescriptorType == USB_DT_DEVICE ||
			    header->bDescriptorType == USB_DT_CONFIG)
			dev_warn(ddev, "config %d contains an unexpected "
			    "descriptor of type 0x%X, skipping\n",
			    cfgno, header->bDescriptorType);

	}	/* for ((buffer2 = buffer, size2 = size); ...) */
	size = buffer2 - buffer;
	config->desc.wTotalLength = cpu_to_le16(buffer2 - buffer0);

	if (n != nintf)
		dev_warn(ddev, "config %d has %d interface%s, different from "
		    "the descriptor's value: %d\n",
		    cfgno, n, plural(n), nintf_orig);
	else if (n == 0)
		dev_warn(ddev, "config %d has no interfaces?\n", cfgno);

	config->desc.bNumInterfaces = nintf = n;

	/* Check for missing interface numbers */
	for (i = 0; i < nintf; ++i) {
		for (j = 0; j < nintf; ++j) {
			if (inums[j] == i)
				break;
		}
		if (j >= nintf)
			dev_warn(ddev, "config %d has no interface number "
			    "%d\n", cfgno, i);
	}

	/*给每一个interface与其alts的interface申请内存空间*/
	for (i = 0; i < nintf; ++i) {
		j = nalts[i]; /*j 表示该interface所具有的altsetting的个数*/
		if (j > USB_MAXALTSETTING) {
			dev_warn(ddev, "too many alternate settings for "
			    "config %d interface %d: %d, "
			    "using maximum allowed: %d\n",
			    cfgno, inums[i], j, USB_MAXALTSETTING);
			nalts[i] = j = USB_MAXALTSETTING;
		}

		/*interface缓存*/
		len = sizeof(*intfc) + sizeof(struct usb_host_interface) * j;
		/*intf_cache内存空间包括interface自身以及它的alts interface的空间*/
		config->intf_cache[i] = intfc = kzalloc(len, GFP_KERNEL);
		if (!intfc)
			return -ENOMEM;
		kref_init(&intfc->ref);
	}

	/* FIXME: parse the BOS descriptor */

	/* Skip over any Class Specific or Vendor Specific descriptors;
	 * find the first interface descriptor */
	config->extra = buffer;
	/*找到第一个interface的所在的位置*/
	i = find_next_descriptor(buffer, size, USB_DT_INTERFACE,
	    USB_DT_INTERFACE, &n);
	config->extralen = i;
	if (n > 0)
		dev_dbg(ddev, "skipped %d descriptor%s after %s\n",
		    n, plural(n), "configuration");
	buffer += i;
	size -= i;

	/* 分析全部interface/altsetting信息*/
	while (size > 0) {
		retval = usb_parse_interface(ddev, cfgno, config,
		    buffer, size, inums, nalts);
		if (retval < 0)
			return retval;

		buffer += retval;
		size -= retval;
	}

	/* Check for missing altsettings */
	for (i = 0; i < nintf; ++i) {
		intfc = config->intf_cache[i];
		for (j = 0; j < intfc->num_altsetting; ++j) {
			for (n = 0; n < intfc->num_altsetting; ++n) {
				if (intfc->altsetting[n].desc.
				    bAlternateSetting == j)
					break;
			}
			if (n >= intfc->num_altsetting)
				dev_warn(ddev, "config %d interface %d has no "
				    "altsetting %d\n", cfgno, inums[i], j);
		}
	}

	return 0;
}

/* hub-only!! ... and only exported for reset/reinit path.
 * otherwise used internally on disconnect/destroy path
 */
void usb_destroy_configuration(struct usb_device *dev)
{
	int c, i;

	if (!dev->config)
		return;

	if (dev->rawdescriptors) {
		for (i = 0; i < dev->descriptor.bNumConfigurations; i++)
			kfree(dev->rawdescriptors[i]);

		kfree(dev->rawdescriptors);
		dev->rawdescriptors = NULL;
	}

	for (c = 0; c < dev->descriptor.bNumConfigurations; c++) {
		struct usb_host_config *cf = &dev->config[c];

		kfree(cf->string);
		for (i = 0; i < cf->desc.bNumInterfaces; i++) {
			if (cf->intf_cache[i])
				kref_put(&cf->intf_cache[i]->ref,
					  usb_release_interface_cache);
		}
	}
	kfree(dev->config);
	dev->config = NULL;
}


/**
 * usb_get_configuration:获得usb设备的全部configuration信息,并
 *						保存在dev->config结构中
 */
int usb_get_configuration(struct usb_device *dev)
{
	struct device *ddev = &dev->dev;
	int ncfg = dev->descriptor.bNumConfigurations;
	int result = 0;
	unsigned int cfgno, length;
	unsigned char *buffer;
	unsigned char *bigbuffer;
	struct usb_config_descriptor *desc;

	cfgno = 0;
	if (dev->authorized == 0)	/* Not really an error */
		goto out_not_authorized;
	result = -ENOMEM;
	if (ncfg > USB_MAXCONFIG) {
		dev_warn(ddev, "too many configurations: %d, "
		    "using maximum allowed: %d\n", ncfg, USB_MAXCONFIG);
		dev->descriptor.bNumConfigurations = ncfg = USB_MAXCONFIG;
	}

	if (ncfg < 1) {
		dev_err(ddev, "no configurations\n");
		return -EINVAL;
	}

	/*为所有的configuration信息分配管理空间*/
	length = ncfg * sizeof(struct usb_host_config);
	dev->config = kzalloc(length, GFP_KERNEL);
	if (!dev->config)
		goto err2;

	/*为所有的configuration分配指针空间*/
	length = ncfg * sizeof(char *);
	dev->rawdescriptors = kzalloc(length, GFP_KERNEL);
	if (!dev->rawdescriptors)
		goto err2;

	buffer = kmalloc(USB_DT_CONFIG_SIZE, GFP_KERNEL);
	if (!buffer)
		goto err2;
	desc = (struct usb_config_descriptor *)buffer;

	result = 0;
	for (; cfgno < ncfg; cfgno++) {
		/*获得cfgno的usb设备configuration描述符信息*/
		result = usb_get_descriptor(dev, USB_DT_CONFIG, cfgno,
		    buffer, USB_DT_CONFIG_SIZE);
		if (result < 0) {
			dev_err(ddev, "unable to read config index %d "
			    "descriptor/%s: %d\n", cfgno, "start", result);
			dev_err(ddev, "chopping to %d config(s)\n", cfgno);
			dev->descriptor.bNumConfigurations = cfgno;
			break;
		} else if (result < 4) {
			dev_err(ddev, "config index %d descriptor too short "
			    "(expected %i, got %i)\n", cfgno,
			    USB_DT_CONFIG_SIZE, result);
			result = -EINVAL;
			goto err;
		}
		/*根据wTotalLength:获得整个描述信息的长度,包括(config, 
		 * interface, endpoint, class or vender)
		*/
		length = max((int) le16_to_cpu(desc->wTotalLength),
		    USB_DT_CONFIG_SIZE);

		bigbuffer = kmalloc(length, GFP_KERNEL);
		if (!bigbuffer) {
			result = -ENOMEM;
			goto err;
		}
		/*获得cfgno的全部configuration信息,包括config interface endpoint
		 * 等描述符信息*/
		result = usb_get_descriptor(dev, USB_DT_CONFIG, cfgno,
		    bigbuffer, length);
		if (result < 0) {
			dev_err(ddev, "unable to read config index %d "
			    "descriptor/%s\n", cfgno, "all");
			kfree(bigbuffer);
			goto err;
		}
		if (result < length) {
			dev_warn(ddev, "config index %d descriptor too short "
			    "(expected %i, got %i)\n", cfgno, length, result);
			length = result;
		}

		/*rawdescriptors[cfgno]指针指向cfgno的全部描述符空间*/
		dev->rawdescriptors[cfgno] = bigbuffer;

		/*分析cfgno配置编号的configuration信息, 结果存放在config[cfgno]中*/
		result = usb_parse_configuration(&dev->dev, cfgno,
		    &dev->config[cfgno], bigbuffer, length);
		if (result < 0) {
			++cfgno;
			goto err;
		}
	}
	result = 0;

err:
	kfree(buffer);
out_not_authorized:
	dev->descriptor.bNumConfigurations = cfgno;
err2:
	if (result == -ENOMEM)
		dev_err(ddev, "out of memory\n");
	return result;
}
