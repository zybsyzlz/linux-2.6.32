/*
 * All the USB notify logic
 *
 * (C) Copyright 2005 Greg Kroah-Hartman <gregkh@suse.de>
 *
 * notifier functions originally based on those in kernel/sys.c
 * but fixed up to not be so broken.
 * 本文件功能：
 *   1.自定义实现usb设备注册和总线注册的通知连回调接口
 */


#include <linux/kernel.h>
#include <linux/notifier.h>
#include <linux/usb.h>
#include <linux/mutex.h>
#include "usb.h"

/*所有的通知链事件都注册到usb_notifier_list上面*/
static BLOCKING_NOTIFIER_HEAD(usb_notifier_list);

/**
 * usb_register_notify - register a notifier callback whenever a usb change happens
 * @nb: pointer to the notifier block for the callback events.
 * @ 向usb_notifier_list通知链注册通知事件
 */
void usb_register_notify(struct notifier_block *nb)
{
	blocking_notifier_chain_register(&usb_notifier_list, nb);
}
EXPORT_SYMBOL_GPL(usb_register_notify);

/**
 * usb_unregister_notify - unregister a notifier callback
 * @nb: pointer to the notifier block for the callback events.
 * @ 向usb_notifier_list通知链删除通知事件
 */
void usb_unregister_notify(struct notifier_block *nb)
{
	blocking_notifier_chain_unregister(&usb_notifier_list, nb);
}
EXPORT_SYMBOL_GPL(usb_unregister_notify);

/*@ usb_notifier_add_device:usb注册设备通知链回调函数*/
void usb_notify_add_device(struct usb_device *udev)
{
	blocking_notifier_call_chain(&usb_notifier_list, USB_DEVICE_ADD, udev);
}

/*@ usb_notifier_remove_device:usb删除设备通知链回调函数*/
void usb_notify_remove_device(struct usb_device *udev)
{
	/* Protect against simultaneous usbfs open */
	mutex_lock(&usbfs_mutex);
	blocking_notifier_call_chain(&usb_notifier_list,
			USB_DEVICE_REMOVE, udev);
	mutex_unlock(&usbfs_mutex);
}

/*@ usb_notify_add_bus:usb总线注册通知连回调函数*/
void usb_notify_add_bus(struct usb_bus *ubus)
{
	blocking_notifier_call_chain(&usb_notifier_list, USB_BUS_ADD, ubus);
}

/*@ usb_notify_remove_bus:usb总线删除通知连回调函数*/
void usb_notify_remove_bus(struct usb_bus *ubus)
{
	blocking_notifier_call_chain(&usb_notifier_list, USB_BUS_REMOVE, ubus);
}
