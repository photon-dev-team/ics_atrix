/* qmidevice.c - gobi QMI device
 * Copyright (c) 2010, Code Aurora Forum. All rights reserved.

 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#include "qmidevice.h"
#include "qcusbnet.h"

struct readreq {
	struct list_head node;
	void *data;
	u16 tid;
	u16 size;
};

struct notifyreq {
	struct list_head node;
	void (* func)(struct qcusbnet *, u16, void *);
	u16  tid;
	void *data;
};

struct client {
	struct list_head node;
	u16 cid;
	struct list_head reads;
	struct list_head notifies;
	struct list_head urbs;
};

struct urbsetup {
	u8 type;
	u8 code;
	u16 value;
	u16 index;
	u16 len;
};

struct qmihandle {
	u16 cid;
	struct qcusbnet *dev;
};

extern int debug;
extern int debug_level;
static int qcusbnet2k_fwdelay = 0;

static bool device_valid(struct qcusbnet *dev);
static struct client *client_bycid(struct qcusbnet *dev, u16 cid);
static bool client_addread(struct qcusbnet *dev, u16 cid, u16 tid, void *data, u16 size);
static bool client_delread(struct qcusbnet *dev, u16 cid, u16 tid, void **data, u16 *size);
static bool client_addnotify(struct qcusbnet *dev, u16 cid, u16 tid,
                             void (*hook)(struct qcusbnet *, u16 cid, void *),
                             void *data);
static bool client_notify(struct qcusbnet *dev, u16 cid, u16 tid);
static bool client_addurb(struct qcusbnet *dev, u16 cid, struct urb *urb);
static struct urb *client_delurb(struct qcusbnet *dev, u16 cid);

static int devqmi_open(struct inode *inode, struct file *file);
static long devqmi_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
static int devqmi_close(struct file *file, fl_owner_t ftable);
static ssize_t devqmi_read(struct file *file, char __user *buf, size_t size,
                           loff_t *pos);
static ssize_t devqmi_write(struct file *file, const char __user *buf,
                            size_t size, loff_t *pos);

#ifdef QMUX_IN_DRIVER
static bool qmi_ready(struct qcusbnet *dev, u16 timeout);
static void wds_callback(struct qcusbnet *dev, u16 cid, void *data);
static int setup_wds_callback(struct qcusbnet *dev);
static int qmidms_getmeid(struct qcusbnet *dev);
#endif

#define IOCTL_QMI_GET_SERVICE_FILE 0x8BE0 + 1
#define IOCTL_QMI_GET_DEVICE_VIDPID 0x8BE0 + 2
#define IOCTL_QMI_GET_DEVICE_MEID 0x8BE0 + 3
//#define CDC_GET_ENCAPSULATED_RESPONSE 0x01A1ll
#define CDC_GET_ENCAPSULATED_RESPONSE 0x05000001a1ll
#define CDC_CONNECTION_SPEED_CHANGE 0x08000000002AA1ll

struct file_operations devqmi_fops =
{
	.owner = THIS_MODULE,
	.read  = devqmi_read,
	.write = devqmi_write,
	.unlocked_ioctl = devqmi_ioctl,
	.open  = devqmi_open,
	.flush = devqmi_close,
};

#ifdef CONFIG_SMP
static inline void assert_locked(struct qcusbnet *dev)
{
	BUG_ON(!spin_is_locked(&dev->qmi.clients_lock));
}
#else
static inline void assert_locked(struct qcusbnet *dev)
{

}
#endif

static bool device_valid(struct qcusbnet *dev)
{
	return dev && dev->valid;
}

void printhex(const void *data, size_t size)
{
	const u8 *cdata = data;
	char *buf;
	size_t pos;

	buf = kmalloc(size * 3 + 1, GFP_ATOMIC);
	if (!buf) {
		VDBG("Unable to allocate buffer\n");
		return;
	}

	memset(buf, 0 , size * 3 + 1);

	for (pos = 0; pos < size; pos++) {
		snprintf(buf + (pos * 3), 4, "%02X ", cdata[pos]);
	}

	VDBG("   : %s\n", buf);

	kfree(buf);
}

void qc_setdown(struct qcusbnet *dev, u8 reason)
{
	set_bit(reason, &dev->down);
	netif_carrier_off(dev->usbnet->net);
}

void qc_cleardown(struct qcusbnet *dev, u8 reason)
{
	clear_bit(reason, &dev->down);
	if (!dev->down)
		netif_carrier_on(dev->usbnet->net);
}

bool qc_isdown(struct qcusbnet *dev, u8 reason)
{
	return test_bit(reason, &dev->down);
}

static void read_callback(struct urb *urb)
{
	struct list_head *node;
	u16 cid;
	struct client *client;
	void *data;
	void *copy;
	u16 size;
	struct qcusbnet *dev;
	unsigned long flags;
	u16 tid;
#ifdef QMUX_IN_DRIVER
	int result;
#endif

	if (!urb) {
		DBG("bad read URB\n");
		return;
	}

	dev = urb->context;
	if (!device_valid(dev)) {
		DBG("Invalid device!\n");
		return;
	}

	if (urb->status) {
		DBG("Read status = %d\n", urb->status);
		return;
	}

	VDBG("Read %d bytes\n", urb->actual_length);

	data = urb->transfer_buffer;
	size = urb->actual_length;

	printhex(data, size);

#ifdef QMUX_IN_DRIVER
	result = qmux_parse(&cid, data, size);
	if (result < 0) {
		DBG("Read error parsing QMUX %d\n", result);
		return;
	}

	if (size < result + 3) {
		DBG("Data buffer too small to parse\n");
		return;
	}

	if (cid == QMICTL)
		tid = *(u8*)(data + result + 1);
	else
		tid = *(u16*)(data + result + 1);
#else
	tid = 0;
	cid = dev->qmi.qmiidx;
#endif
	spin_lock_irqsave(&dev->qmi.clients_lock, flags);

	list_for_each(node, &dev->qmi.clients) {
		client = list_entry(node, struct client, node);
		if (client->cid == cid || (client->cid | 0xff00) == cid) {
			copy = kmalloc(size, GFP_ATOMIC);
			if (!copy) {
				DBG("%s malloc failed\n", __func__);
				spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
				return;
			}
			memcpy(copy, data, size);
			if (!client_addread(dev, client->cid, tid, copy, size)) {
				DBG("Error allocating pReadMemListEntry "
					  "read will be discarded\n");
				kfree(copy);
				spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
				return;
			}

			VDBG("Creating new readListEntry for client 0x%04X, TID %x\n",
			    cid, tid);

			client_notify(dev, client->cid, tid);

			if (cid >> 8 != 0xff)
				break;
		}
	}

	spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
}

static void int_callback(struct urb *urb)
{
	int status;
	int interval;
	struct qcusbnet *dev = (struct qcusbnet *)urb->context;

	if (!device_valid(dev)) {
		DBG("Invalid device!\n");
		return;
	}

	if (urb->status) {
		VDBG("Int status = %d\n", urb->status);
		if (urb->status != -EOVERFLOW)
			return;
	} else {
#if 0
		if ((urb->actual_length == 8)
		&&  (*(u64*)urb->transfer_buffer == CDC_GET_ENCAPSULATED_RESPONSE)) {
#else
		if (urb->actual_length == 8) {
#endif
			usb_fill_control_urb(dev->qmi.readurb, dev->usbnet->udev,
			                     usb_rcvctrlpipe(dev->usbnet->udev, 0),
			                     (unsigned char *)dev->qmi.readsetup,
			                     dev->qmi.readbuf,
			                     DEFAULT_READ_URB_LENGTH,
			                     read_callback, dev);
			status = usb_submit_urb(dev->qmi.readurb, GFP_ATOMIC);
			if (status) {
				DBG("Error submitting Read URB %d\n", status);
				return;
			}
		} else if ((urb->actual_length == 16)
		&& (*(u64*)urb->transfer_buffer == CDC_CONNECTION_SPEED_CHANGE)) {
			/* if upstream or downstream is 0, stop traffic.
			 * Otherwise resume it */
			if ((*(u32*)(urb->transfer_buffer + 8) == 0)
			||  (*(u32*)(urb->transfer_buffer + 12) == 0)) {
				qc_setdown(dev, DOWN_CDC_CONNECTION_SPEED);
				DBG("traffic stopping due to CONNECTION_SPEED_CHANGE\n");
			} else {
				qc_cleardown(dev, DOWN_CDC_CONNECTION_SPEED);
				DBG("resuming traffic due to CONNECTION_SPEED_CHANGE\n");
			}
		} else {
			DBG("ignoring invalid interrupt in packet\n");
			printhex(urb->transfer_buffer, urb->actual_length);
		}
	}

	interval = (dev->usbnet->udev->speed == USB_SPEED_HIGH) ? 7 : 3;

	usb_fill_int_urb(urb, urb->dev,	urb->pipe, urb->transfer_buffer,
	                 urb->transfer_buffer_length, urb->complete,
	                 urb->context, interval);
	status = usb_submit_urb(urb, GFP_ATOMIC);
	if (status)
		DBG("Error re-submitting Int URB %d\n", status);
	return;
}

int qc_startread(struct qcusbnet *dev)
{
	int interval;
	int numends;
	int i;
	struct usb_host_endpoint *endpoint = NULL;

	if (!device_valid(dev)) {
		DBG("Invalid device!\n");
		return -ENXIO;
	}

	dev->qmi.readurb = usb_alloc_urb(0, GFP_KERNEL);
	if (!dev->qmi.readurb) {
		DBG("Error allocating read urb\n");
		return -ENOMEM;
	}

	dev->qmi.inturb = usb_alloc_urb(0, GFP_KERNEL);
	if (!dev->qmi.inturb) {
		usb_free_urb(dev->qmi.readurb);
		DBG("Error allocating int urb\n");
		return -ENOMEM;
	}

	dev->qmi.readbuf = kmalloc(DEFAULT_READ_URB_LENGTH, GFP_KERNEL);
	if (!dev->qmi.readbuf) {
		usb_free_urb(dev->qmi.readurb);
		usb_free_urb(dev->qmi.inturb);
		DBG("Error allocating read buffer\n");
		return -ENOMEM;
	}

	dev->qmi.intbuf = kmalloc(DEFAULT_READ_URB_LENGTH, GFP_KERNEL);
	if (!dev->qmi.intbuf) {
		usb_free_urb(dev->qmi.readurb);
		usb_free_urb(dev->qmi.inturb);
		kfree(dev->qmi.readbuf);
		DBG("Error allocating int buffer\n");
		return -ENOMEM;
	}

	dev->qmi.readsetup = kmalloc(sizeof(*dev->qmi.readsetup), GFP_KERNEL);
	if (!dev->qmi.readsetup) {
		usb_free_urb(dev->qmi.readurb);
		usb_free_urb(dev->qmi.inturb);
		kfree(dev->qmi.readbuf);
		kfree(dev->qmi.intbuf);
		DBG("Error allocating setup packet buffer\n");
		return -ENOMEM;
	}

	dev->qmi.readsetup->type = 0xA1;
	dev->qmi.readsetup->code = 1;
	dev->qmi.readsetup->value = 0;
	dev->qmi.readsetup->index = dev->iface->cur_altsetting->desc.bInterfaceNumber;
	dev->qmi.readsetup->len = DEFAULT_READ_URB_LENGTH;
	VDBG("interface number is %d\n", dev->qmi.readsetup->index);

	interval = (dev->usbnet->udev->speed == USB_SPEED_HIGH) ? 7 : 3;
	
	numends = dev->iface->cur_altsetting->desc.bNumEndpoints;
	for (i = 0; i < numends; i++) {
		endpoint = dev->iface->cur_altsetting->endpoint + i;
		if (!endpoint) {
			DBG("invalid endpoint %u\n", i);
			return -EINVAL;
		}
		
		if (usb_endpoint_dir_in(&endpoint->desc)
		  && usb_endpoint_xfer_int(&endpoint->desc)) {
			VDBG("Interrupt endpoint is %x\n", endpoint->desc.bEndpointAddress);
			break;
		}
	}

	usb_fill_int_urb(dev->qmi.inturb, dev->usbnet->udev,
	                 usb_rcvintpipe(dev->usbnet->udev, endpoint->desc.bEndpointAddress),
	                 dev->qmi.intbuf, DEFAULT_READ_URB_LENGTH,
	                 int_callback, dev, interval);

	return usb_submit_urb(dev->qmi.inturb, GFP_KERNEL);
}

void qc_stopread(struct qcusbnet * dev)
{
	if (dev->qmi.readurb) {
		VDBG("Killing read URB\n");
		usb_kill_urb(dev->qmi.readurb);
	}

	if (dev->qmi.inturb) {
		VDBG("Killing int URB\n");
		usb_kill_urb(dev->qmi.inturb);
	}

	kfree(dev->qmi.readsetup);
	dev->qmi.readsetup = NULL;
	kfree(dev->qmi.readbuf);
	dev->qmi.readbuf = NULL;
	kfree(dev->qmi.intbuf);
	dev->qmi.intbuf = NULL;

	usb_free_urb(dev->qmi.readurb);
	dev->qmi.readurb = NULL;
	usb_free_urb(dev->qmi.inturb);
	dev->qmi.inturb = NULL;
}

#ifdef QMUX_IN_DRIVER
static int read_async(struct qcusbnet *dev, u16 cid, u16 tid,
                      void (*hook)(struct qcusbnet*, u16, void *), void *data)
{
	struct list_head *node;
	struct client *client;
	struct readreq *readreq;

	unsigned long flags;

	if (!device_valid(dev)) {
		DBG("Invalid device!\n");
		return -ENXIO;
	}

	spin_lock_irqsave(&dev->qmi.clients_lock, flags);

	client = client_bycid(dev, cid);
	if (!client) {
		DBG("Could not find matching client ID 0x%04X\n", cid);
		spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
		return -ENXIO;
	}

	list_for_each(node, &client->reads) {
		readreq = list_entry(node, struct readreq, node);
		if (!tid || tid == readreq->tid) {
			spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
			hook(dev, cid, data);
			return 0;
		}
	}

	if (!client_addnotify(dev, cid, tid, hook, data))
		DBG("Unable to register for notification\n");

	spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
	return 0;
}
#endif

static void upsem(struct qcusbnet *dev, u16 cid, void *data) 
{
	VDBG("0x%04X\n", cid);
	up((struct semaphore *)data);
}

static int read_sync(struct qcusbnet *dev, void **buf, u16 cid, u16 tid)
{
	struct list_head *node;
	int result;
	struct client * client;
	struct notifyreq *notify;
	struct semaphore sem;
	void * data;
	unsigned long flags;
	u16 size;

	if (!device_valid(dev)) {
		DBG("Invalid device!\n");
		return -ENXIO;
	}

	spin_lock_irqsave(&dev->qmi.clients_lock, flags);

	client = client_bycid(dev, cid);
	if (!client) {
		DBG("Could not find matching client ID 0x%04X\n", cid);
		spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
		return -ENXIO;
	}

	while (!client_delread(dev, cid, tid, &data, &size)) {
		sema_init(&sem, 0);
		if (!client_addnotify(dev, cid, tid, upsem, &sem)) {
			DBG("unable to register for notification\n");
			spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
			return -EFAULT;
		}

		spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);

		result = down_interruptible(&sem);
		if (result) {
			DBG("Interrupted %d\n", result);
			spin_lock_irqsave(&dev->qmi.clients_lock, flags);
			list_for_each(node, &client->notifies) {
				notify = list_entry(node, struct notifyreq, node);
				if (notify->data == &sem) {
					list_del(&notify->node);
					kfree(notify);
					break;
				}
			}

			spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
			return -EINTR;
		}

		if (!device_valid(dev)) {
			DBG("Invalid device!\n");
			return -ENXIO;
		}

		spin_lock_irqsave(&dev->qmi.clients_lock, flags);
	}

	spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
	*buf = data;
	return size;
}

static void write_callback(struct urb *urb)
{
	if (!urb) {
		DBG("null urb\n");
		return;
	}

	VDBG("Write status/size %d/%d\n", urb->status, urb->actual_length);
	up((struct semaphore *)urb->context);
}

static int write_sync(struct qcusbnet *dev, char *buf, int size, u16 cid)
{
	int result;
	struct semaphore sem;
	struct urb *urb;
	struct urbsetup setup;
	unsigned long flags;

	if (!device_valid(dev)) {
		DBG("Invalid device!\n");
		return -ENXIO;
	}

	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb) {
		DBG("URB mem error\n");
		return -ENOMEM;
	}

#ifdef QMUX_IN_DRIVER
	result = qmux_fill(cid, buf, size);
	if (result < 0) {
		usb_free_urb(urb);
		return result;
	}
#endif
	/* CDC Send Encapsulated Request packet */
	setup.type = 0x21;
	setup.code = 0;
	setup.value = 0;
	setup.index = dev->iface->cur_altsetting->desc.bInterfaceNumber;
	setup.len = 0;
	setup.len = size;

	usb_fill_control_urb(urb, dev->usbnet->udev,
	                     usb_sndctrlpipe(dev->usbnet->udev, 0),
	                     (unsigned char *)&setup, (void*)buf, size,
	                     NULL, dev);

	VDBG("Actual Write:\n");
	printhex(buf, size);

	sema_init(&sem, 0);

	urb->complete = write_callback;
	urb->context = &sem;

	result = usb_autopm_get_interface(dev->iface);
	if (result < 0) {
		DBG("unable to resume interface: %d\n", result);
		if (result == -EPERM) {
			qc_suspend(dev->iface, PMSG_SUSPEND);
		}
		return result;
	}

	spin_lock_irqsave(&dev->qmi.clients_lock, flags);

	if (!client_addurb(dev, cid, urb)) {
		usb_free_urb(urb);
		spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
		usb_autopm_put_interface(dev->iface);
		return -EINVAL;
	}

	result = usb_submit_urb(urb, GFP_KERNEL);
	if (result < 0)	{
		DBG("submit URB error %d\n", result);
		if (client_delurb(dev, cid) != urb) {
			DBG("Didn't get write URB back\n");
		}

		usb_free_urb(urb);

		spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
		usb_autopm_put_interface(dev->iface);
		return result;
	}

	spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
	result = down_interruptible(&sem);
	if (!device_valid(dev)) {
		DBG("Invalid device!\n");
		return -ENXIO;
	}

	usb_autopm_put_interface(dev->iface);
	spin_lock_irqsave(&dev->qmi.clients_lock, flags);
	if (client_delurb(dev, cid) != urb)
	{
		DBG("Didn't get write URB back\n");
		spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
		return -EINVAL;
	}
	spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);

	if (!result) {
		if (!urb->status) {
			result = size;
		} else {
			DBG("bad status = %d\n", urb->status);
			result = urb->status;
		}
	} else {
		DBG("Interrupted %d !!!\n", result);
		DBG("Device may be in bad state and need reset !!!\n");
		usb_kill_urb(urb);
	}

	usb_free_urb(urb);
	return result;
}

static int client_alloc(struct qcusbnet *dev, u8 type)
{
	u16 cid;
	struct client *client;
	unsigned long flags;
#ifdef QMUX_IN_DRIVER
	int result;
	void * wbuf;
	size_t wbufsize;
	void * rbuf;
	u16 rbufsize;
	u8 tid;
#endif

	if (!device_valid(dev)) {
		DBG("Invalid device!\n");
		return -ENXIO;
	}

#ifdef QMUX_IN_DRIVER
	if (type) {
		tid = atomic_add_return(1, &dev->qmi.qmitid);
		if (!tid)
			atomic_add_return(1, &dev->qmi.qmitid);
		wbuf = qmictl_new_getcid(tid, type, &wbufsize);
		if (!wbuf)
			return -ENOMEM;
		result = write_sync(dev, wbuf, wbufsize, QMICTL);
		kfree(wbuf);

		if (result < 0)
			return result;

		result = read_sync(dev, &rbuf, QMICTL, tid);
		if (result < 0) {
			DBG("bad read data %d\n", result);
			return result;
		}
		rbufsize = result;

		result = qmictl_alloccid_resp(rbuf, rbufsize, &cid);
		kfree(rbuf);

		if (result < 0)
			return result;
	} else {
		cid = 0;
	}
#else
	cid = type;
#endif

	spin_lock_irqsave(&dev->qmi.clients_lock, flags);
	if (client_bycid(dev, cid)) {
		DBG("Client memory already exists\n");
		spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
		return -ETOOMANYREFS;
	}

	client = kmalloc(sizeof(*client), GFP_ATOMIC);
	if (!client) {
		DBG("Error allocating read list\n");
		spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
		return -ENOMEM;
	}

	list_add_tail(&client->node, &dev->qmi.clients);
	client->cid = cid;
	INIT_LIST_HEAD(&client->reads);
	INIT_LIST_HEAD(&client->notifies);
	INIT_LIST_HEAD(&client->urbs);
	spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
	return cid;
}

static void client_free(struct qcusbnet *dev, u16 cid)
{
	struct list_head *node, *tmp;
	struct client *client;
	struct urb * urb;
	void * data;
	u16 size;
	unsigned long flags;
#ifdef QMUX_IN_DRIVER
	int result;
	void * wbuf;
	size_t wbufsize;
	void * rbuf;
	u16 rbufsize;
	u8 tid;
#endif

	if (!device_valid(dev)) {
		DBG("invalid device\n");
		return;
	}

	VDBG("releasing 0x%04X\n", cid);

#ifdef QMUX_IN_DRIVER
	if (cid != QMICTL)
	{
		tid = atomic_add_return(1, &dev->qmi.qmitid);
		if (!tid)
			tid = atomic_add_return(1, &dev->qmi.qmitid);
		wbuf = qmictl_new_releasecid(tid, cid, &wbufsize);
		if (!wbuf) {
			DBG("memory error\n");
		} else {
			result = write_sync(dev, wbuf, wbufsize, QMICTL);
			kfree(wbuf);

			if (result < 0) {
				DBG("bad write status %d\n", result);
			} else {
				result = read_sync(dev, &rbuf, QMICTL, tid);
				if (result < 0) {
					DBG("bad read status %d\n", result);
				} else {
					rbufsize = result;
					result = qmictl_freecid_resp(rbuf, rbufsize);
					kfree(rbuf);
					if (result < 0)
						DBG("error %d parsing response\n", result);
				}
			}
		}
	}
#endif

	spin_lock_irqsave(&dev->qmi.clients_lock, flags);
	list_for_each_safe(node, tmp, &dev->qmi.clients) {
		client = list_entry(node, struct client, node);
		if (client->cid == cid) {
			while (client_notify(dev, cid, 0));

			urb = client_delurb(dev, cid);
			while (urb != NULL) {
				usb_kill_urb(urb);
				usb_free_urb(urb);
				urb = client_delurb(dev, cid);
			}

			while (client_delread(dev, cid, 0, &data, &size))
				kfree(data);

			list_del(&client->node);
			kfree(client);
		}
	}

	spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
}

struct client *client_bycid(struct qcusbnet *dev, u16 cid)
{
	struct list_head *node;
	struct client *client;

	if (!device_valid(dev))	{
		DBG("Invalid device\n");
		return NULL;
	}

	assert_locked(dev);

	list_for_each(node, &dev->qmi.clients) {
		client = list_entry(node, struct client, node);
		if (client->cid == cid)
			return client;
	}

	VDBG("Could not find client mem 0x%04X\n", cid);
	return NULL;
}

static bool client_addread(struct qcusbnet *dev, u16 cid, u16 tid, void *data,
                           u16 size)
{
	struct client *client;
	struct readreq *req;

	assert_locked(dev);

	client = client_bycid(dev, cid);
	if (!client) {
		DBG("Could not find this client's memory 0x%04X\n", cid);
		return false;
	}

	req = kmalloc(sizeof(*req), GFP_ATOMIC);
	if (!req) {
		DBG("Mem error\n");
		return false;
	}

	req->data = data;
	req->size = size;
	req->tid = tid;

	list_add_tail(&req->node, &client->reads);

	return true;
}

static bool client_delread(struct qcusbnet *dev, u16 cid, u16 tid, void **data,
                           u16 *size)
{
	struct client *client;
	struct readreq *req;
	struct list_head *node;

	assert_locked(dev);

	client = client_bycid(dev, cid);
	if (!client) {
		DBG("Could not find this client's memory 0x%04X\n", cid);
		return false;
	}

	list_for_each(node, &client->reads) {
		req = list_entry(node, struct readreq, node);
		if (!tid || tid == req->tid) {
			*data = req->data;
			*size = req->size;
			list_del(&req->node);
			kfree(req);
			return true;
		}

		VDBG("skipping 0x%04X data TID = %x\n", cid, req->tid);
	}

	VDBG("No read memory to pop, Client 0x%04X, TID = %x\n", cid, tid);
	return false;
}

static bool client_addnotify(struct qcusbnet *dev, u16 cid, u16 tid,
                      void (*hook)(struct qcusbnet *, u16, void *),
                      void *data)
{
	struct client *client;
	struct notifyreq *req;

	assert_locked(dev);

	client = client_bycid(dev, cid);
	if (!client) {
		DBG("Could not find this client's memory 0x%04X\n", cid);
		return false;
	}

	req = kmalloc(sizeof(*req), GFP_ATOMIC);
	if (!req) {
		DBG("Mem error\n");
		return false;
	}

	list_add_tail(&req->node, &client->notifies);
	req->func = hook;
	req->data = data;
	req->tid = tid;

	return true;
}

static bool client_notify(struct qcusbnet *dev, u16 cid, u16 tid)
{
	struct client *client;
	struct notifyreq *delnotify, *notify;
	struct list_head *node;

	assert_locked(dev);

	client = client_bycid(dev, cid);
	if (!client) {
		DBG("Could not find this client's memory 0x%04X\n", cid);
		return false;
	}

	delnotify = NULL;

	list_for_each(node, &client->notifies) {
		notify = list_entry(node, struct notifyreq, node);
		if (!tid || !notify->tid || tid == notify->tid) {
			delnotify = notify;
			break;
		}

		VDBG("skipping data TID = %x\n", notify->tid);
	}

	if (delnotify) {
		list_del(&delnotify->node);
		if (delnotify->func) {
			spin_unlock(&dev->qmi.clients_lock);
			delnotify->func(dev, cid, delnotify->data);
			spin_lock(&dev->qmi.clients_lock);
		}
		kfree(delnotify);
		return true;
	}

	VDBG("no one to notify for TID %x\n", tid);
	return false;
}

static bool client_addurb(struct qcusbnet *dev, u16 cid, struct urb *urb)
{
	struct client *client;
	struct urbreq *req;

	assert_locked(dev);

	client = client_bycid(dev, cid);
	if (!client) {
		DBG("Could not find this client's memory 0x%04X\n", cid);
		return false;
	}

	req = kmalloc(sizeof(*req), GFP_ATOMIC);
	if (!req) {
		DBG("Mem error\n");
		return false;
	}

	req->urb = urb;
	list_add_tail(&req->node, &client->urbs);

	return true;
}

static struct urb *client_delurb(struct qcusbnet *dev, u16 cid)
{
	struct client *client;
	struct urbreq *req;
	struct urb *urb;

	assert_locked(dev);

	client = client_bycid(dev, cid);
	if (!client) {
		DBG("Could not find this client's memory 0x%04X\n", cid);
		return NULL;
	}

	if (list_empty(&client->urbs)) {
		DBG("No URB's to pop\n");
		return NULL;
	}

	req = list_first_entry(&client->urbs, struct urbreq, node);
	list_del(&req->node);
	urb = req->urb;
	kfree(req);
	return urb;
}

static int devqmi_open(struct inode *inode, struct file *file)
{
	struct qmihandle * handle;
	struct qmidev *qmidev = container_of(inode->i_cdev, struct qmidev, cdev);
	struct qcusbnet * dev = container_of(qmidev, struct qcusbnet, qmi);

	if (!device_valid(dev)) {
		DBG("Invalid device\n");
		return -ENXIO;
	}

	file->private_data = kmalloc(sizeof(struct qmihandle), GFP_KERNEL);
	if (!file->private_data) {
		DBG("Mem error\n");
		return -ENOMEM;
	}

	handle = (struct qmihandle *)file->private_data;
#ifdef QMUX_IN_DRIVER
	handle->cid = (u16)-1;
#else
	handle->cid = qmidev->qmiidx;

#endif /* QMUX_IN_DRIVER */
	handle->dev = dev;

	return 0;
}

static long devqmi_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int result;
	u32 vidpid;

	struct qmihandle *handle = (struct qmihandle *)file->private_data;

	if (!handle) {
		DBG("Bad file data\n");
		return -EBADF;
	}

	if (!device_valid(handle->dev)) {
		DBG("Invalid device! Updating f_ops\n");
		file->f_op = file->f_dentry->d_inode->i_fop;
		return -ENXIO;
	}

	switch (cmd) {
		case IOCTL_QMI_GET_SERVICE_FILE:

			VDBG("Setting up QMI for service %lu\n", arg);
			if (!(u8)arg) {
				DBG("Cannot use QMICTL from userspace\n");
				return -EINVAL;
			}

			if (handle->cid != (u16)-1) {
				DBG("Close the current connection before opening a new one\n");
				return -EBADR;
			}

			result = client_alloc(handle->dev, (u8)arg);
			if (result < 0)
				return result;
			handle->cid = result;

			return 0;
			break;


		case IOCTL_QMI_GET_DEVICE_VIDPID:
			if (!arg) {
				DBG("Bad VIDPID buffer\n");
				return -EINVAL;
			}

			if (!handle->dev->usbnet) {
				DBG("Bad usbnet\n");
				return -ENOMEM;
			}

			if (!handle->dev->usbnet->udev) {
				DBG("Bad udev\n");
				return -ENOMEM;
			}

			vidpid = ((le16_to_cpu(handle->dev->usbnet->udev->descriptor.idVendor) << 16)
			       + le16_to_cpu(handle->dev->usbnet->udev->descriptor.idProduct));

			result = copy_to_user((unsigned int *)arg, &vidpid, 4);
			if (result)
				DBG("Copy to userspace failure\n");

			return result;
			break;

		case IOCTL_QMI_GET_DEVICE_MEID:
			if (!arg) {
				DBG("Bad MEID buffer\n");
				return -EINVAL;
			}

			result = copy_to_user((unsigned int *)arg, &handle->dev->meid[0], 14);
			if (result)
				DBG("copy to userspace failure\n");

			return result;
			break;
		default:
			return -EBADRQC;
	}
}

static int devqmi_close(struct file *file, fl_owner_t ftable)
{
	struct qmihandle * handle = (struct qmihandle *)file->private_data;
	struct list_head * tasks;
	struct task_struct * task;
	struct fdtable * fdtable;
	int count = 0;
	int used = 0;
	unsigned long flags;

	if (!handle) {
		DBG("bad file data\n");
		return -EBADF;
	}

	if (file_count(file) != 1) {
		/* XXX: This can't possibly be safe. We don't hold any sort of
		 * lock here, and we're walking a list of threads... */
		list_for_each(tasks, &current->group_leader->tasks) {
			task = container_of(tasks, struct task_struct, tasks);
			if (!task || !task->files)
				continue;
			spin_lock_irqsave(&task->files->file_lock, flags);
			fdtable = files_fdtable(task->files);
			for (count = 0; count < fdtable->max_fds; count++) {
				/* Before this function was called, this file was removed
				 * from our task's file table so if we find it in a file
				 * table then it is being used by another task
				 */
				if (fdtable->fd[count] == file) {
					used++;
					break;
				}
			}
			spin_unlock_irqrestore(&task->files->file_lock, flags);
		}

		if (used > 0) {
			DBG("not closing, as this FD is open by %d other process\n", used);
			return 0;
		}
	}

	if (!device_valid(handle->dev)) {
		DBG("Invalid device! Updating f_ops\n");
		file->f_op = file->f_dentry->d_inode->i_fop;
		return -ENXIO;
	}

	VDBG("0x%04X\n", handle->cid);

	file->private_data = NULL;
#ifdef QMUX_IN_DRIVER
	if (handle->cid != (u16)-1)
		client_free(handle->dev, handle->cid);
#else
	handle->cid = -1;
#endif

	kfree(handle);
	return 0;
}

static ssize_t devqmi_read(struct file *file, char __user *buf, size_t size,
                           loff_t *pos)
{
	int result;
	void * data = NULL;
	void * smalldata;
	struct qmihandle * handle = (struct qmihandle *)file->private_data;

	if (!handle) {
		DBG("Bad file data\n");
		return -EBADF;
	}

	if (!device_valid(handle->dev)) {
		DBG("Invalid device! Updating f_ops\n");
		file->f_op = file->f_dentry->d_inode->i_fop;
		return -ENXIO;
	}

	if (handle->cid == (u16)-1) {
		DBG("Client ID must be set before reading 0x%04X\n",
		    handle->cid);
		return -EBADR;
	}

	result = read_sync(handle->dev, &data, handle->cid, 0);
	if (result <= 0)
		return result;
	
#ifdef QMUX_IN_DRIVER
	result -= qmux_size;
	smalldata = data + qmux_size;
#else
	smalldata = data;
#endif

	if (result > size) {
		DBG("Read data is too large for amount user has requested\n");
		kfree(data);
		return -EOVERFLOW;
	}

	if (copy_to_user(buf, smalldata, result)) {
		DBG("Error copying read data to user\n");
		result = -EFAULT;
	}

	kfree(data);
	return result;
}

static ssize_t devqmi_write (struct file *file, const char __user * buf,
                             size_t size, loff_t *pos)
{
	int status;
	void *wbuf;
	struct qmihandle *handle = (struct qmihandle *)file->private_data;

	if (!handle) {
		DBG("Bad file data\n");
		return -EBADF;
	}

	if (!device_valid(handle->dev)) {
		DBG("Invalid device! Updating f_ops\n");
		file->f_op = file->f_dentry->d_inode->i_fop;
		return -ENXIO;
	}

	if (handle->cid == (u16)-1) {
		DBG("Client ID must be set before writing 0x%04X\n",
			  handle->cid);
		return -EBADR;
	}

#ifdef QMUX_IN_DRIVER
	wbuf = kmalloc(size + qmux_size, GFP_KERNEL);
#else
	wbuf = kmalloc(size, GFP_KERNEL);
#endif
	if (!wbuf)
		return -ENOMEM;
#ifdef QMUX_IN_DRIVER
	status = copy_from_user(wbuf + qmux_size, buf, size);
#else
	status = copy_from_user(wbuf, buf, size);
#endif

	if (status) {
		DBG("Unable to copy data from userspace %d\n", status);
		kfree(wbuf);
		return status;
	}

#ifdef QMUX_IN_DRIVER
	status = write_sync(handle->dev, wbuf, size + qmux_size, handle->cid);
#else
	status = write_sync(handle->dev, wbuf, size, handle->cid);
#endif
	kfree(wbuf);
#ifdef QMUX_IN_DRIVER
	if (status == size + qmux_size)
#else
	if (status == size)
#endif
		return size;
	return status;
}

int qc_register(struct qcusbnet *dev)
{
	int result;
	int qmiidx = 0;
	dev_t devno;
	char * name;

	dev->valid = true;
#ifdef QMUX_IN_DRIVER
	result = client_alloc(dev, QMICTL);
	if (result) {
		dev->valid = false;
		return result;
	}
	atomic_set(&dev->qmi.qmitid, 1);
#endif

	result = qc_startread(dev);
	if (result) {
		dev->valid = false;
		return result;
	}

#ifdef QMUX_IN_DRIVER
	if (!qmi_ready(dev, 30000)) {
		DBG("Device unresponsive to QMI\n");
		return -ETIMEDOUT;
	}

	result = setup_wds_callback(dev);
	if (result) {
		dev->valid = false;
		return result;
	}

	result = qmidms_getmeid(dev);
	if (result) {
		dev->valid = false;
		return result;
	}
#else /* QMUX_IN_DRIVER */
	DBG("Initial Net device link is connected\n");
	qc_cleardown(dev, DOWN_NO_NDIS_CONNECTION);
#endif /* QMUX_IN_DRIVER */

	result = alloc_chrdev_region(&devno, 0, 1, "motqmi");
	if (result < 0)
		return result;

	cdev_init(&dev->qmi.cdev, &devqmi_fops);
	dev->qmi.cdev.owner = THIS_MODULE;
	dev->qmi.cdev.ops = &devqmi_fops;

	result = cdev_add(&dev->qmi.cdev, devno, 1);
	if (result) {
		DBG("error adding cdev\n");
		return result;
	}

	name = strstr(dev->usbnet->net->name, "qmi");
	if (!name) {
		DBG("Bad net name: %s\n", dev->usbnet->net->name);
		return -ENXIO;
	}
	name += strlen("qmi");
	qmiidx = simple_strtoul(name, NULL, 10);
	if (qmiidx < 0) {
		DBG("Bad minor number\n");
		return -ENXIO;
	}

	printk(KERN_INFO "creating motqmi%d\n", qmiidx);
	device_create(dev->qmi.devclass, NULL, devno, NULL, "motqmi%d", qmiidx);

	dev->qmi.devnum = devno;
#ifndef QMUX_IN_DRIVER
	dev->qmi.qmiidx = qmiidx;
	client_alloc(dev, qmiidx);
#endif
	return 0;
}

void qc_deregister(struct qcusbnet *dev)
{
	struct list_head *node;
#ifndef QMUX_IN_DRIVER
	struct list_head *tmp;
#endif
	struct client *client;
	struct inode * inode;
	struct list_head * inodes;
	struct list_head * tasks;
	struct task_struct * task;
	struct fdtable * fdtable;
	struct file * file;
	unsigned long flags;
	int count = 0;

	if (!device_valid(dev)) {
		DBG("wrong device\n");
		return;
	}

#ifdef QMUX_IN_DRIVER
	list_for_each(node, &dev->qmi.clients) {
#else
	list_for_each_safe(node, tmp, &dev->qmi.clients) {
#endif
		client = list_entry(node, struct client, node);
		VDBG("release 0x%04X\n", client->cid);
		client_free(dev, client->cid);
	}

	qc_stopread(dev);
	dev->valid = false;
	list_for_each(inodes, &dev->qmi.cdev.list) {
		inode = container_of(inodes, struct inode, i_devices);
		if (inode != NULL && !IS_ERR(inode)) {
			list_for_each(tasks, &current->group_leader->tasks) {
				task = container_of(tasks, struct task_struct, tasks);
				if (!task || !task->files)
					continue;
				spin_lock_irqsave(&task->files->file_lock, flags);
				fdtable = files_fdtable(task->files);
				for (count = 0; count < fdtable->max_fds; count++) {
					file = fdtable->fd[count];
					if (file != NULL &&  file->f_dentry != NULL) {
						if (file->f_dentry->d_inode == inode) {
							rcu_assign_pointer(fdtable->fd[count], NULL);
							spin_unlock_irqrestore(&task->files->file_lock, flags);
							DBG("forcing close of open file handle\n");
							filp_close(file, task->files);
							spin_lock_irqsave(&task->files->file_lock, flags);
						}
					}
				}
				spin_unlock_irqrestore(&task->files->file_lock, flags);
			}
		}
	}

	if (!IS_ERR(dev->qmi.devclass))
		device_destroy(dev->qmi.devclass, dev->qmi.devnum);
	cdev_del(&dev->qmi.cdev);
	unregister_chrdev_region(dev->qmi.devnum, 1);
}

#ifdef QMUX_IN_DRIVER
static bool qmi_ready(struct qcusbnet *dev, u16 timeout)
{
	int result;
	void * wbuf;
	size_t wbufsize;
	void * rbuf;
	u16 rbufsize;
	struct semaphore sem;
	u16 now;
	unsigned long flags;
	u8 tid;

	if (!device_valid(dev)) {
		DBG("Invalid device\n");
		return -EFAULT;
	}


	for (now = 0; now < timeout; now += 100) {
		sema_init(&sem, 0);

		tid = atomic_add_return(1, &dev->qmi.qmitid);
		if (!tid)
			tid = atomic_add_return(1, &dev->qmi.qmitid);
		if (wbuf)
			kfree(wbuf);
		wbuf = qmictl_new_ready(tid, &wbufsize);
		if (!wbuf)
			return -ENOMEM;

		result = read_async(dev, QMICTL, tid, upsem, &sem);
		if (result) {
			kfree(wbuf);
			return false;
		}

		write_sync(dev, wbuf, wbufsize, QMICTL);

		msleep(100);
		if (!down_trylock(&sem)) {
			spin_lock_irqsave(&dev->qmi.clients_lock, flags);
			if (client_delread(dev,	QMICTL,	tid, &rbuf, &rbufsize)) {
				spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
				kfree(rbuf);
				break;
			}
		} else {
			spin_lock_irqsave(&dev->qmi.clients_lock, flags);
			client_notify(dev, QMICTL, tid);
			spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
		}
	}

	if (wbuf)
		kfree(wbuf);

	if (now >= timeout)
		return false;

	VDBG("QMI Ready after %u milliseconds\n", now);

	/* 3580 and newer doesn't need a delay; older needs 5000ms */
	if (qcusbnet2k_fwdelay)
		msleep(qcusbnet2k_fwdelay * 1000);

	return true;
}

static void wds_callback(struct qcusbnet *dev, u16 cid, void *data)
{
	bool ret;
	int result;
	void * rbuf;
	u16 rbufsize;

	struct net_device_stats * stats = &(dev->usbnet->net->stats);

	struct qmiwds_stats dstats = {
		.txok = (u32)-1,
		.rxok = (u32)-1,
		.txerr = (u32)-1,
		.rxerr = (u32)-1,
		.txofl = (u32)-1,
		.rxofl = (u32)-1,
		.txbytesok = (u64)-1,
		.rxbytesok = (u64)-1,
	};
	unsigned long flags;

	if (!device_valid(dev)) {
		DBG("Invalid device\n");
		return;
	}

	spin_lock_irqsave(&dev->qmi.clients_lock, flags);
	ret = client_delread(dev, cid, 0, &rbuf, &rbufsize);
	spin_unlock_irqrestore(&dev->qmi.clients_lock, flags); 

	if (!ret) {
		DBG("WDS callback failed to get data\n");
		return;
	}

	dstats.linkstate = !qc_isdown(dev, DOWN_NO_NDIS_CONNECTION);
	dstats.reconfigure = false;

	result = qmiwds_event_resp(rbuf, rbufsize, &dstats);
	if (result < 0) {
		DBG("bad WDS packet\n");
	} else {
		if (dstats.txofl != (u32)-1)
			stats->tx_fifo_errors = dstats.txofl;

		if (dstats.rxofl != (u32)-1)
			stats->rx_fifo_errors = dstats.rxofl;

		if (dstats.txerr != (u32)-1)
			stats->tx_errors = dstats.txerr;

		if (dstats.rxerr != (u32)-1)
			stats->rx_errors = dstats.rxerr;

		if (dstats.txok != (u32)-1)
			stats->tx_packets = dstats.txok + stats->tx_errors;

		if (dstats.rxok != (u32)-1)
			stats->rx_packets = dstats.rxok + stats->rx_errors;

		if (dstats.txbytesok != (u64)-1)
			stats->tx_bytes = dstats.txbytesok;

		if (dstats.rxbytesok != (u64)-1)
			stats->rx_bytes = dstats.rxbytesok;

		if (dstats.reconfigure) {
			DBG("Net device link reset\n");
			qc_setdown(dev, DOWN_NO_NDIS_CONNECTION);
			qc_cleardown(dev, DOWN_NO_NDIS_CONNECTION);
		} else {
			if (dstats.linkstate) {
				DBG("Net device link is connected\n");
				qc_cleardown(dev, DOWN_NO_NDIS_CONNECTION);
			} else {
				DBG("Net device link is disconnected\n");
				qc_setdown(dev, DOWN_NO_NDIS_CONNECTION);
			}
		}
	}

	kfree(rbuf);

	result = read_async(dev, cid, 0, wds_callback, data);
	if (result != 0)
		DBG("unable to setup next async read\n");
}

static int setup_wds_callback(struct qcusbnet * dev)
{
	int result;
	void *buf;
	size_t size;
	u16 cid;

	if (!device_valid(dev)) {
		DBG("Invalid device\n");
		return -EFAULT;
	}

	result = client_alloc(dev, QMIWDS);
	if (result < 0)
		return result;
	cid = result;

	buf = qmiwds_new_seteventreport(1, &size);
	if (!buf)
		return -ENOMEM;

	result = write_sync(dev, buf, size, cid);
	kfree(buf);

	if (result < 0) {
		return result;
	}

	buf = qmiwds_new_getpkgsrvcstatus(2, &size);
	if (buf == NULL)
		return -ENOMEM;

	result = write_sync(dev, buf, size, cid);
	kfree(buf);

	if (result < 0)
		return result;

	result = read_async(dev, cid, 0, wds_callback, NULL);
	if (result) {
		DBG("unable to setup async read\n");
		return result;
	}

/*
	result = usb_control_msg(dev->usbnet->udev,
	                         usb_sndctrlpipe(dev->usbnet->udev, 0),
	                         0x22, 0x21, 1, 0, NULL, 0, 100);
	if (result < 0) {
		DBG("Bad SetControlLineState status %d\n", result);
		return result;
	}
*/
	return 0;
}

static int qmidms_getmeid(struct qcusbnet * dev)
{
	int result;
	void * wbuf;
	size_t wbufsize;
	void * rbuf;
	u16 rbufsize;
	u16 cid;

	if (!device_valid(dev))	{
		DBG("Invalid device\n");
		return -EFAULT;
	}

	result = client_alloc(dev, QMIDMS);
	if (result < 0)
		return result;
	cid = result;

	wbuf = qmidms_new_getmeid(1, &wbufsize);
	if (!wbuf)
		return -ENOMEM;

	result = write_sync(dev, wbuf, wbufsize, cid);
	kfree(wbuf);

	if (result < 0)
		return result;

	result = read_sync(dev, &rbuf, cid, 1);
	if (result < 0)
		return result;
	rbufsize = result;

	result = qmidms_meid_resp(rbuf, rbufsize, &dev->meid[0], 14);
	kfree(rbuf);

	if (result < 0) {
		DBG("bad get MEID resp\n");
		memset(&dev->meid[0], '0', 14);
	}

	client_free(dev, cid);
	return 0;
}
#endif

module_param(qcusbnet2k_fwdelay, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(qcusbnet2k_fwdelay, "Delay for old firmware");
