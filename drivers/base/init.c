/*
 * Copyright (c) 2002-3 Patrick Mochel
 * Copyright (c) 2002-3 Open Source Development Labs
 *
 * This file is released under the GPLv2
 */

#include <linux/device.h>
#include <linux/init.h>
#include <linux/memory.h>

#include "base.h"

/**
 * driver_init:初始化驱动模型
 * !Called early from init/main.c.
 */
void __init driver_init(void)
{
	/*初始化devtmp文件系统*/
	devtmpfs_init();
	/*在/sys/目录下创建
	 * devices dev dev/char dev/block bus 
	 * classes firmware hypervisor 等目录*/
	devices_init();
	buses_init();
	classes_init();
	firmware_init();
	hypervisor_init();

	/* These are also core pieces, but must come after the
	 * core core pieces.
	 */
	platform_bus_init();
	system_bus_init();
	cpu_dev_init();
	memory_dev_init();
}
