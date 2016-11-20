/*
 * OHCI HCD (Host Controller Driver) for USB.
 *
 * (C) Copyright 1999 Roman Weissgaerber <weissg@vienna.at>
 * (C) Copyright 2000-2002 David Brownell <dbrownell@users.sourceforge.net>
 *
 * This file is licenced under the GPL.
 * 本文件的功能：
 *  1. ed队列的处理
 *  2. td队列的处理
 *  3. done队列的处理
 */

#include <linux/irq.h>
/*
 * urb_free_priv:释放主机的td队列和从主机上删除pending_list
 * @ hc: 主机
 * @ urb_priv:待释放的结构
 * */
static void urb_free_priv (struct ohci_hcd *hc, urb_priv_t *urb_priv)
{
	int	last = urb_priv->length - 1;  /*获得需要释放的td的个数*/

	if (last >= 0) {
		int	i;
		struct td *td;
		for (i = 0; i <= last; i++) {
			td = urb_priv->td [i];
			if (td)
				td_free (hc, td);
		}
	}

	list_del (&urb_priv->pending);
	kfree (urb_priv);
}

/*
 * finish_urb :终止urb的传输并释放传输urb建立的内存空间
 * @ ohci: 主机结构
 * @ urb : 待终止的urb
 * @ status:状态码
 */
static void
finish_urb(struct ohci_hcd *ohci, struct urb *urb, int status)
__releases(ohci->lock)
__acquires(ohci->lock)
{
	// ASSERT (urb->hcpriv != 0);

	urb_free_priv (ohci, urb->hcpriv);
	if (likely(status == -EINPROGRESS))
		status = 0;

	switch (usb_pipetype (urb->pipe)) {
	case PIPE_ISOCHRONOUS:
		ohci_to_hcd(ohci)->self.bandwidth_isoc_reqs--;  /*减少等时传输的服务请求*/
		if (ohci_to_hcd(ohci)->self.bandwidth_isoc_reqs == 0) {
			if (quirk_amdiso(ohci))
				quirk_amd_pll(1);
			if (quirk_amdprefetch(ohci))
				sb800_prefetch(ohci, 0);
		}
		break;
	case PIPE_INTERRUPT:
		ohci_to_hcd(ohci)->self.bandwidth_int_reqs--; /*减少中断类型的服务请求*/
		break;
	}

#ifdef OHCI_VERBOSE_DEBUG
	urb_print(urb, "RET", usb_pipeout (urb->pipe), status);
#endif

	usb_hcd_unlink_urb_from_ep(ohci_to_hcd(ohci), urb);
	spin_unlock (&ohci->lock);
	usb_hcd_giveback_urb(ohci_to_hcd(ohci), urb, status);
	spin_lock (&ohci->lock);

	/*周期性传输请求为0*/
	if (ohci_to_hcd(ohci)->self.bandwidth_isoc_reqs == 0
			&& ohci_to_hcd(ohci)->self.bandwidth_int_reqs == 0) {
		ohci->hc_control &= ~(OHCI_CTRL_PLE|OHCI_CTRL_IE); /*关闭周期性和等时传输的链表*/
		ohci_writel (ohci, ohci->hc_control, &ohci->regs->control);  /*写回控制寄存器中*/
	}
}

/**
 * balance:找到传输load负载最小的分支
 * @ ohci:
 * @ interval: 周期性传输的轮询时间间隔
 * @ load:负载需要的帧周期(ns)
 */
static int balance (struct ohci_hcd *ohci, int interval, int load)
{
	int	i, branch = -ENOSPC;

	/* iso periods can be huge; iso tds specify frame numbers */
	if (interval > NUM_INTS)
		interval = NUM_INTS; /*32ms中断传输时间间隔*/

	/*找到负载最小，总线带宽足够传输load负载的分支*/
	for (i = 0; i < interval ; i++) {
		if (branch < 0 || ohci->load [branch] > ohci->load [i]) {
			int	j;

			/* usb 1.1 says 90% of one frame(1ms), 也就是大于0.9ms
			 * 查找分支负载小于0.9ms,也就是有足够总线周期的分支*/
			for (j = i; j < NUM_INTS; j += interval) {
				/*判断j的论寻是否有足够帧周期时间*/
				if ((ohci->load [j] + load) > 900)
					break;
			}
			if (j < NUM_INTS)
				continue;

			branch = i;  /*找到负载最小的分支`*/
		}
	}
	return branch;
}

/**
 * periodic_link:ed链入周期性传输链表
 */
static void periodic_link (struct ohci_hcd *ohci, struct ed *ed)
{
	unsigned i;

	ohci_vdbg (ohci, "link %sed %p branch %d [%dus.], interval %d\n",
		(ed->hwINFO & cpu_to_hc32 (ohci, ED_ISO)) ? "iso " : "",
		ed, ed->branch, ed->load, ed->interval);
	/*ed->branch :保存了负载最小的分支（轮训时间间隔）*/
	for (i = ed->branch; i < NUM_INTS; i += ed->interval) {
		struct ed	**prev = &ohci->periodic[i];
		__hc32		*prev_p = &ohci->hcca->int_table[i]; /*中断ed表*/
		struct ed	*here = *prev;

		/*ed插入到周期性链表中,周期间隔长的在链表的前面
		 * 周期间隔慢的在链表的后面，下面使用了一个小
		 * 技巧:怎样在O(1)时间没插入一个链表元素*/
		while (here && ed != here) {
			if (ed->interval > here->interval)
				break;
			prev = &here->ed_next;
			prev_p = &here->hwNextED;
			here = *prev;
		}
		if (ed != here) {
			ed->ed_next = here;
			if (here)
				ed->hwNextED = *prev_p;
			wmb ();
			*prev = ed; /*插入到周期链表中*/
			/*ed插入到int_table中*/
			*prev_p = cpu_to_hc32(ohci, ed->dma);
			wmb();
		}
		ohci->load [i] += ed->load; /*增加i分支的负载周期*/
	}
	/*计算出传输load负载需要的总线的带宽分配的次数*/
	ohci_to_hcd(ohci)->self.bandwidth_allocated += ed->load / ed->interval;
}

/**
 * periodic_unlink:在周期性传输链表中删除ed
 */
static void periodic_unlink (struct ohci_hcd *ohci, struct ed *ed)
{
	int	i;

	for (i = ed->branch; i < NUM_INTS; i += ed->interval) {
		struct ed	*temp;
		struct ed	**prev = &ohci->periodic [i];
		__hc32		*prev_p = &ohci->hcca->int_table [i];

		/*在周期性链表中删除ed，删除操作使用了一个小技巧:
		 * 怎样在O(1)时间内删除链表元素*/
		while (*prev && (temp = *prev) != ed) {
			prev_p = &temp->hwNextED;
			prev = &temp->ed_next;
		}
		if (*prev) {
			*prev_p = ed->hwNextED;
			*prev = ed->ed_next; /*从周期连表中删除*/
		}
		/*减少第i个分支的传输负载时间*/
		ohci->load [i] -= ed->load;
	}
	ohci_to_hcd(ohci)->self.bandwidth_allocated -= ed->load / ed->interval;

	ohci_vdbg (ohci, "unlink %sed %p branch %d [%dus.], interval %d\n",
		(ed->hwINFO & cpu_to_hc32 (ohci, ED_ISO)) ? "iso " : "",
		ed, ed->branch, ed->load, ed->interval);
}
/**  
 * ed_schedule:将ed加入到ohci主机的ed传输队列中
 * @ochi :主机结构
 * @ed:待调度的ed结构
 * @ 注:控制和批量传输属于非周期性的传输，所放的队列不一样
 *		中断和等时传输属于周期性的传输，处理的机制是一样的
 *      控制传输和批量传输：加入队列，形成双向的ed链表
*/
static int ed_schedule (struct ohci_hcd *ohci, struct ed *ed)
{
	int	branch;

	ed->state = ED_OPER;
	ed->ed_prev = NULL;
	ed->ed_next = NULL;
	ed->hwNextED = 0;
	if (quirk_zfmicro(ohci)
			&& (ed->type == PIPE_INTERRUPT)
			&& !(ohci->eds_scheduled++))
		mod_timer(&ohci->unlink_watchdog, round_jiffies(jiffies + HZ));
	wmb ();

	switch (ed->type) {
	case PIPE_CONTROL:
		/*如果主机的控制传输的链表为空，表示没有其它的ed结构加入进来，此时
		 * 直接将该ed作为ed的ed_controlhead头，否则，将ed插入到主机的控制传输
		 * 链表的尾部
		*/
		if (ohci->ed_controltail == NULL) {
			WARN_ON (ohci->hc_control & OHCI_CTRL_CLE);
			ohci_writel (ohci, ed->dma,
					&ohci->regs->ed_controlhead);
		} else {
			ohci->ed_controltail->ed_next = ed;  /*插入到ed的队列尾部*/
			ohci->ed_controltail->hwNextED = cpu_to_hc32 (ohci,
								ed->dma);  /*更新队列尾部的下一个ed的地址域*/
		}
		ed->ed_prev = ohci->ed_controltail;
		/*ed为空或者ed全部处理完毕，则直接复位控制传输*/
		if (!ohci->ed_controltail && !ohci->ed_rm_list) {
			wmb();
			ohci->hc_control |= OHCI_CTRL_CLE; /*使能ed控制传输*/
			ohci_writel (ohci, 0, &ohci->regs->ed_controlcurrent); /*复位当前的控制传输的ed地址*/
			ohci_writel (ohci, ohci->hc_control,
					&ohci->regs->control);
		}
		/*移动ed的尾指针*/
		ohci->ed_controltail = ed;
		break;

	case PIPE_BULK:
        /*如果host的bulk链表为空，即表示没有其它的ed插入到bulk链表中
		 * 则该ed直接当作bulk的头指针*/
		if (ohci->ed_bulktail == NULL) { 
			WARN_ON (ohci->hc_control & OHCI_CTRL_BLE);
			ohci_writel (ohci, ed->dma, &ohci->regs->ed_bulkhead);
		} else {
			ohci->ed_bulktail->ed_next = ed;
			ohci->ed_bulktail->hwNextED = cpu_to_hc32 (ohci,
								ed->dma);
		}
		ed->ed_prev = ohci->ed_bulktail;
		if (!ohci->ed_bulktail && !ohci->ed_rm_list) {
			wmb();
			ohci->hc_control |= OHCI_CTRL_BLE;
			ohci_writel (ohci, 0, &ohci->regs->ed_bulkcurrent);
			ohci_writel (ohci, ohci->hc_control,
					&ohci->regs->control);
		}
		ohci->ed_bulktail = ed;
		break;

	// case PIPE_INTERRUPT:
	// case PIPE_ISOCHRONOUS:
	default:  /*周期性传输类型*/
		branch = balance (ohci, ed->interval, ed->load);
		if (branch < 0) {
			ohci_dbg (ohci,
				"ERR %d, interval %d msecs, load %d\n",
				branch, ed->interval, ed->load);
			// FIXME if there are TDs queued, fail them!
			return branch;
		}
		ed->branch = branch;
		periodic_link (ohci, ed);
	}

	/* the HC may not see the schedule updates yet, but if it does
	 * then they'll be properly ordered.
	 */
	return 0;
}

/**
 * ed_deschedule: ed从ED的队列中删除
 * @ ohci: 主机结构
 * @ ed: 待删除的ed
 */
static void ed_deschedule (struct ohci_hcd *ohci, struct ed *ed)
{
	/*设置待删除的ed为SKIP*/
	ed->hwINFO |= cpu_to_hc32 (ohci, ED_SKIP);
	wmb ();
	/*ed的状态设置为UNLINK*/
	ed->state = ED_UNLINK;

	switch (ed->type) {
	case PIPE_CONTROL:
		/* remove ED from the HC's list: */
		if (ed->ed_prev == NULL) {   
			if (!ed->hwNextED) {
				ohci->hc_control &= ~OHCI_CTRL_CLE;
				ohci_writel (ohci, ohci->hc_control,
						&ohci->regs->control);
				// a ohci_readl() later syncs CLE with the HC
			} else { 
				/*point hc controlhead to next ed*/
				ohci_writel (ohci,
					hc32_to_cpup (ohci, &ed->hwNextED), 
					&ohci->regs->ed_controlhead);
			}
		} else {
			ed->ed_prev->ed_next = ed->ed_next;
			ed->ed_prev->hwNextED = ed->hwNextED;
		}
		/* remove ED from the HCD's list: */
		if (ohci->ed_controltail == ed) {  
			ohci->ed_controltail = ed->ed_prev;
			if (ohci->ed_controltail)
				ohci->ed_controltail->ed_next = NULL;
		} else if (ed->ed_next) {
			ed->ed_next->ed_prev = ed->ed_prev;
		}
		break;

	case PIPE_BULK:
		/* remove ED from the HC's list: */
		if (ed->ed_prev == NULL) {
			if (!ed->hwNextED) {
				ohci->hc_control &= ~OHCI_CTRL_BLE;
				ohci_writel (ohci, ohci->hc_control,
						&ohci->regs->control);
				// a ohci_readl() later syncs BLE with the HC
			} else
				ohci_writel (ohci,
					hc32_to_cpup (ohci, &ed->hwNextED),
					&ohci->regs->ed_bulkhead);
		} else {
			ed->ed_prev->ed_next = ed->ed_next;
			ed->ed_prev->hwNextED = ed->hwNextED;
		}
		/* remove ED from the HCD's list: */
		if (ohci->ed_bulktail == ed) {
			ohci->ed_bulktail = ed->ed_prev;
			if (ohci->ed_bulktail)
				ohci->ed_bulktail->ed_next = NULL;
		} else if (ed->ed_next) {
			ed->ed_next->ed_prev = ed->ed_prev;
		}
		break;

	// case PIPE_INTERRUPT:
	// case PIPE_ISOCHRONOUS:
	default:
		periodic_unlink (ohci, ed);
		break;
	}
}

/* 
 * ed_get : 初始化一个ed结构，并返回 
 * @ ochi:  ochi主机结构
 * @ ep:    相对于主机来讲的一个ed结构（内部包含所有的ed信息）
 * @ udev:  usb设备结构
 * @ pipe:  通信的pipe
 * @ interval:  帧间隔时间（ms）
 */
static struct ed *ed_get (struct ohci_hcd *ohci,struct usb_host_endpoint *ep,
	  struct usb_device *udev, unsigned int pipe, int interval) 
{
	struct ed *ed;
	unsigned long flags;
	struct td *td;
	int	is_out;
	u32	info;

	spin_lock_irqsave (&ohci->lock, flags);

	if ((ed = ep->hcpriv)!= NULL)
		goto done;

	ed = ed_alloc (ohci, GFP_ATOMIC);
	if (!ed) {
		/* out of memory */
		goto done;
	}

	/*为ed的td传输队列申请链表尾节点*/
	td = td_alloc (ohci, GFP_ATOMIC);
	if (!td) {
		/* out of memory */
		ed_free (ohci, ed);
		ed = NULL;
		goto done;
	}
	ed->dummy = td; 
	ed->hwTailP = cpu_to_hc32 (ohci, td->td_dma); /*这是td传输的开始地方*/
	ed->hwHeadP = ed->hwTailP;	/* ED_C, ED_H zeroed */
	ed->state = ED_IDLE;

	is_out = !(ep->desc.bEndpointAddress & USB_DIR_IN);

	/* FIXME usbcore changes dev->devnum before SET_ADDRESS
	 * succeeds ... otherwise we wouldn't need "pipe".
	 */
	info = usb_pipedevice (pipe);
	ed->type = usb_pipetype(pipe);

	info |= (ep->desc.bEndpointAddress & ~USB_DIR_IN) << 7;
	info |= le16_to_cpu(ep->desc.wMaxPacketSize) << 16;
	if (udev->speed == USB_SPEED_LOW)
		info |= ED_LOWSPEED;
		/* only control transfers store pids in tds */
	if (ed->type != PIPE_CONTROL) {
		info |= is_out ? ED_OUT : ED_IN;
		if (ed->type != PIPE_BULK) {
				/* periodic transfers... */
			if (ed->type == PIPE_ISOCHRONOUS)
				info |= ED_ISO;
			else if (interval > 32)	/* iso can be bigger */
				interval = 32;
			ed->interval = interval; /*帧间隔*/
			ed->load = usb_calc_bus_time (
				udev->speed, !is_out,
				ed->type == PIPE_ISOCHRONOUS,
				le16_to_cpu(ep->desc.wMaxPacketSize))
					/ 1000;
		}
	}
	ed->hwINFO = cpu_to_hc32(ohci, info);
	ep->hcpriv = ed;  /*将申请的ed的结构体保存在主机端的ep的私有空间中*/

done:
	spin_unlock_irqrestore (&ohci->lock, flags);
	return ed;
}
/**
 * start_ed_unlinl:将ed从主机的ED调度链表中删除
 * @ ohci: 主机结构
 * @ ed: 待删除ed结构
 */
static void start_ed_unlink (struct ohci_hcd *ohci, struct ed *ed)
{
	ed->hwINFO |= cpu_to_hc32 (ohci, ED_DEQUEUE);
	ed_deschedule (ohci, ed);

	 /*将ed插入到ohci的ed_rm_list当中*/
	ed->ed_next = ohci->ed_rm_list;
	ed->ed_prev = NULL;
	ohci->ed_rm_list = ed;

	/* enable SOF interrupt */
	ohci_writel (ohci, OHCI_INTR_SF, &ohci->regs->intrstatus);
	ohci_writel (ohci, OHCI_INTR_SF, &ohci->regs->intrenable);
	// flush those writes, and get latest HCCA contents
	(void) ohci_readl (ohci, &ohci->regs->control);

	/* SF interrupt might get delayed; record the frame counter value that
	 * indicates when the HC isn't looking at it, so concurrent unlinks
	 * behave.  frame_no wraps every 2^16 msec, and changes right before
	 * SF is triggered.
	 */
	/*tick保存当前传输时的帧号+1*/
	ed->tick = ohci_frame_no(ohci) + 1;
}

/**
 * td_fill:用urb内容初始化td队列 
 * @ ohci:  ohci主机结构
 * @ info:  TD传输的属性值 
 * @ data:  传输数据地址
 * @ len:   传输的数据长度
 * @ urb:   传输的urb结构
 * @ index: td传输数组的下标索引
*/
static void td_fill (struct ohci_hcd *ohci, u32 info, dma_addr_t data, 
				int len, struct urb *urb, int index)
{
	struct td *td, *td_pt;
	struct urb_priv	*urb_priv = urb->hcpriv;
	int	is_iso = info & TD_ISO;
	int	hash;
	
	if (index != (urb_priv->length - 1)
			|| (urb->transfer_flags & URB_NO_INTERRUPT))
		info |= TD_DI_SET (6); /*延迟中断设置*/

	/*将index索引所指的td初始化并链入urb->priv->ed->dummy所指的队尾的队列中*/
	td_pt = urb_priv->td [index];
	td = urb_priv->td [index] = urb_priv->ed->dummy;
	urb_priv->ed->dummy = td_pt;

	td->ed = urb_priv->ed;
	td->next_dl_td = NULL;
	td->index = index;
	td->urb = urb;
	td->data_dma = data; /*数据传输的dma地址*/
	if (!len)
		data = 0;

	td->hwINFO = cpu_to_hc32 (ohci, info);
	if (is_iso) {
		td->hwCBP = cpu_to_hc32 (ohci, data & 0xFFFFF000);
		*ohci_hwPSWp(ohci, td, 0) = cpu_to_hc16 (ohci,
						(data & 0x0FFF) | 0xE000);
		td->ed->last_iso = info & 0xffff;
	} else {
		td->hwCBP = cpu_to_hc32 (ohci, data); /*设置TD传输的指针*/
	}
	if (data)
		td->hwBE = cpu_to_hc32 (ohci, data + len - 1);
	else
		td->hwBE = 0;
	td->hwNextTD = cpu_to_hc32 (ohci, td_pt->td_dma);

	/* append to queue */
	list_add_tail (&td->td_list, &td->ed->td_list);

	/* hash it for later reverse mapping
	 * td 插入到ohci->td_hash[hash]的链表中（头插法）*/
	hash = TD_HASH_FUNC (td->td_dma);
	td->td_hash = ohci->td_hash [hash];
	ohci->td_hash [hash] = td;

	/* HC might read the TD (or cachelines) right away ... */
	wmb ();
	/*更新ed描述符中的td尾指针*/
	td->ed->hwTailP = td->hwNextTD;
}

/**
 * td_submit_urb: 将urb加入到主机的ed的td队列当中
 * @ ochi:主机结构
 * @ urb：待传输的urb结构
 */
static void td_submit_urb (struct ohci_hcd	*ohci, struct urb *urb) 
{
	struct urb_priv	*urb_priv = urb->hcpriv;
	dma_addr_t	data;
	int		data_len = urb->transfer_buffer_length; /*获得urb传输的总数据长度*/
	int		cnt = 0;
	u32		info = 0;
	int		is_out = usb_pipeout (urb->pipe);
	int		periodic = 0;

	/* OHCI handles the bulk/interrupt data toggles itself.  We just
	 * use the device toggle bits for resetting, and rely on the fact
	 * that resetting toggle is meaningless if the endpoint is active.
	 */
	if (!usb_gettoggle (urb->dev, usb_pipeendpoint (urb->pipe), is_out)) {
		usb_settoggle (urb->dev, usb_pipeendpoint (urb->pipe),
			is_out, 1);
		urb_priv->ed->hwHeadP &= ~cpu_to_hc32 (ohci, ED_C);
	}

	urb_priv->td_cnt = 0;
	/*urb传输加入到主机的host pending中,因此在传输的过程中
	 * pending 用于决定主机是否有传输的请求*/
	list_add (&urb_priv->pending, &ohci->pending);

	if (data_len)
		data = urb->transfer_dma; /*传输队列的数据地址指针*/
	else
		data = 0;

	switch (urb_priv->ed->type) {
	case PIPE_INTERRUPT:
		/*有中断传输的请求*/
		periodic = ohci_to_hcd(ohci)->self.bandwidth_int_reqs++ == 0
			&& ohci_to_hcd(ohci)->self.bandwidth_isoc_reqs == 0;
		/* FALLTHROUGH */
	case PIPE_BULK:
		info = is_out
			? TD_T_TOGGLE | TD_CC | TD_DP_OUT
			: TD_T_TOGGLE | TD_CC | TD_DP_IN;
		/* TDs _could_ transfer up to 8K each */
		while (data_len > 4096) {
			td_fill (ohci, info, data, 4096, urb, cnt);
			data += 4096;
			data_len -= 4096;
			cnt++;
		}
		/* maybe avoid ED halt on final TD short read */
		if (!(urb->transfer_flags & URB_SHORT_NOT_OK))
			info |= TD_R;
		td_fill (ohci, info, data, data_len, urb, cnt);
		cnt++;
		if ((urb->transfer_flags & URB_ZERO_PACKET)
				&& cnt < urb_priv->length) {
			td_fill (ohci, info, 0, 0, urb, cnt);
			cnt++;
		}
		/*写入命令状态寄存器，hc发起传输*/
		if (urb_priv->ed->type == PIPE_BULK) {
			wmb ();
			ohci_writel (ohci, OHCI_BLF, &ohci->regs->cmdstatus); 
		}
		break;

	/* control manages DATA0/DATA1 toggle per-request; SETUP resets it,
	 * any DATA phase works normally, and the STATUS ack is special.
	 */
	case PIPE_CONTROL:
		info = TD_CC | TD_DP_SETUP | TD_T_DATA0;
		td_fill (ohci, info, urb->setup_dma, 8, urb, cnt++);
		if (data_len > 0) {
			info = TD_CC | TD_R | TD_T_DATA1;
			info |= is_out ? TD_DP_OUT : TD_DP_IN;
			/* NOTE:  mishandles transfers >8K, some >4K */
			td_fill (ohci, info, data, data_len, urb, cnt++);
		}
		info = (is_out || data_len == 0)
			? TD_CC | TD_DP_IN | TD_T_DATA1
			: TD_CC | TD_DP_OUT | TD_T_DATA1;
		td_fill (ohci, info, data, 0, urb, cnt++);
		wmb ();
		/*开始传输*/
		ohci_writel (ohci, OHCI_CLF, &ohci->regs->cmdstatus);
		break;

	/* ISO has no retransmit, so no toggle; and it uses special TDs.
	 * Each TD could handle multiple consecutive frames (interval 1);
	 * we could often reduce the number of TDs here.
	 */
	case PIPE_ISOCHRONOUS:
		for (cnt = 0; cnt < urb->number_of_packets; cnt++) {
			int	frame = urb->start_frame;

			// FIXME scheduling should handle frame counter
			// roll-around ... exotic case (and OHCI has
			// a 2^16 iso range, vs other HCs max of 2^10)
			frame += cnt * urb->interval;
			frame &= 0xffff;
			td_fill (ohci, TD_CC | TD_ISO | frame,
				data + urb->iso_frame_desc [cnt].offset,
				urb->iso_frame_desc [cnt].length, urb, cnt);
		}
		if (ohci_to_hcd(ohci)->self.bandwidth_isoc_reqs == 0) {
			if (quirk_amdiso(ohci))
				quirk_amd_pll(0);
			if (quirk_amdprefetch(ohci))
				sb800_prefetch(ohci, 1);
		}
		/*有等时传输的请求,只传输1次*/
		periodic = ohci_to_hcd(ohci)->self.bandwidth_isoc_reqs++ == 0
			&& ohci_to_hcd(ohci)->self.bandwidth_int_reqs == 0;
		break;
	}

	/* start periodic dma if needed */
	if (periodic) {
		wmb ();
		ohci->hc_control |= OHCI_CTRL_PLE|OHCI_CTRL_IE;
		ohci_writel (ohci, ohci->hc_control, &ohci->regs->control);
	}

	// ASSERT (urb_priv->length == cnt);
}

/**
 * td_done: 更新urb实际传输的数据长度和状态信息,并且从td_list删除该td
 */
static int td_done(struct ohci_hcd *ohci, struct urb *urb, struct td *td)
{
	u32	tdINFO = hc32_to_cpup (ohci, &td->hwINFO);
	int	cc = 0;
	int	status = -EINPROGRESS;

	list_del (&td->td_list);

	/* ISO ... drivers see per-TD length/status */
	if (tdINFO & TD_ISO) {
		/*获得起始索引的PSW值,为什么总是index=0的PSW值呢*/
		u16	tdPSW = ohci_hwPSW(ohci, td, 0);
		int	dlen = 0;

		/*获得传输条件码*/
		cc = (tdPSW >> 12) & 0xF;
		if (tdINFO & TD_CC)	/* hc didn't touch? */
			return status;

		if (usb_pipeout (urb->pipe))
			dlen = urb->iso_frame_desc [td->index].length;
		else {
			/* short reads are always OK for ISO */
			if (cc == TD_DATAUNDERRUN)
				cc = TD_CC_NOERROR;
			/*获得传输数据的长度*/
			dlen = tdPSW & 0x3ff;
		}
		urb->actual_length += dlen;
		urb->iso_frame_desc [td->index].actual_length = dlen;
		urb->iso_frame_desc [td->index].status = cc_to_error [cc];

		if (cc != TD_CC_NOERROR)
			ohci_vdbg (ohci,
				"urb %p iso td %p (%d) len %d cc %d\n",
				urb, td, 1 + td->index, dlen, cc);

	} else {
		int	type = usb_pipetype (urb->pipe);
		u32	tdBE = hc32_to_cpup (ohci, &td->hwBE);  /*获得buf end pointer*/

		cc = TD_CC_GET (tdINFO);  /*获得条件码信息*/

		/* update packet status if needed (short is normally ok) */
		if (cc == TD_DATAUNDERRUN
				&& !(urb->transfer_flags & URB_SHORT_NOT_OK))
			cc = TD_CC_NOERROR;
		if (cc != TD_CC_NOERROR && cc < 0x0E)
			status = cc_to_error[cc];  /*获得错误条件码信息*/

		/* count all non-empty packets except control SETUP packet */
		if ((type != PIPE_CONTROL || td->index != 0) && tdBE != 0) {
			/*td传输到buffer的尾部,即buffer中的数据全部传输完成
			 * 则更新td->hwCBP=0*/
			if (td->hwCBP == 0)
				urb->actual_length += tdBE - td->data_dma + 1;
			else
				urb->actual_length +=
					  hc32_to_cpup (ohci, &td->hwCBP)
					- td->data_dma;
		}

		if (cc != TD_CC_NOERROR && cc < 0x0E)
			ohci_vdbg (ohci,
				"urb %p td %p (%d) cc %d, len=%d/%d\n",
				urb, td, 1 + td->index, cc,
				urb->actual_length,
				urb->transfer_buffer_length);
	}
	return status;
}

/*
 * ed_halted: 设置ed的SKIP信息，并删除该ed中的td传输队列
 */
static void ed_halted(struct ohci_hcd *ohci, struct td *td, int cc)
{
	struct urb		*urb = td->urb;
	urb_priv_t		*urb_priv = urb->hcpriv;
	struct ed		*ed = td->ed;
	struct list_head	*tmp = td->td_list.next;
	__hc32			toggle = ed->hwHeadP & cpu_to_hc32 (ohci, ED_C);

	ed->hwINFO |= cpu_to_hc32 (ohci, ED_SKIP);
	wmb ();
	ed->hwHeadP &= ~cpu_to_hc32 (ohci, ED_H);

	 /*删除urb的所有td传输队列节点*/
	while (tmp != &ed->td_list) {
		struct td	*next;

		next = list_entry (tmp, struct td, td_list);
		tmp = next->td_list.next;

		if (next->urb != urb)
			break;
		list_del(&next->td_list);
		urb_priv->td_cnt++;
		ed->hwHeadP = next->hwNextTD | toggle;
	}

	switch (cc) {
	case TD_DATAUNDERRUN:
		if ((urb->transfer_flags & URB_SHORT_NOT_OK) == 0)
			break;
		/* fallthrough */
	case TD_CC_STALL:
		if (usb_pipecontrol (urb->pipe))
			break;
		/* fallthrough */
	default:
		ohci_dbg (ohci,
			"urb %p path %s ep%d%s %08x cc %d --> status %d\n",
			urb, urb->dev->devpath,
			usb_pipeendpoint (urb->pipe),
			usb_pipein (urb->pipe) ? "in" : "out",
			hc32_to_cpu (ohci, td->hwINFO),
			cc, cc_to_error [cc]);
	}
}

/**
 * dl_reverse_done_list:反转done_head的链表
 * @ 备注：因为,当ohci的TD传输完成（或者是传输失败时）都会
 *         将TD节点插入(头插法)到done_head链表中,因此，为了
 *         还原TD进入ED传输时的顺序，所以将done_head进行反转
 *         将变成TD进入的FIFO顺序
*/
static struct td *dl_reverse_done_list (struct ohci_hcd *ohci)
{
	u32		td_dma;
	struct td	*td_rev = NULL;
	struct td	*td = NULL;

	/*获得done队列的头指针地址*/
	td_dma = hc32_to_cpup (ohci, &ohci->hcca->done_head);
	ohci->hcca->done_head = 0;
	wmb();

	while (td_dma) {
		int		cc;

		td = dma_to_td (ohci, td_dma);
		if (!td) {
			ohci_err (ohci, "bad entry %8x\n", td_dma);
			break;
		}

		td->hwINFO |= cpu_to_hc32 (ohci, TD_DONE);           /*设置传输完成标记，可以被删除了*/
		cc = TD_CC_GET (hc32_to_cpup (ohci, &td->hwINFO));  /*获得传输条件码*/

		 /*有错误产生，且ed设置了halt位，则停止该ed*/
		if (cc != TD_CC_NOERROR
				&& (td->ed->hwHeadP & cpu_to_hc32 (ohci, ED_H)))
			ed_halted(ohci, td, cc);

		/*将td都反向链接起来(头插法)*/
		td->next_dl_td = td_rev; 
		td_rev = td;
		td_dma = hc32_to_cpup (ohci, &td->hwNextTD);
	}
	return td_rev;  /*反转后的链表头指针*/
}

/**
 * finish_unlinks 处理ed_rm_list的链表
 * @ tick:帧序号
*/
static void finish_unlinks (struct ohci_hcd *ohci, u16 tick)
{
	struct ed	*ed, **last;

rescan_all:
	/*处理由于主机的状态原因，从主机ed队列上删除临时存放在ed_rm_list的ed传输
	 * 存放在ed_rm_list上的ed,比如主机复位，挂起，stop都会导致已经加入到
	 * 主机ed队列上的ed被临时存放在ed_rm_list上
	 */
	for (last = &ohci->ed_rm_list, ed = *last; ed != NULL; ed = *last) {
		struct list_head	*entry, *tmp;
		int			completed, modified;
		__hc32			*prev;

		/* only take off EDs that the HC isn't using, accounting for
		 * frame counter wraps and EDs with partially retired TDs
		 */
		if (likely (HC_IS_RUNNING(ohci_to_hcd(ohci)->state))) {
			/*如果ed->tick大于tick，则跳过*/
			if (tick_before (tick, ed->tick)) {
skip_ed:
				last = &ed->ed_next;
				continue;
			}

			/*ed上还有未完成传输的td*/
			if (!list_empty (&ed->td_list)) {
				struct td	*td;
				u32		head;

				td = list_entry (ed->td_list.next, struct td,
							td_list);
				head = hc32_to_cpu (ohci, ed->hwHeadP) &
								TD_MASK;

				/* INTR_WDH may need to clean up first */
				if (td->td_dma != head) {
					if (ed == ohci->ed_to_check)
						ohci->ed_to_check = NULL;
					else
						goto skip_ed;
				}
			}
		}

		/*从ed_rm_list上删除ed*/
		*last = ed->ed_next;
		ed->ed_next = NULL;
		modified = 0;

		/*处理加入到ed中的td传输队列*/
rescan_this:
		completed = 0;
		prev = &ed->hwHeadP;
		/*处理ed的td传输队列*/
		list_for_each_safe (entry, tmp, &ed->td_list) {
			struct td	*td;
			struct urb	*urb;
			urb_priv_t	*urb_priv;
			__hc32		savebits;
			u32		tdINFO;

			/*得到ed的td传输链表*/
			td = list_entry (entry, struct td, td_list);
			urb = td->urb;
			urb_priv = td->urb->hcpriv;

			/*如果urb没有被取消,则跳过,因此只处理已经被取消的
			 * urb*/
			if (!urb->unlinked) {
				prev = &td->hwNextTD;
				continue;
			}

			/* patch pointer hc uses */
			savebits = *prev & ~cpu_to_hc32 (ohci, TD_MASK);
			*prev = td->hwNextTD | savebits;

			/*虽然urb被取消了，但是需要保存ed的toggle信息
			 * 为了后续的传输，不被打乱*/
			tdINFO = hc32_to_cpup(ohci, &td->hwINFO);
			if ((tdINFO & TD_T) == TD_T_DATA0)
				ed->hwHeadP &= ~cpu_to_hc32(ohci, ED_C);
			else if ((tdINFO & TD_T) == TD_T_DATA1)
				ed->hwHeadP |= cpu_to_hc32(ohci, ED_C);

			 
			 /*也许在urb完全被取消之前，TD已经传输了
			 * urb的一部分内容，则对已经传输完的内容
			 * 需要统计其传输信息，并增加传输个数*/
			td_done (ohci, urb, td);
			urb_priv->td_cnt++;

			/*urb td全部处理了,则释放相应的内存空间*/
			if (urb_priv->td_cnt == urb_priv->length) {
				modified = completed = 1;
				finish_urb(ohci, urb, 0);
			}
		}
		/*当前td的所有传输请求完成后，如果ed的td传输链表
		 * 不为空的话，继续该ed的td传输请求*/
		if (completed && !list_empty (&ed->td_list))
			goto rescan_this;

		/*该ed的所有传输请求应经完成了因此设置ed的状态为ED_IDLE*/
		ed->state = ED_IDLE;
		if (quirk_zfmicro(ohci) && ed->type == PIPE_INTERRUPT)
			ohci->eds_scheduled--;
		ed->hwHeadP &= ~cpu_to_hc32(ohci, ED_H);
		ed->hwNextED = 0;
		wmb ();
		ed->hwINFO &= ~cpu_to_hc32 (ohci, ED_SKIP | ED_DEQUEUE);

		/*如果ed的td队列不为空,则继续传输urb->unlinked = 1*/
		if (!list_empty (&ed->td_list)) {
			if (HC_IS_RUNNING(ohci_to_hcd(ohci)->state))
				ed_schedule (ohci, ed);
		}

		if (modified)  /*继续下一个ed*/
			goto rescan_all;
	}

	/*主机上临时ed队列全部处理完成，也就是说，主机上的ed队列都全部处理完成
	 * 因此,复位主机的ed队列指针
	 */  
	if (HC_IS_RUNNING(ohci_to_hcd(ohci)->state)
			&& ohci_to_hcd(ohci)->state != HC_STATE_QUIESCING
			&& !ohci->ed_rm_list) {
		u32	command = 0, control = 0;

		if (ohci->ed_controltail) {  /*控制传输的处理*/
			command |= OHCI_CLF;
			if (quirk_zfmicro(ohci))
				mdelay(1);
			if (!(ohci->hc_control & OHCI_CTRL_CLE)) {
				control |= OHCI_CTRL_CLE;
				ohci_writel (ohci, 0,
					&ohci->regs->ed_controlcurrent);
			}
		}
		if (ohci->ed_bulktail) {  /*批量传输的处理*/
			command |= OHCI_BLF;
			if (quirk_zfmicro(ohci))
				mdelay(1);
			if (!(ohci->hc_control & OHCI_CTRL_BLE)) {
				control |= OHCI_CTRL_BLE;
				ohci_writel (ohci, 0,
					&ohci->regs->ed_bulkcurrent);
			}
		}

		/* CLE/BLE to enable, CLF/BLF to (maybe) kickstart */
		if (control) {
			ohci->hc_control |= control;
			if (quirk_zfmicro(ohci))
				mdelay(1);
			ohci_writel (ohci, ohci->hc_control,
					&ohci->regs->control);
		}
		if (command) {
			if (quirk_zfmicro(ohci))
				mdelay(1);
			ohci_writel (ohci, command, &ohci->regs->cmdstatus);
		}
	}
}

/*
 * taskback_td :处理done list的td结点 
 */
static void takeback_td(struct ohci_hcd *ohci, struct td *td)
{
	struct urb	*urb = td->urb;
	urb_priv_t	*urb_priv = urb->hcpriv;
	struct ed	*ed = td->ed;
	int		status;

	/* update URB's length and status from TD */
	status = td_done(ohci, urb, td);
	urb_priv->td_cnt++;

	/* If all this urb's TDs are done, call complete() */
	if (urb_priv->td_cnt == urb_priv->length)
		finish_urb(ohci, urb, status);

	/* clean schedule:  unlink EDs that are no longer busy */
	if (list_empty(&ed->td_list)) { 
		/*ed 没有其它的td传输请求,则将该ED从ED传输链表中删除*/
		if (ed->state == ED_OPER)
			start_ed_unlink(ohci, ed);
	} else if ((ed->hwINFO & cpu_to_hc32(ohci, ED_SKIP | ED_DEQUEUE))
			== cpu_to_hc32(ohci, ED_SKIP)) {
		/*如果由于某种原因，导致ed被设置了SKIP标志,但是，
		 *此时ED中还有剩余的TD需要传输，则清空ED的SKIP，
		 *继续进行传输
		*/
		td = list_entry(ed->td_list.next, struct td, td_list);
		if (!(td->hwINFO & cpu_to_hc32(ohci, TD_DONE))) {
			ed->hwINFO &= ~cpu_to_hc32(ohci, ED_SKIP);
			/* ... hc may need waking-up */
			switch (ed->type) {
			case PIPE_CONTROL:
				ohci_writel(ohci, OHCI_CLF,
						&ohci->regs->cmdstatus);
				break;
			case PIPE_BULK:
				ohci_writel(ohci, OHCI_BLF,
						&ohci->regs->cmdstatus);
				break;
			}
		}
	}
}

/**
 * dl_done_list:ohci done list处理接口
 */
static void dl_done_list (struct ohci_hcd *ohci)
{
	struct td *td = dl_reverse_done_list (ohci);

	while (td) {
		struct td	*td_next = td->next_dl_td;
		takeback_td(ohci, td);
		td = td_next;
	}
}
