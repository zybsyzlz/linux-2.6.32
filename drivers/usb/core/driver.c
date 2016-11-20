/*
 * drivers/usb/driver.c - most of the driver model stuff for usb
 *
 * (C) Copyright 2005 Greg Kroah-Hartman <gregkh@suse.de>
 *
 * based on drivers/usb/usb.c which had the following copyrights:
 *	(C) Copyright Linus Torvalds 1999
 *	(C) Copyright Johannes Erdfelt 1999-2001
 *	(C) Copyright Andreas Gal 1999
 *	(C) Copyright Gregory P. Smith 1999
 *	(C) Copyright Deti Fliegl 1999 (new USB architecture)
 *	(C) Copyright Randy Dunlap 2000
 *	(C) Copyright David Brownell 2000-2004
 *	(C) Copyright Yggdrasil Computing, Inc. 2000
 *		(usb_device_id matching changes by Adam J. Richter)
 *	(C) Copyright Greg Kroah-Hartman 2002-2003
 *
 * NOTE! This is not actually a driver at all, rather this is
 * just a collection of helper routines that implement the
 * matching, probing, releasing, suspending and resuming for
 * real drivers.
 * 本文件的功能：
 *             1. usb设备与接口的注册和注销接口实现
 *             2. usb总线结构描述
 *             3. usb设备与接口的电源管理中的唤醒和挂起接口实现
 */

#include <linux/device.h>
#include <linux/usb.h>
#include <linux/usb/quirks.h>
#include <linux/workqueue.h>
#include "hcd.h"
#include "usb.h"


#ifdef CONFIG_HOTPLUG

/**
 * usb_store_new_id:改变USB设备的ID号
 * @ dynids:
 * @ driver:
 * @ buf: 新的动态id缓存
 * @ count:buf的长度
 */
ssize_t usb_store_new_id(struct usb_dynids *dynids,
			 struct device_driver *driver,
			 const char *buf, size_t count)
{
	struct usb_dynid *dynid;
	u32 idVendor = 0;
	u32 idProduct = 0;
	int fields = 0;
	int retval = 0;

	fields = sscanf(buf, "%x %x", &idVendor, &idProduct);
	if (fields < 2)
		return -EINVAL;

	dynid = kzalloc(sizeof(*dynid), GFP_KERNEL);
	if (!dynid)
		return -ENOMEM;

	INIT_LIST_HEAD(&dynid->node);
	dynid->id.idVendor = idVendor;
	dynid->id.idProduct = idProduct;
	dynid->id.match_flags = USB_DEVICE_ID_MATCH_DEVICE;

	/*新的动态id保存在usb设备dynids链表中*/
	spin_lock(&dynids->lock);
	list_add_tail(&dynid->node, &dynids->list);
	spin_unlock(&dynids->lock);

	/*增加新的动态id后，所有总线上的设备重新绑定与探测*/
	if (get_driver(driver)) {
		retval = driver_attach(driver);
		put_driver(driver);
	}

	if (retval)
		return retval;
	return count;
}
EXPORT_SYMBOL_GPL(usb_store_new_id);

static ssize_t store_new_id(struct device_driver *driver,
			    const char *buf, size_t count)
{
	struct usb_driver *usb_drv = to_usb_driver(driver);

	return usb_store_new_id(&usb_drv->dynids, driver, buf, count);
}
static DRIVER_ATTR(new_id, S_IWUSR, NULL, store_new_id);

/**
 * usb_create_newid_file: 给interface创建id属性文件
*/
static int usb_create_newid_file(struct usb_driver *usb_drv)
{
	int error = 0;

	/*如果驱动没有动态id，则不进行创建*/
	if (usb_drv->no_dynamic_id)
		goto exit;

	if (usb_drv->probe != NULL)
		error = driver_create_file(&usb_drv->drvwrap.driver,
					   &driver_attr_new_id);
exit:
	return error;
}

/*usb_remove_newid_file:删除interface驱动的id属性文件*/
static void usb_remove_newid_file(struct usb_driver *usb_drv)
{
	if (usb_drv->no_dynamic_id)
		return;

	if (usb_drv->probe != NULL)
		driver_remove_file(&usb_drv->drvwrap.driver,
				   &driver_attr_new_id);
}

static void usb_free_dynids(struct usb_driver *usb_drv)
{
	struct usb_dynid *dynid, *n;

	spin_lock(&usb_drv->dynids.lock);
	list_for_each_entry_safe(dynid, n, &usb_drv->dynids.list, node) {
		list_del(&dynid->node);
		kfree(dynid);
	}
	spin_unlock(&usb_drv->dynids.lock);
}
#else
static inline int usb_create_newid_file(struct usb_driver *usb_drv)
{
	return 0;
}

static void usb_remove_newid_file(struct usb_driver *usb_drv)
{
}

static inline void usb_free_dynids(struct usb_driver *usb_drv)
{
}
#endif
/**
 * usb_match_dynamic_id :查看是否与动态设置的动态id表中的匹配，
*/
static const struct usb_device_id *usb_match_dynamic_id(struct usb_interface *intf,
							struct usb_driver *drv)
{
	struct usb_dynid *dynid;

	spin_lock(&drv->dynids.lock);
	list_for_each_entry(dynid, &drv->dynids.list, node) {
		if (usb_match_one_id(intf, &dynid->id)) {
			spin_unlock(&drv->dynids.lock);
			return &dynid->id;
		}
	}
	spin_unlock(&drv->dynids.lock);
	return NULL;
}


/**
 * usb_probe_device: 设备探测接口函数
 */
static int usb_probe_device(struct device *dev)
{
	struct usb_device_driver *udriver = to_usb_device_driver(dev->driver);
	struct usb_device *udev = to_usb_device(dev);
	int error = -ENODEV;

	dev_dbg(dev, "%s\n", __func__);

	/* TODO: Add real matching code */

	/* The device should always appear to be in use
	 * unless the driver suports autosuspend.
	 */
	udev->pm_usage_cnt = !(udriver->supports_autosuspend);

	error = udriver->probe(udev);
	return error;
}

/**
 * usb_unbind_device:usb设备取消驱动的绑定
 */
static int usb_unbind_device(struct device *dev)
{
	struct usb_device_driver *udriver = to_usb_device_driver(dev->driver);

	udriver->disconnect(to_usb_device(dev));
	return 0;
}

/*
 * Cancel any pending scheduled resets
 *
 * [see usb_queue_reset_device()]
 *
 * Called after unconfiguring / when releasing interfaces. See
 * comments in __usb_queue_reset_device() regarding
 * udev->reset_running.
 */
static void usb_cancel_queued_reset(struct usb_interface *iface)
{
	if (iface->reset_running == 0)
		cancel_work_sync(&iface->reset_ws);
}

/**  
 * usb_probe_interface:接口探测函数
*/
static int usb_probe_interface(struct device *dev)
{
	struct usb_driver *driver = to_usb_driver(dev->driver);
	struct usb_interface *intf = to_usb_interface(dev);
	struct usb_device *udev = interface_to_usbdev(intf);
	const struct usb_device_id *id;
	int error = -ENODEV;

	dev_dbg(dev, "%s\n", __func__);

	intf->needs_binding = 0;

	if (usb_device_is_owned(udev))
		return -ENODEV;

	if (udev->authorized == 0) {
		dev_err(&intf->dev, "Device is not authorized for usage\n");
		return -ENODEV;
	}

	id = usb_match_id(intf, driver->id_table);
	if (!id)
		id = usb_match_dynamic_id(intf, driver);
	if (id) {
		dev_dbg(dev, "%s - got id\n", __func__);

		error = usb_autoresume_device(udev);
		if (error)
			return error;

		/* Interface "power state" doesn't correspond to any hardware
		 * state whatsoever.  We use it to record when it's bound to
		 * a driver that may start I/0:  it's not frozen/quiesced.
		 */
		mark_active(intf);
		intf->condition = USB_INTERFACE_BINDING;

		/* The interface should always appear to be in use
		 * unless the driver suports autosuspend.
		 */
		atomic_set(&intf->pm_usage_cnt, !driver->supports_autosuspend);

		/* Carry out a deferred switch to altsetting 0 */
		if (intf->needs_altsetting0) {
			error = usb_set_interface(udev, intf->altsetting[0].
					desc.bInterfaceNumber, 0);
			if (error < 0)
				goto err;

			intf->needs_altsetting0 = 0;
		}

		/*usb dev driver probe*/
		error = driver->probe(intf, id);
		if (error)
			goto err;

		intf->condition = USB_INTERFACE_BOUND;
		usb_autosuspend_device(udev);
	}

	return error;

err:
	mark_quiesced(intf);
	intf->needs_remote_wakeup = 0;
	intf->condition = USB_INTERFACE_UNBOUND;
	usb_cancel_queued_reset(intf);
	usb_autosuspend_device(udev);
	return error;
}

/**
 * usb_unbind_interface: interface取消驱动的绑定
*/
static int usb_unbind_interface(struct device *dev)
{
	struct usb_driver *driver = to_usb_driver(dev->driver);
	struct usb_interface *intf = to_usb_interface(dev);
	struct usb_device *udev;
	int error, r;

	/*取消驱动绑定进行中*/
	intf->condition = USB_INTERFACE_UNBINDING;

	/* Autoresume for set_interface call below */
	udev = interface_to_usbdev(intf);
	error = usb_autoresume_device(udev);

	/* Terminate all URBs for this interface unless the driver
	 * supports "soft" unbinding.
	 */
	if (!driver->soft_unbind)
		usb_disable_interface(udev, intf, false);

	driver->disconnect(intf);
	usb_cancel_queued_reset(intf);

	/* Reset other interface state.
	 * We cannot do a Set-Interface if the device is suspended or
	 * if it is prepared for a system sleep (since installing a new
	 * altsetting means creating new endpoint device entries).
	 * When either of these happens, defer the Set-Interface.
	 */
	if (intf->cur_altsetting->desc.bAlternateSetting == 0) {
		/* Already in altsetting 0 so skip Set-Interface.
		 * Just re-enable it without affecting the endpoint toggles.
		 */
		usb_enable_interface(udev, intf, false);
	} else if (!error && intf->dev.power.status == DPM_ON) {
		/*重新指定altsetting[0]为当前的altsetting*/
		r = usb_set_interface(udev, intf->altsetting[0].
				desc.bInterfaceNumber, 0);
		if (r < 0)
			intf->needs_altsetting0 = 1;
	} else {
		intf->needs_altsetting0 = 1; /*需要重新设置altsetting0*/
	}
	usb_set_intfdata(intf, NULL);

	/*驱动取消绑定完成*/
	intf->condition = USB_INTERFACE_UNBOUND;
	mark_quiesced(intf);
	intf->needs_remote_wakeup = 0;

	if (!error)
		usb_autosuspend_device(udev);

	return 0;
}

/**
 * usb_driver_claim_interface:给iface直接绑定驱动
 */
int usb_driver_claim_interface(struct usb_driver *driver,
				struct usb_interface *iface, void *priv)
{
	struct device *dev = &iface->dev;
	struct usb_device *udev = interface_to_usbdev(iface);
	int retval = 0;

	if (dev->driver)
		return -EBUSY;

	/*直接进行驱动绑定，省去了驱动与设备的match*/
	dev->driver = &driver->drvwrap.driver;
	usb_set_intfdata(iface, priv);
	iface->needs_binding = 0;

	usb_pm_lock(udev);
	iface->condition = USB_INTERFACE_BOUND;
	mark_active(iface);
	atomic_set(&iface->pm_usage_cnt, !driver->supports_autosuspend);
	usb_pm_unlock(udev);

	/* if interface was already added, bind now; else let
	 * the future device_add() bind it, bypassing probe()
	 */
	if (device_is_registered(dev))
		retval = device_bind_driver(dev);

	return retval;
}
EXPORT_SYMBOL_GPL(usb_driver_claim_interface);

/**
 * usb_driver_release_interface:释放iface接口绑定的driver驱动
 */
void usb_driver_release_interface(struct usb_driver *driver,
					struct usb_interface *iface)
{
	struct device *dev = &iface->dev;

	/* this should never happen, don't release something that's not ours */
	if (!dev->driver || dev->driver != &driver->drvwrap.driver)
		return;

	/* don't release from within disconnect() */
	if (iface->condition != USB_INTERFACE_BOUND)
		return;
	iface->condition = USB_INTERFACE_UNBINDING;

	/* Release via the driver core only if the interface
	 * has already been registered
	 */
	if (device_is_registered(dev)) {
		device_release_driver(dev);
	} else {
		down(&dev->sem);
		usb_unbind_interface(dev);
		dev->driver = NULL;
		up(&dev->sem);
	}
}
EXPORT_SYMBOL_GPL(usb_driver_release_interface);

/**
 * usb_match_device:根据id的match_flags设置属性值，匹配usb设备的信息
 * @ dev: usb设备结构
 * @ id: 
*/
int usb_match_device(struct usb_device *dev, const struct usb_device_id *id)
{
	if ((id->match_flags & USB_DEVICE_ID_MATCH_VENDOR) &&
	    id->idVendor != le16_to_cpu(dev->descriptor.idVendor))
		return 0;

	if ((id->match_flags & USB_DEVICE_ID_MATCH_PRODUCT) &&
	    id->idProduct != le16_to_cpu(dev->descriptor.idProduct))
		return 0;

	/* No need to test id->bcdDevice_lo != 0, since 0 is never
	   greater than any unsigned number. */
	if ((id->match_flags & USB_DEVICE_ID_MATCH_DEV_LO) &&
	    (id->bcdDevice_lo > le16_to_cpu(dev->descriptor.bcdDevice)))
		return 0;

	if ((id->match_flags & USB_DEVICE_ID_MATCH_DEV_HI) &&
	    (id->bcdDevice_hi < le16_to_cpu(dev->descriptor.bcdDevice)))
		return 0;

	if ((id->match_flags & USB_DEVICE_ID_MATCH_DEV_CLASS) &&
	    (id->bDeviceClass != dev->descriptor.bDeviceClass))
		return 0;

	if ((id->match_flags & USB_DEVICE_ID_MATCH_DEV_SUBCLASS) &&
	    (id->bDeviceSubClass != dev->descriptor.bDeviceSubClass))
		return 0;

	if ((id->match_flags & USB_DEVICE_ID_MATCH_DEV_PROTOCOL) &&
	    (id->bDeviceProtocol != dev->descriptor.bDeviceProtocol))
		return 0;

	return 1;
}

/** 
 * usb_match_one_id: interface 是否与id匹配
*/
int usb_match_one_id(struct usb_interface *interface,
		     const struct usb_device_id *id)
{
	struct usb_host_interface *intf;
	struct usb_device *dev;

	/* proc_connectinfo in devio.c may call us with id == NULL. */
	if (id == NULL)
		return 0;

	intf = interface->cur_altsetting;
	dev = interface_to_usbdev(interface);

	if (!usb_match_device(dev, id))
		return 0;

	/* The interface class, subclass, and protocol should never be
	 * checked for a match if the device class is Vendor Specific,
	 * unless the match record specifies the Vendor ID. */
	if (dev->descriptor.bDeviceClass == USB_CLASS_VENDOR_SPEC &&
			!(id->match_flags & USB_DEVICE_ID_MATCH_VENDOR) &&
			(id->match_flags & (USB_DEVICE_ID_MATCH_INT_CLASS |
				USB_DEVICE_ID_MATCH_INT_SUBCLASS |
				USB_DEVICE_ID_MATCH_INT_PROTOCOL)))
		return 0;

	/*查看interface的属性是否与id匹配*/
	if ((id->match_flags & USB_DEVICE_ID_MATCH_INT_CLASS) &&
	    (id->bInterfaceClass != intf->desc.bInterfaceClass))
		return 0;

	if ((id->match_flags & USB_DEVICE_ID_MATCH_INT_SUBCLASS) &&
	    (id->bInterfaceSubClass != intf->desc.bInterfaceSubClass))
		return 0;

	if ((id->match_flags & USB_DEVICE_ID_MATCH_INT_PROTOCOL) &&
	    (id->bInterfaceProtocol != intf->desc.bInterfaceProtocol))
		return 0;

	return 1;
}
EXPORT_SYMBOL_GPL(usb_match_one_id);

/**
 * usb_match_id:usb接口与id的信息匹配接口
 */
const struct usb_device_id *usb_match_id(struct usb_interface *interface,
					 const struct usb_device_id *id)
{
	if (id == NULL)
		return NULL;

	for (; id->idVendor || id->idProduct || id->bDeviceClass ||
	       id->bInterfaceClass || id->driver_info; id++) {
		if (usb_match_one_id(interface, id))
			return id;
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(usb_match_id);

/**
 * usb_device_match:usb设备(包括总线)与驱动的匹配接口
*/
static int usb_device_match(struct device *dev, struct device_driver *drv)
{
	/* devices and interfaces are handled separately */
	if (is_usb_device(dev)) {

		/* interface drivers never match devices */
		if (!is_usb_device_driver(drv))
			return 0;

		/* TODO: Add real matching code 
		 * 如果dev属于一个usb设备,则直接表示匹配*/
		return 1;

	} else if (is_usb_interface(dev)) {
		struct usb_interface *intf;
		struct usb_driver *usb_drv;
		const struct usb_device_id *id;

		/* device drivers never match interfaces */
		if (is_usb_device_driver(drv))
			return 0;

		intf = to_usb_interface(dev);
		usb_drv = to_usb_driver(drv);

		id = usb_match_id(intf, usb_drv->id_table);
		if (id)
			return 1;

		id = usb_match_dynamic_id(intf, usb_drv);
		if (id)
			return 1;
	}

	return 0;
}

#ifdef	CONFIG_HOTPLUG
/**
 * usb_uevent:usb总线支持热插拔事件的接口
 */
static int usb_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	struct usb_device *usb_dev;

	/* driver is often null here; dev_dbg() would oops */
	pr_debug("usb %s: uevent\n", dev_name(dev));

	if (is_usb_device(dev)) {
		usb_dev = to_usb_device(dev);
	} else if (is_usb_interface(dev)) {
		struct usb_interface *intf = to_usb_interface(dev);

		usb_dev = interface_to_usbdev(intf);
	} else {
		return 0;
	}

	if (usb_dev->devnum < 0) {
		pr_debug("usb %s: already deleted?\n", dev_name(dev));
		return -ENODEV;
	}
	if (!usb_dev->bus) {
		pr_debug("usb %s: bus removed?\n", dev_name(dev));
		return -ENODEV;
	}

#ifdef	CONFIG_USB_DEVICEFS
	/* If this is available, userspace programs can directly read
	 * all the device descriptors we don't tell them about.  Or
	 * act as usermode drivers.
	 */
	if (add_uevent_var(env, "DEVICE=/proc/bus/usb/%03d/%03d",
			   usb_dev->bus->busnum, usb_dev->devnum))
		return -ENOMEM;
#endif

	/* per-device configurations are common */
	if (add_uevent_var(env, "PRODUCT=%x/%x/%x",
			   le16_to_cpu(usb_dev->descriptor.idVendor),
			   le16_to_cpu(usb_dev->descriptor.idProduct),
			   le16_to_cpu(usb_dev->descriptor.bcdDevice)))
		return -ENOMEM;

	/* class-based driver binding models */
	if (add_uevent_var(env, "TYPE=%d/%d/%d",
			   usb_dev->descriptor.bDeviceClass,
			   usb_dev->descriptor.bDeviceSubClass,
			   usb_dev->descriptor.bDeviceProtocol))
		return -ENOMEM;
	return 0;
}

#else

static int usb_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	return -ENODEV;
}
#endif	/* CONFIG_HOTPLUG */

/**
 * usb_register_device_driver :注册usb设备驱动
 */
int usb_register_device_driver(struct usb_device_driver *new_udriver,
		struct module *owner)
{
	int retval = 0;

	if (usb_disabled())
		return -ENODEV;

	new_udriver->drvwrap.for_devices = 1; /*表示为设备*/
	new_udriver->drvwrap.driver.name = (char *) new_udriver->name;
	new_udriver->drvwrap.driver.bus = &usb_bus_type;
	new_udriver->drvwrap.driver.probe = usb_probe_device;
	new_udriver->drvwrap.driver.remove = usb_unbind_device;
	new_udriver->drvwrap.driver.owner = owner;

	retval = driver_register(&new_udriver->drvwrap.driver);

	if (!retval) {
		pr_info("%s: registered new device driver %s\n",
			usbcore_name, new_udriver->name);
		usbfs_update_special();
	} else {
		printk(KERN_ERR "%s: error %d registering device "
			"	driver %s\n",
			usbcore_name, retval, new_udriver->name);
	}

	return retval;
}
EXPORT_SYMBOL_GPL(usb_register_device_driver);

/**
 * usb_deregister_device_driver:注销usb的设备的驱动(与接口驱动是不一样的)
 */
void usb_deregister_device_driver(struct usb_device_driver *udriver)
{
	pr_info("%s: deregistering device driver %s\n",
			usbcore_name, udriver->name);

	driver_unregister(&udriver->drvwrap.driver);
	usbfs_update_special();
}
EXPORT_SYMBOL_GPL(usb_deregister_device_driver);

/**
 * usb_register_driver: 注册USB接口设备驱动.
 * @ new_driver: interface驱动
 * @ owner:
 * @ mod_name:驱动模块的名字
 */
int usb_register_driver(struct usb_driver *new_driver, struct module *owner,
			const char *mod_name)
{
	int retval = 0;

	if (usb_disabled())
		return -ENODEV;

	new_driver->drvwrap.for_devices = 0; /*注册一个接口驱动*/
	new_driver->drvwrap.driver.name = (char *) new_driver->name;
	new_driver->drvwrap.driver.bus = &usb_bus_type;
	new_driver->drvwrap.driver.probe = usb_probe_interface;
	new_driver->drvwrap.driver.remove = usb_unbind_interface;
	new_driver->drvwrap.driver.owner = owner;
	new_driver->drvwrap.driver.mod_name = mod_name;
	spin_lock_init(&new_driver->dynids.lock);
	INIT_LIST_HEAD(&new_driver->dynids.list);

	retval = driver_register(&new_driver->drvwrap.driver);

	if (!retval) {
		pr_info("%s: registered new interface driver %s\n",
			usbcore_name, new_driver->name);
		usbfs_update_special();
		usb_create_newid_file(new_driver);
	} else {
		printk(KERN_ERR "%s: error %d registering interface "
			"	driver %s\n",
			usbcore_name, retval, new_driver->name);
	}

	return retval;
}
EXPORT_SYMBOL_GPL(usb_register_driver);

/**
 * usb_deregister:注销interface的驱动
 */
void usb_deregister(struct usb_driver *driver)
{
	pr_info("%s: deregistering interface driver %s\n",
			usbcore_name, driver->name);

	usb_remove_newid_file(driver);
	usb_free_dynids(driver);
	driver_unregister(&driver->drvwrap.driver);

	usbfs_update_special();
}
EXPORT_SYMBOL_GPL(usb_deregister);

/**
 * usb_forced_unbind_intf:强制取消inf的驱动绑定
 */
void usb_forced_unbind_intf(struct usb_interface *intf)
{
	struct usb_driver *driver = to_usb_driver(intf->dev.driver);

	dev_dbg(&intf->dev, "forced unbind\n");
	usb_driver_release_interface(driver, intf);

	/* Mark the interface for later rebinding */
	intf->needs_binding = 1;
}
/**
 * usb_rebind_intf:重新为interface绑定驱动
 */
void usb_rebind_intf(struct usb_interface *intf)
{
	int rc;

	/* Delayed unbind of an existing driver
	 * 如果interface存在driver,则首先要释放掉它的driver,
	 * 然后在进行重新绑定
	*/
	if (intf->dev.driver) {
		struct usb_driver *driver =
				to_usb_driver(intf->dev.driver);

		dev_dbg(&intf->dev, "forced unbind\n");
		usb_driver_release_interface(driver, intf);
	}

	/* Try to rebind the interface 
	 * 进行驱动的绑定
	*/
	if (intf->dev.power.status == DPM_ON) {
		intf->needs_binding = 0;
		rc = device_attach(&intf->dev);
		if (rc < 0)
			dev_warn(&intf->dev, "rebind failed: %d\n", rc);
	}
}

#ifdef CONFIG_PM

#define DO_UNBIND	0
#define DO_REBIND	1

/**
 * do_unbind_rebind:通过action的值取消或者重新绑定interface的驱动
 */
static void do_unbind_rebind(struct usb_device *udev, int action)
{
	struct usb_host_config	*config;
	int			i;
	struct usb_interface	*intf;
	struct usb_driver	*drv;

	config = udev->actconfig;
	if (!config)
		return ;

	for (i = 0; i < config->desc.bNumInterfaces; ++i) {
		intf = config->interface[i];
		switch (action) {
		case DO_UNBIND:
			if (intf->dev.driver) {
				drv = to_usb_driver(intf->dev.driver);
				/*如果该接口不支持唤醒和挂起电源事件
				 * 则取消该接口的驱动绑定*/
				if (!drv->suspend || !drv->resume)
					usb_forced_unbind_intf(intf);
			}
			break;
		case DO_REBIND:
			if (intf->needs_binding)
				usb_rebind_intf(intf);
			break;
		}
	}
}

/** 
 * usb_suspend_device:挂起usb设备
 * @ udev:
 * @ msg:挂起的事件信息
*/
static int usb_suspend_device(struct usb_device *udev, pm_message_t msg)
{
	struct usb_device_driver	*udriver;
	int				status = 0;

	if (udev->state == USB_STATE_NOTATTACHED ||
			udev->state == USB_STATE_SUSPENDED)
		goto done;

	/* For devices that don't have a driver, we do a generic suspend. */
	if (udev->dev.driver)
		udriver = to_usb_device_driver(udev->dev.driver);
	else {
		udev->do_remote_wakeup = 0;
		/*generic_driver作为设备驱动*/
		udriver = &usb_generic_driver; 
	}
	if (udriver->suspend)
		status = udriver->suspend(udev, msg);
	else 
		status = -EINVAL;

 done:
	dev_vdbg(&udev->dev, "%s: status %d\n", __func__, status);
	return status;
}

/**
 * usb_resume_device: 唤醒usb设备
 * @dev : usb设备结构
 * @msg: 唤醒时的操作事件
*/
static int usb_resume_device(struct usb_device *udev, pm_message_t msg)
{
	struct usb_device_driver	*udriver;
	int				status = 0;

	if (udev->state == USB_STATE_NOTATTACHED)
		goto done;

	/* Can't resume it if it doesn't have a driver. */
	if (udev->dev.driver == NULL) {
		status = -ENOTCONN;
		goto done;
	}

	/*如果唤醒之前需要复位，则设置usb设备的唤醒复位标记*/
	if (udev->quirks & USB_QUIRK_RESET_RESUME)
		udev->reset_resume = 1;

	udriver = to_usb_device_driver(udev->dev.driver);
	status = udriver->resume(udev, msg);

 done:
	dev_vdbg(&udev->dev, "%s: status %d\n", __func__, status);
	if (status == 0)
		udev->autoresume_disabled = 0; /*设备唤醒成功，则自动唤醒的标志设置为0，不需要自动唤醒了*/
	return status;
}

/**
 * usb_suspend_interface : 挂起usb设备接口
 * @ udev: usb设备
 * @ intf: usb interface
 * @ msg:  电源管理事件
*/
static int usb_suspend_interface(struct usb_device *udev,
		struct usb_interface *intf, pm_message_t msg)
{
	struct usb_driver	*driver;
	int			status = 0;

	/* with no hardware, USB interfaces only use FREEZE and ON states */
	if (udev->state == USB_STATE_NOTATTACHED || !is_active(intf))
		goto done;

	/* This can happen; see usb_driver_release_interface() */
	if (intf->condition == USB_INTERFACE_UNBOUND)
		goto done;
	driver = to_usb_driver(intf->dev.driver);

	if (driver->suspend) {
		status = driver->suspend(intf, msg);
		if (status == 0)
			mark_quiesced(intf);
		else if (!(msg.event & PM_EVENT_AUTO))
			dev_err(&intf->dev, "%s error %d\n",
					"suspend", status);
	} else {
		/* Later we will unbind the driver and reprobe */
		intf->needs_binding = 1;
		dev_warn(&intf->dev, "no %s for driver %s?\n",
				"suspend", driver->name);
		mark_quiesced(intf);
	}

 done:
	dev_vdbg(&intf->dev, "%s: status %d\n", __func__, status);
	return status;
}

/**
 * usb_resume_interface :唤醒usb设备接口
 * @ udev: usb设备接口
 * @ intf: usb接口
 * @ msg:  电源管理事件
 * @ reset_resume: 唤醒是否复位标记
*/
static int usb_resume_interface(struct usb_device *udev,
		struct usb_interface *intf, pm_message_t msg, int reset_resume)
{
	struct usb_driver	*driver;
	int			status = 0;

	if (udev->state == USB_STATE_NOTATTACHED || is_active(intf))
		goto done;

	/* Don't let autoresume interfere with unbinding */
	if (intf->condition == USB_INTERFACE_UNBINDING)
		goto done;

	/* Can't resume it if it doesn't have a driver. 
	 * 如果接口处于未绑定状态，则需要设置该接口的信息*/
	if (intf->condition == USB_INTERFACE_UNBOUND) {

		/* Carry out a deferred switch to altsetting 0 */
		if (intf->needs_altsetting0 &&
				intf->dev.power.status == DPM_ON) {
			usb_set_interface(udev, intf->altsetting[0].
					desc.bInterfaceNumber, 0);
			intf->needs_altsetting0 = 0;
		}
		goto done;
	}

	/* Don't resume if the interface is marked for rebinding
	 * 如果设备没有resume接口，则需要needs_binding*/
	if (intf->needs_binding)
		goto done;
	driver = to_usb_driver(intf->dev.driver);

	if (reset_resume) {
		if (driver->reset_resume) {
			status = driver->reset_resume(intf);
			if (status)
				dev_err(&intf->dev, "%s error %d\n",
						"reset_resume", status);
		} else {
			intf->needs_binding = 1;
			dev_warn(&intf->dev, "no %s for driver %s?\n",
					"reset_resume", driver->name);
		}
	} else {
		if (driver->resume) {
			status = driver->resume(intf);
			if (status)
				dev_err(&intf->dev, "%s error %d\n",
						"resume", status);
		} else {
			/*不存在resume接口，需要重新绑定驱动*/
			intf->needs_binding = 1;
			dev_warn(&intf->dev, "no %s for driver %s?\n",
					"resume", driver->name);
		}
	}

done:
	dev_vdbg(&intf->dev, "%s: status %d\n", __func__, status);
	if (status == 0 && intf->condition == USB_INTERFACE_BOUND)
		mark_active(intf);

	/* Later we will unbind the driver and/or reprobe, if necessary */
	return status;
}

#ifdef	CONFIG_USB_SUSPEND

/**
 * autosuspend_check: 是否可以自动挂起检查
 * @ udev:
 * @ reschedule:
 * @ return: 自动挂起返回0, 否则返回非零值
 */
static int autosuspend_check(struct usb_device *udev, int reschedule)
{
	int			i;
	struct usb_interface	*intf;
	unsigned long		suspend_time, j;

	/* For autosuspend, fail fast if anything is in use or autosuspend
	 * is disabled.  Also fail if any interfaces require remote wakeup
	 * but it isn't available.
	 */
	if (udev->pm_usage_cnt > 0)
		return -EBUSY;
	if (udev->autosuspend_delay < 0 || udev->autosuspend_disabled)
		return -EPERM;

	suspend_time = udev->last_busy + udev->autosuspend_delay;
	if (udev->actconfig) {
		for (i = 0; i < udev->actconfig->desc.bNumInterfaces; i++) {
			intf = udev->actconfig->interface[i];
			if (!is_active(intf))
				continue;
			if (atomic_read(&intf->pm_usage_cnt) > 0)
				return -EBUSY;
			if (intf->needs_remote_wakeup &&
					!udev->do_remote_wakeup) {
				dev_dbg(&udev->dev, "remote wakeup needed "
						"for autosuspend\n");
				return -EOPNOTSUPP;
			}

			/* Don't allow autosuspend if the device will need
			 * a reset-resume and any of its interface drivers
			 * doesn't include support.
			 */
			if (udev->quirks & USB_QUIRK_RESET_RESUME) {
				struct usb_driver *driver;

				driver = to_usb_driver(intf->dev.driver);
				if (!driver->reset_resume ||
				    intf->needs_remote_wakeup)
					return -EOPNOTSUPP;
			}
		}
	}

	/* If everything is okay but the device hasn't been idle for long
	 * enough, queue a delayed autosuspend request.  If the device
	 * _has_ been idle for long enough and the reschedule flag is set,
	 * likewise queue a delayed (1 second) autosuspend request.
	 */
	j = jiffies;
	/*如果设定的延迟时间没有超过，则需要调度延迟工作队列*/
	if (time_before(j, suspend_time))
		reschedule = 1;
	else
		suspend_time = j + HZ;
	if (reschedule) {
		if (!timer_pending(&udev->autosuspend.timer)) {
			queue_delayed_work(ksuspend_usb_wq, &udev->autosuspend,
				round_jiffies_up_relative(suspend_time - j));
		}
		return -EAGAIN;
	}
	return 0;
}

#else

static inline int autosuspend_check(struct usb_device *udev, int reschedule)
{
	return 0;
}

#endif	/* CONFIG_USB_SUSPEND */

/**
 * usb_suspend_both: 挂起usb设备和它的interface
 */
static int usb_suspend_both(struct usb_device *udev, pm_message_t msg)
{
	int			status = 0;
	int			i = 0;
	struct usb_interface	*intf;
	struct usb_device	*parent = udev->parent;

	if (udev->state == USB_STATE_NOTATTACHED ||
			udev->state == USB_STATE_SUSPENDED)
		goto done;

	udev->do_remote_wakeup = device_may_wakeup(&udev->dev);

	if (msg.event & PM_EVENT_AUTO) {
		status = autosuspend_check(udev, 0);
		if (status < 0)
			goto done;
	}

	/*挂起该udev的所有接口*/
	if (udev->actconfig) {
		for (; i < udev->actconfig->desc.bNumInterfaces; i++) {
			intf = udev->actconfig->interface[i];
			/*挂起usb的interface*/
			status = usb_suspend_interface(udev, intf, msg);
			if (status != 0)
				break;
		}
	}
	/*挂起udev本身*/
	if (status == 0)
		status = usb_suspend_device(udev, msg);

	/* If the suspend failed, resume interfaces that did get suspended */
	if (status != 0) {
		pm_message_t msg2;

		msg2.event = msg.event ^ (PM_EVENT_SUSPEND | PM_EVENT_RESUME);
		while (--i >= 0) {
			intf = udev->actconfig->interface[i];
			usb_resume_interface(udev, intf, msg2, 0);
		}

		/* Try another autosuspend when the interfaces aren't busy */
		if (msg.event & PM_EVENT_AUTO)
			autosuspend_check(udev, status == -EBUSY);

	/* If the suspend succeeded then prevent any more URB submissions,
	 * flush any outstanding URBs, and propagate the suspend up the tree.
	 */
	} else {
		cancel_delayed_work(&udev->autosuspend);
		udev->can_submit = 0;
		for (i = 0; i < 16; ++i) {
			usb_hcd_flush_endpoint(udev, udev->ep_out[i]);
			usb_hcd_flush_endpoint(udev, udev->ep_in[i]);
		}

		/* If this is just a FREEZE or a PRETHAW, udev might
		 * not really be suspended.  Only true suspends get
		 * propagated up the device tree.
		 */
		/*如果udev存在父亲，则挂起父亲dev*/
		if (parent && udev->state == USB_STATE_SUSPENDED)
			usb_autosuspend_device(parent);
	}

 done:
	dev_vdbg(&udev->dev, "%s: status %d\n", __func__, status);
	return status;
}

/**
 *usb_resume_both : 自动唤醒usb设备和它的接口
 *@ udev:usb设备接口
 *@ msg: 唤醒操作的动作：比如自动唤醒／唤醒等
*/
static int usb_resume_both(struct usb_device *udev, pm_message_t msg)
{
	int			status = 0;
	int			i;
	struct usb_interface	*intf;
	struct usb_device	*parent = udev->parent;

	/*取消usb 设备的自动挂起工作队列*/
	cancel_delayed_work(&udev->autosuspend);
	if (udev->state == USB_STATE_NOTATTACHED) {
		status = -ENODEV;
		goto done;
	}
	udev->can_submit = 1;  /*urb队列可以submit*/

	 /*唤醒整棵设备树*/
	if (udev->state == USB_STATE_SUSPENDED) {
		/*如果需要自动唤醒，但是设备的自动唤醒标记为disable，则错误返回*/
		if ((msg.event & PM_EVENT_AUTO) &&
				udev->autoresume_disabled) {
			status = -EPERM;
			goto done;
		}
		/*递归唤醒整棵usb树*/
		if (parent) {
			status = usb_autoresume_device(parent);
			if (status == 0) {
				/*唤醒udev自己*/
				status = usb_resume_device(udev, msg);
				if (status || udev->state ==
						USB_STATE_NOTATTACHED) {
					usb_autosuspend_device(parent);

					/*设备处于未链接状态,
					 * discon_suspended:表示未链接且挂起标记*/
					if (udev->state ==
							USB_STATE_NOTATTACHED)
						udev->discon_suspended = 1;
				}
			}
		} else {
			/*没有parent 表明是一个root hub*/
			status = usb_resume_device(udev, msg); 
		}
	} else if (udev->reset_resume)
		status = usb_resume_device(udev, msg);

	/*唤醒设备的所有接口*/
	if (status == 0 && udev->actconfig) {
		for (i = 0; i < udev->actconfig->desc.bNumInterfaces; i++) {
			intf = udev->actconfig->interface[i];
			usb_resume_interface(udev, intf, msg,
					udev->reset_resume);
		}
	}

 done:
	dev_vdbg(&udev->dev, "%s: status %d\n", __func__, status);
	if (!status)
		udev->reset_resume = 0;
	return status;
}

#ifdef CONFIG_USB_SUSPEND
/**
 * usb_autopm_do_device: 唤醒或挂起udev设备与接口树
 * @ inc_usage_cnt: <0:挂起设备，>0 唤醒设备
 */
static int usb_autopm_do_device(struct usb_device *udev, int inc_usage_cnt)
{
	int	status = 0;

	usb_pm_lock(udev);
	udev->auto_pm = 1;
	udev->pm_usage_cnt += inc_usage_cnt;
	WARN_ON(udev->pm_usage_cnt < 0);
	if (inc_usage_cnt)
		udev->last_busy = jiffies;
	if (inc_usage_cnt >= 0 && udev->pm_usage_cnt > 0) {
		if (udev->state == USB_STATE_SUSPENDED)
			status = usb_resume_both(udev, PMSG_AUTO_RESUME);
		if (status != 0)
			udev->pm_usage_cnt -= inc_usage_cnt;
		else if (inc_usage_cnt)
			udev->last_busy = jiffies;
	} else if (inc_usage_cnt <= 0 && udev->pm_usage_cnt <= 0) {
		/*如果pm_usage_cnt 小于0,则挂起该设备*/
		status = usb_suspend_both(udev, PMSG_AUTO_SUSPEND);
	}
	usb_pm_unlock(udev);
	return status;
}

/**
 * usb_autosuspend_work:自动挂起工作队列回调函数
 */
void usb_autosuspend_work(struct work_struct *work)
{
	struct usb_device *udev =
		container_of(work, struct usb_device, autosuspend.work);

	usb_autopm_do_device(udev, 0);
}

/**
 * usb_autoresume_work:自动唤醒工作队列回调函数
 */
void usb_autoresume_work(struct work_struct *work)
{
	struct usb_device *udev =
		container_of(work, struct usb_device, autoresume);
	if (usb_autopm_do_device(udev, 1) == 0)
		usb_autopm_do_device(udev, -1);
}

/**
 * usb_autosuspend_device:自动挂起usb设备
 */
void usb_autosuspend_device(struct usb_device *udev)
{
	int	status;

	status = usb_autopm_do_device(udev, -1);
	dev_vdbg(&udev->dev, "%s: cnt %d\n",
			__func__, udev->pm_usage_cnt);
}

/**
 * usb_try_autosuspend_device:尝试挂起usb设备和它的接口
 */
void usb_try_autosuspend_device(struct usb_device *udev)
{
	usb_autopm_do_device(udev, 0);
	dev_vdbg(&udev->dev, "%s: cnt %d\n",
			__func__, udev->pm_usage_cnt);
}

/**
 * usb_autoresume_device:立刻自动唤醒usb设备和它的interfaces
 */
int usb_autoresume_device(struct usb_device *udev)
{
	int	status;

	status = usb_autopm_do_device(udev, 1);
	dev_vdbg(&udev->dev, "%s: status %d cnt %d\n",
			__func__, status, udev->pm_usage_cnt);
	return status;
}

/**
 * usb_autopm_do_interface:usb的interface电源管理核心函数
 * @ intf:usb的interface指针
 * @ inc_usage_cnt: <0:挂起接口,>0唤醒接口
*/
static int usb_autopm_do_interface(struct usb_interface *intf,
		int inc_usage_cnt)
{
	/*获得接口所属的usb设备*/
	struct usb_device	*udev = interface_to_usbdev(intf);
	int			status = 0;

	usb_pm_lock(udev);
	if (intf->condition == USB_INTERFACE_UNBOUND) {
		status = -ENODEV;
		goto out;
	}

	udev->auto_pm = 1;
	atomic_add(inc_usage_cnt, &intf->pm_usage_cnt);
	udev->last_busy = jiffies;
	if (inc_usage_cnt >= 0 &&
			atomic_read(&intf->pm_usage_cnt) > 0) {
		if (udev->state == USB_STATE_SUSPENDED)
			status = usb_resume_both(udev,
					PMSG_AUTO_RESUME);
		if (status != 0)
			atomic_sub(inc_usage_cnt, &intf->pm_usage_cnt);
		else
			udev->last_busy = jiffies;
	} else if (inc_usage_cnt <= 0 &&
			atomic_read(&intf->pm_usage_cnt) <= 0) {
		status = usb_suspend_both(udev, PMSG_AUTO_SUSPEND);
	}
out:
	usb_pm_unlock(udev);
	return status;
}

/**
 * usb_autopm_put_interface:立刻自动挂起接口
 * @intf: 接口指针
 */
void usb_autopm_put_interface(struct usb_interface *intf)
{
	int	status;

	status = usb_autopm_do_interface(intf, -1);
	dev_vdbg(&intf->dev, "%s: status %d cnt %d\n",
			__func__, status, atomic_read(&intf->pm_usage_cnt));
}
EXPORT_SYMBOL_GPL(usb_autopm_put_interface);

/**
 * usb_autopm_put_interface_async:异步挂起接口 
 */
void usb_autopm_put_interface_async(struct usb_interface *intf)
{
	struct usb_device	*udev = interface_to_usbdev(intf);
	int			status = 0;

	if (intf->condition == USB_INTERFACE_UNBOUND) {
		status = -ENODEV;
		goto out;
	}

	udev->last_busy = jiffies;
	atomic_dec(&intf->pm_usage_cnt);
	if (udev->autosuspend_disabled || udev->autosuspend_delay < 0)
		status = -EPERM;
	else if (atomic_read(&intf->pm_usage_cnt) <= 0 &&
			!timer_pending(&udev->autosuspend.timer)) {
		/*udev->autosuspend_delay时间后挂起接口*/
		queue_delayed_work(ksuspend_usb_wq, &udev->autosuspend,
				round_jiffies_up_relative(
					udev->autosuspend_delay));
	}
out:
	dev_vdbg(&intf->dev, "%s: status %d cnt %d\n",
			__func__, status, atomic_read(&intf->pm_usage_cnt));
}
EXPORT_SYMBOL_GPL(usb_autopm_put_interface_async);

/**
 * usb_autopm_get_interface:立即自动唤醒接口
 * @intf:待唤醒的usb接口
 */
int usb_autopm_get_interface(struct usb_interface *intf)
{
	int	status;

	status = usb_autopm_do_interface(intf, 1);
	dev_vdbg(&intf->dev, "%s: status %d cnt %d\n",
			__func__, status, atomic_read(&intf->pm_usage_cnt));
	return status;
}
EXPORT_SYMBOL_GPL(usb_autopm_get_interface);

/**
 * usb_autopm_get_interface_async:异步自动唤醒接口
 */
int usb_autopm_get_interface_async(struct usb_interface *intf)
{
	struct usb_device	*udev = interface_to_usbdev(intf);
	int			status = 0;

	if (intf->condition == USB_INTERFACE_UNBOUND)
		status = -ENODEV;
	else if (udev->autoresume_disabled)
		status = -EPERM;
	else {
		atomic_inc(&intf->pm_usage_cnt);
		if (atomic_read(&intf->pm_usage_cnt) > 0 &&
				udev->state == USB_STATE_SUSPENDED)
			queue_work(ksuspend_usb_wq, &udev->autoresume);
	}
	dev_vdbg(&intf->dev, "%s: status %d cnt %d\n",
			__func__, status, atomic_read(&intf->pm_usage_cnt));
	return status;
}
EXPORT_SYMBOL_GPL(usb_autopm_get_interface_async);

/**
 * usb_autopm_set_interface:自动挂起或者唤醒接口
 * @intf:带挂起或者唤醒的接口 
 */
int usb_autopm_set_interface(struct usb_interface *intf)
{
	int	status;

	status = usb_autopm_do_interface(intf, 0);
	dev_vdbg(&intf->dev, "%s: status %d cnt %d\n",
			__func__, status, atomic_read(&intf->pm_usage_cnt));
	return status;
}
EXPORT_SYMBOL_GPL(usb_autopm_set_interface);

#else

void usb_autosuspend_work(struct work_struct *work)
{

}

void usb_autoresume_work(struct work_struct *work)
{

}

#endif /* CONFIG_USB_SUSPEND */

/**
 * usb_external_suspend_device:自动挂起由电源管理事件引起的usb设备与它的接口
 * @udev:待挂起的usb设备 
 * @msg:电源管理的事件状态值 
 */
int usb_external_suspend_device(struct usb_device *udev, pm_message_t msg)
{
	int	status;

	do_unbind_rebind(udev, DO_UNBIND);
	usb_pm_lock(udev);
	udev->auto_pm = 0;
	status = usb_suspend_both(udev, msg);
	usb_pm_unlock(udev);
	return status;
}

/**
 * usb_external_suspend_device:自动唤醒由电源管理事件引起的usb设备与它的接口
 * @udev:待唤醒的usb设备 
 * @msg:电源管理的事件状态值 
 */
int usb_external_resume_device(struct usb_device *udev, pm_message_t msg)
{
	int	status;

	usb_pm_lock(udev);
	udev->auto_pm = 0;
	status = usb_resume_both(udev, msg);  /*递归调用唤醒这课usb设备树*/
	udev->last_busy = jiffies;
	usb_pm_unlock(udev);
	if (status == 0)
		do_unbind_rebind(udev, DO_REBIND);

	/* Now that the device is awake, we can start trying to autosuspend
	 * it again. */
	if (status == 0)
		usb_try_autosuspend_device(udev);
	return status;
}

/**
 * usb_suspend:挂起usb设备
 * @dev:待挂起的usb设备
 * @msg:电源管理状态值
*/
int usb_suspend(struct device *dev, pm_message_t msg)
{
	struct usb_device	*udev;

	udev = to_usb_device(dev);

	if (udev->state == USB_STATE_SUSPENDED) {
		if (udev->parent || udev->speed != USB_SPEED_HIGH)
			udev->skip_sys_resume = 1;
		return 0;
	}

	udev->skip_sys_resume = 0;
	return usb_external_suspend_device(udev, msg);
}

/**
 * usb_resume:唤醒usb设备
 * @dev:待唤醒的usb设备
 * @msg:电源管理状态值
*/
int usb_resume(struct device *dev, pm_message_t msg)
{
	struct usb_device	*udev;
	int			status;

	udev = to_usb_device(dev);

	/*skip_sys_resume:
	 * 1:表示usb设备已经为挂起状态，而且忽略
	 *   以后的设备唤醒的操作
	 * 0:不忽略设备的唤醒事件
    */
	if (udev->skip_sys_resume)
		return 0;
	status = usb_external_resume_device(udev, msg);

	if (status == -ENODEV)
		return 0;
	return status;
}

#endif /* CONFIG_PM */

/*usb总线类型结构*/
struct bus_type usb_bus_type = {
	.name =		"usb",
	.match =	usb_device_match,
	.uevent =	usb_uevent,
};
