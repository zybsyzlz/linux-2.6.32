/*
 * OHCI HCD (Host Controller Driver) for USB.
 *
 * (C) Copyright 1999 Roman Weissgaerber <weissg@vienna.at>
 * (C) Copyright 2000-2002 David Brownell <dbrownell@users.sourceforge.net>
 * (C) Copyright 2002 Hewlett-Packard Company
 *
 * USB Bus Glue for Samsung S3C2410
 *
 * Written by Christopher Hoover <ch@hpl.hp.com>
 * Based on fragments of previous driver by Russell King et al.
 *
 * Modified for S3C2410 from ohci-sa1111.c, ohci-omap.c and ohci-lh7a40.c
 *	by Ben Dooks, <ben@simtec.co.uk>
 *	Copyright (C) 2004 Simtec Electronics
 *
 * Thanks to basprog@mail.ru for updates to newer kernels
 *
 * This file is licenced under the GPL.
*/

#include <linux/platform_device.h>
#include <linux/clk.h>
#include <plat/usb-control.h>

#define valid_port(idx) ((idx) == 1 || (idx) == 2)

/* clock device associated with the hcd */

static struct clk *clk;
static struct clk *usb_clk;

static void s3c2410_start_hc(struct platform_device *dev, struct usb_hcd *hcd)
{
	dev_dbg(&dev->dev, "s3c2410_start_hc:\n");
	clk_enable(usb_clk);
	mdelay(2);			/* let the bus clock stabilise */
	clk_enable(clk);
}

static void s3c2410_stop_hc(struct platform_device *dev)
{
	dev_dbg(&dev->dev, "s3c2410_stop_hc:\n");
	clk_disable(clk);
	clk_disable(usb_clk);
}

static void
usb_hcd_s3c2410_remove (struct usb_hcd *hcd, struct platform_device *dev)
{
	usb_remove_hcd(hcd);
	s3c2410_stop_hc(dev);
	iounmap(hcd->regs);
	release_mem_region(hcd->rsrc_start, hcd->rsrc_len);
	usb_put_hcd(hcd);
}

/**
 * usb_hcd_s3c2410_probe - initialize S3C2410-based HCDs
 * Context: !in_interrupt()
 */
static int usb_hcd_s3c2410_probe (const struct hc_driver *driver,
				  struct platform_device *dev)
{
	struct usb_hcd *hcd = NULL;
	int retval, irq_num;
	struct resource *res;

	dev_info(&dev->dev, "%s\n", __func__);
	hcd = usb_create_hcd(driver, &dev->dev, "s3c24xx");
	if (hcd == NULL)
		return -ENOMEM;
	res = platform_get_resource(dev, IORESOURCE_MEM, 0);
	if (res == NULL) {
		retval = -ENOENT;
		goto err_put;
	}
	hcd->rsrc_start = res->start;
	hcd->rsrc_len   = resource_size(res);

	if (!request_mem_region(hcd->rsrc_start, hcd->rsrc_len, hcd_name)) {
		dev_err(&dev->dev, "request_mem_region failed\n");
		retval = -EBUSY;
		goto err_put;
	}

	clk = clk_get(&dev->dev, "usb-host");
	if (IS_ERR(clk)) {
		dev_err(&dev->dev, "cannot get usb-host clock\n");
		retval = -ENOENT;
		goto err_mem;
	}

	usb_clk = clk_get(&dev->dev, "usb-bus-host");
	if (IS_ERR(usb_clk)) {
		dev_err(&dev->dev, "cannot get usb-bus-host clock\n");
		retval = -ENOENT;
		goto err_clk;
	}

	s3c2410_start_hc(dev, hcd);

	hcd->regs = ioremap(hcd->rsrc_start, hcd->rsrc_len);
	if (!hcd->regs) {
		dev_err(&dev->dev, "ioremap failed\n");
		retval = -ENOMEM;
		goto err_ioremap;
	}

	ohci_hcd_init(hcd_to_ohci(hcd));
  
	retval = irq_num = platform_get_irq(dev, 0);
	if (irq_num < 0)
		goto err_ioremap;
	retval = usb_add_hcd(hcd, irq_num, IRQF_DISABLED);
	if (retval != 0)
		goto err_ioremap;

	return 0;

 err_ioremap:
	s3c2410_stop_hc(dev);
	iounmap(hcd->regs);
	clk_put(usb_clk);

 err_clk:
	clk_put(clk);

 err_mem:
	release_mem_region(hcd->rsrc_start, hcd->rsrc_len);

 err_put:
	usb_put_hcd(hcd);
	return retval;
}

static int
ohci_s3c2410_start (struct usb_hcd *hcd)
{
	struct ohci_hcd	*ohci = hcd_to_ohci (hcd);
	int ret;

	if ((ret = ohci_init(ohci)) < 0)
		return ret;

	if ((ret = ohci_run (ohci)) < 0) {
		err ("can't start %s", hcd->self.bus_name);
		ohci_stop (hcd);
		return ret;
	}

	return 0;
}


static const struct hc_driver ohci_s3c2410_hc_driver = {
	.description =		hcd_name,
	.product_desc =		"S3C24XX OHCI",
	.hcd_priv_size =	sizeof(struct ohci_hcd),

	/*
	 * generic hardware linkage
	 */
	.irq =			ohci_irq,
	.flags =		HCD_USB11 | HCD_MEMORY,

	/*
	 * basic lifecycle operations
	 */
	.start =		ohci_s3c2410_start,
	.stop =			ohci_stop,
	.shutdown =		ohci_shutdown,

	/*
	 * managing i/o requests and associated device resources
	 */
	.urb_enqueue =		ohci_urb_enqueue,
	.urb_dequeue =		ohci_urb_dequeue,
	.endpoint_disable =	ohci_endpoint_disable,

	/*
	 * scheduling support
	 */
	.get_frame_number =	ohci_get_frame,

	/*
	 * root hub support
	 */
	.hub_status_data = ohci_hub_status_data, 
	.hub_control =	ohci_hub_control,    
#ifdef	CONFIG_PM
	.bus_suspend =		ohci_bus_suspend,
	.bus_resume =		ohci_bus_resume,
#endif
	.start_port_reset =	ohci_start_port_reset,
};

/* device driver */

static int ohci_hcd_s3c2410_drv_probe(struct platform_device *pdev)
{
	return usb_hcd_s3c2410_probe(&ohci_s3c2410_hc_driver, pdev);
}

static int ohci_hcd_s3c2410_drv_remove(struct platform_device *pdev)
{
	struct usb_hcd *hcd = platform_get_drvdata(pdev);

	usb_hcd_s3c2410_remove(hcd, pdev);
	return 0;
}

static struct platform_driver ohci_hcd_s3c2410_driver = {
	.probe		= ohci_hcd_s3c2410_drv_probe,
	.remove		= ohci_hcd_s3c2410_drv_remove,
	.shutdown	= usb_hcd_platform_shutdown,
	/*.suspend	= ohci_hcd_s3c2410_drv_suspend, */
	/*.resume	= ohci_hcd_s3c2410_drv_resume, */
	.driver		= {
					.owner	= THIS_MODULE,
					.name	= "s3c2410-ohci",
	},
};
MODULE_ALIAS("platform:s3c2410-ohci");
