/* drivers/usb/function/mtp.c
 *
 * Function Device for the MTP Protocol
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/list.h>
#include <asm/atomic.h>
#include <asm/uaccess.h>

#include <linux/usb/ch9.h>
#include <linux/usb/composite.h>
#include <linux/usb/gadget.h>
#include <linux/usb/android_composite.h>

#include "f_mot_android.h"

#define mtp_err(fmt, arg...)	printk(KERN_ERR "%s(): " fmt, __func__, ##arg)
#ifdef DEBUG
#define mtp_debug(fmt, arg...)	printk(KERN_DEBUG "%s(): " fmt, __func__, ##arg)
#else
#define mtp_debug(fmt, arg...)
#endif

/* number/size of rx and tx requests to allocate */
#define TXN_MAX    8192
#define RX_REQ_MAX 4
#define TX_REQ_MAX 4

#define MTP_FUNCTION_NAME  "mtp"
#define MTP_INTERFACE_NAME "Motorola MTP Device"


/* MTP event codes, including class-specific requests (per PIMA 15740) */
#define  MTP_EVENT_DISCONNECTED           0x01
#define  MTP_EVENT_CONNECTED              0x02
#define  MTP_EVENT_EJECT                  0x03
#define  MTP_EVENT_READ_ERROR             0x04
#define  MTP_EVENT_READ_RELEASED          0x05
#define  MTP_EVENT_CSR_CANCEL             0x64
#define  MTP_EVENT_CSR_GET_EVENT          0x65
#define  MTP_EVENT_CSR_DEVICE_RESET       0x66
#define  MTP_EVENT_CSR_GET_DEVICE_STATUS  0x67

/* MTP Driver command codes */
#define  MTP_COMMAND_DISCONNECT 0
#define  MTP_COMMAND_CONNECT    1
#define  MTP_COMMAND_CSR_REPLY  2
#define  MTP_COMMAND_RESET      3
#define  MTP_COMMAND_DISABLE    4

/* MTP Driver connection states and macros */
#define  MTP_STATE_DISCONNECTED 0
#define  MTP_STATE_CONNECTING   1
#define  MTP_STATE_CONNECTED    2
#define  MTP_STATE_RESET        3

#define  MTP_DISCONNECTED (_context.connection_state == MTP_STATE_DISCONNECTED)
#define  MTP_CONNECTED    (_context.connection_state == MTP_STATE_CONNECTED)
#define  MTP_CONNECTING   (_context.connection_state == MTP_STATE_CONNECTING)
#define  MTP_RESET        (_context.connection_state == MTP_STATE_RESET)

/* PIMA15740 MTP CSR response codes */
#define  MTP_CSR_CODE_OK        0x2001
#define  MTP_CSR_CODE_BUSY      0x2019
#define  MTP_CSR_CODE_CANCELLED 0x201F

#define EVENTBUF_SIZE 50

struct mtp_context {
	struct usb_function function;
	struct usb_composite_dev *cdev;
	int connection_state;
	int read_error;
	int write_error;
	unsigned bound;

	atomic_t read_excl;
	atomic_t write_excl;
	atomic_t open_excl;
	atomic_t control_excl;
	atomic_t event_excl;
	atomic_t eventread_excl;

	spinlock_t lock;

	struct usb_ep *out;
	struct usb_ep *in;

	struct list_head tx_idle;
	struct list_head rx_idle;
	struct list_head rx_done;

	wait_queue_head_t read_wq;
	wait_queue_head_t write_wq;
	wait_queue_head_t event_wq;

	/* For managing userspace bulk endpoint reads */
	struct usb_request *read_req;
	unsigned char *read_buf;
	unsigned char *user_buf;
	unsigned char *user_bufptr;
	unsigned read_count;

	struct usb_request *ctl_tx_req;

	/* For managing MTP driver - userspace interaction */
	int releasing;		/* The MTP driver is terminating blocking userspace reads */
	int events_queued;	/* If events are waiting to be read */
	unsigned char event_buf[EVENTBUF_SIZE];
	int event_buf_index;

	/* For managing Class Specific Requests */
	int csr_resp_ready;
	unsigned char csr_buf[4];
};

static struct mtp_context _context;

struct ex_compat_id_descriptor_header {
	int dwLength;		/* Length of the complete descriptor  */
	u16 bcdVersion;		/* Version number in BCD format       */
	u16 wIndex;		/* OS Feature Descriptor identifier   */
	u8 bCount;		/* Number of custom property sections */
	u8 reserved[7];
};

struct ex_compat_id_descriptor_function {
	u8 bFirstInterfaceNumber;
	u8 reserved1;
	u8 compatibleID[8];
	u8 subcompatibleID[8];
	u8 reserved2[6];
};

struct ex_compat_id_descriptor {
	struct ex_compat_id_descriptor_header hdr;
	struct ex_compat_id_descriptor_function func;
};

static struct ex_compat_id_descriptor _ex_compat_id_descriptor = {
	{			/* Header */
	 __constant_cpu_to_le32(40),	/* Total Length               */
	 __constant_cpu_to_le16(0x0100),	/* BCD Version (1.00)         */
	 __constant_cpu_to_le16(0x0004),	/* Extended compat ID         */
	 0x01,			/* Number functions (1 - MTP) */
	 {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}	/* Reserved                   */
	 },
	{			/* MTP Function Section */
	 0x00,			/* First interface number */
	 0x01,			/* Reserved - set to 0x01 */
	 {'M', 'T', 'P', 0, 0, 0, 0, 0},	/* Compatible ID          */
	 {0, 0, 0, 0, 0, 0, 0, 0},	/* Subcompatible ID       */
	 {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}	/* Reserved               */
	 }
};

#define STRING_INTERFACE        0
#define STRING_MTP              1

static int mtp_ext_str_idx = 238;

/* static strings, in UTF-8 */
static struct usb_string mtp_string_defs[] = {
	[STRING_INTERFACE].s = "Motorola MTP Interface",
	[STRING_MTP].s = "MSFT100\034",
	{ /* ZEROES END LIST */ },
};

static struct usb_gadget_strings mtp_string_table = {
	.language = 0x0409,	/* en-us */
	.strings = mtp_string_defs,
};

static struct usb_gadget_strings *mtp_strings[] = {
	&mtp_string_table,
	NULL,
};

/* There is only one interface. */
static struct usb_interface_descriptor intf_desc = {
	.bLength = sizeof intf_desc,
	.bDescriptorType = USB_DT_INTERFACE,
	.bInterfaceNumber = 0,
	.bNumEndpoints = 3,
	.bInterfaceClass = 0x06,
	.bInterfaceSubClass = 0x01,
	.bInterfaceProtocol = 0x01,
};

static struct usb_endpoint_descriptor fs_bulk_in_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = USB_DIR_IN,
	.bmAttributes = USB_ENDPOINT_XFER_BULK,
};

static struct usb_endpoint_descriptor fs_bulk_out_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = USB_DIR_OUT,
	.bmAttributes = USB_ENDPOINT_XFER_BULK,
};

static struct usb_endpoint_descriptor fs_intr_in_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = USB_DIR_IN,
	.bmAttributes = USB_ENDPOINT_XFER_INT,
	.wMaxPacketSize = __constant_cpu_to_le16(64),
	.bInterval = 10,
};

static struct usb_descriptor_header *fs_mtp_descs[] = {
	(struct usb_descriptor_header *) &intf_desc,
	(struct usb_descriptor_header *) &fs_bulk_out_desc,
	(struct usb_descriptor_header *) &fs_bulk_in_desc,
	(struct usb_descriptor_header *) &fs_intr_in_desc,
	NULL,
};

static struct usb_endpoint_descriptor hs_bulk_in_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bmAttributes = USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize = __constant_cpu_to_le16(512),
	.bInterval = 0,
};

static struct usb_endpoint_descriptor hs_bulk_out_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bmAttributes = USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize = __constant_cpu_to_le16(512),
	.bInterval = 0,
};

static struct usb_endpoint_descriptor hs_intr_in_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bmAttributes = USB_ENDPOINT_XFER_INT,
	.wMaxPacketSize = __constant_cpu_to_le16(64),
	.bInterval = 10,
};

static struct usb_descriptor_header *hs_mtp_descs[] = {
	(struct usb_descriptor_header *) &intf_desc,
	(struct usb_descriptor_header *) &hs_bulk_out_desc,
	(struct usb_descriptor_header *) &hs_bulk_in_desc,
	(struct usb_descriptor_header *) &hs_intr_in_desc,
	NULL,
};

struct mtp_cancel_req {
	u16 CancellationCode;	/* Cancellation code  */
	u32 TransactionID;	/* Cancelled transaction ID */
} __attribute__ ((packed));



static inline int add_event(unsigned char code, unsigned char *buf,
			    int len)
{
	struct mtp_context *ctxt = &_context;

	/* Prevent overflow of the event buffer */
	if ((ctxt->event_buf_index + len + 2) > EVENTBUF_SIZE)
		return 0;

	mtp_debug("mtp add_event: code=%d\n", code);

	/* Make sure to set state and wake up bulk read activity (/dev/mtp) for certain events */
	switch (code) {
	case MTP_EVENT_READ_RELEASED:
		if (MTP_CONNECTED) {
			/* Only release if connected */
			ctxt->releasing = 1;
			wake_up(&(_context.read_wq));
		}
		break;

	case MTP_EVENT_READ_ERROR:
		ctxt->read_error = 1;
		wake_up(&(_context.read_wq));
		break;

	case MTP_EVENT_CONNECTED:
	case MTP_EVENT_DISCONNECTED:
		wake_up(&(_context.read_wq));
		break;
	}

	/* Copy (total) event length and code into event buffer */
	ctxt->event_buf[ctxt->event_buf_index++] = code;
	ctxt->event_buf[ctxt->event_buf_index++] = len + 2;

	/* If a buffer was supplied, copy to event buffer and advance event buffer index */
	if (buf) {
		memcpy((ctxt->event_buf + ctxt->event_buf_index), buf,
		       len);
		ctxt->event_buf_index += len;
	}

	/* Increment num events in buffer */
	ctxt->event_buf[0] = ++ctxt->events_queued;
	wake_up(&ctxt->event_wq);

	return 1;
}

static inline int _lock(atomic_t *excl)
{
	if (atomic_inc_return(excl) == 1) {
		return 0;
	} else {
		atomic_dec(excl);
		return -1;
	}
}

static inline void _unlock(atomic_t *excl)
{
	atomic_dec(excl);
}

static struct usb_request *req_new(struct usb_ep *ep, int size)
{
	struct usb_request *req = usb_ep_alloc_request(ep, GFP_KERNEL);
	if (!req)
		return NULL;

	/* now allocate buffers for the requests */
	req->buf = kmalloc(size, GFP_KERNEL);
	if (!req->buf) {
		usb_ep_free_request(ep, req);
		return NULL;
	}

	return req;
}

static void req_free(struct usb_request *req, struct usb_ep *ep)
{
	if (req) {
		kfree(req->buf);
		usb_ep_free_request(ep, req);
	}
}

/* add a request to the tail of a list */
void mtp_req_put(struct mtp_context *ctxt, struct list_head *head,
		 struct usb_request *req)
{
	unsigned long flags;

	spin_lock_irqsave(&ctxt->lock, flags);
	list_add_tail(&req->list, head);
	spin_unlock_irqrestore(&ctxt->lock, flags);
}


/* remove a request from the head of a list */
struct usb_request *mtp_req_get(struct mtp_context *ctxt,
				struct list_head *head)
{
	unsigned long flags;
	struct usb_request *req;

	spin_lock_irqsave(&ctxt->lock, flags);
	if (list_empty(head)) {
		req = 0;
	} else {
		req = list_first_entry(head, struct usb_request, list);
		list_del(&req->list);
	}
	spin_unlock_irqrestore(&ctxt->lock, flags);
	return req;
}


static void mtp_complete_in(struct usb_ep *ept, struct usb_request *req)
{
	struct mtp_context *ctxt = req->context;

	if (!MTP_RESET && (req->status != 0))
		ctxt->write_error = 1;

	if ((req->length >= ept->maxpacket)
	    && ((req->length % ept->maxpacket) == 0)) {
		/* Queue zero length packet */
		req->length = 0;
		usb_ep_queue(ctxt->in, req, GFP_ATOMIC);
		return;
	}
	mtp_req_put(ctxt, &ctxt->tx_idle, req);

	wake_up(&ctxt->write_wq);
}


static void mtp_complete_out(struct usb_ep *ept, struct usb_request *req)
{
	struct mtp_context *ctxt = req->context;

	if (MTP_RESET) {
		/* While handling reset, place completing requests back to the idle queue */
		mtp_req_put(ctxt, &ctxt->rx_idle, req);
	} else if (req->status != 0) {
		mtp_debug("mtp_complete_out: non zero status \n");
		add_event(MTP_EVENT_READ_ERROR, NULL, 0);
		mtp_req_put(ctxt, &ctxt->rx_idle, req);
	} else {
		mtp_req_put(ctxt, &ctxt->rx_done, req);
	}

	wake_up(&ctxt->read_wq);
}


/*
 * Operations for the mtp driver: Interface for bulk IN/OUT data transfer to userspace
 */

static ssize_t mtp_read(struct file *fp, char __user buf, size_t count,
			loff_t pos)
{
	struct mtp_context *ctxt = &_context;
	struct usb_request *req;
	int r = 0, xfer;
	int ret;
	int copying = 1;

	mtp_debug("mtp_read: enter\n");

	if (_lock(&ctxt->read_excl)) {
		mtp_debug("mtp_read: exit with EBUSY\n");
		return -EBUSY;
	}

	/* Save the user buffer pointer for handling reset operations */
	ctxt->user_buf = buf;
	ctxt->user_bufptr = buf;

	/* we will block until connected or a read event occurs */
	while (!(MTP_CONNECTED || ctxt->read_error || ctxt->releasing)) {
		mtp_debug
		    ("mtp_read: waiting for connected state or read event\n");
		ret =
		    wait_event_interruptible(ctxt->read_wq,
					     (MTP_CONNECTED
					      || ctxt->read_error
					      || ctxt->releasing));
		if (ret < 0) {
			_unlock(&ctxt->read_excl);
			return ret;
		}
	}

	while (copying) {
		if (ctxt->releasing) {
			ctxt->releasing = 0;

			/* Don't exit/release if handling reset */
			if (!MTP_RESET) {
				mtp_debug("mtp_read: releasing\n");
				r = -EIO;
			}
			break;
		} else if (!MTP_RESET && ctxt->read_error) {
			mtp_debug("mtp_read: exit with EIO\n");
			r = -EIO;
			break;
		}

		/* Block during reset handling; queue idle requests afterwards */
		while (MTP_RESET) {
			mtp_debug
			    ("mtp_read: waiting for reset handling to complete\n");
			ret =
			    wait_event_interruptible(ctxt->read_wq,
						     !MTP_RESET);
			if (ret < 0) {
				_unlock(&ctxt->read_excl);
				return ret;
			}
		}

		/* if we have idle read requests, get them queued */
		while (!MTP_RESET
		       && (req = mtp_req_get(ctxt, &ctxt->rx_idle))) {
			req->length = TXN_MAX;
			ret = usb_ep_queue(ctxt->out, req, GFP_ATOMIC);
			if (!MTP_RESET && (ret < 0)) {
				r = -EIO;
				mtp_debug
				    ("mtp_read: exit for failed usb_ept_queue_xfer\n");
				add_event(MTP_EVENT_READ_ERROR, NULL, 0);
				mtp_req_put(ctxt, &ctxt->rx_idle, req);
				goto fail;
			}
		}

		/* if we have data pending, give it to userspace */
		if (ctxt->read_count > 0) {
			xfer =
			    (ctxt->read_count <
			     count) ? ctxt->read_count : count;

			if (copy_to_user
			    (ctxt->user_bufptr, ctxt->read_buf, xfer)) {
				if (!MTP_RESET) {
					mtp_debug
					    ("mtp_read: exit for failed copy_to_user\n");
					add_event(MTP_EVENT_READ_ERROR,
						  NULL, 0);
					r = -EFAULT;
					break;
				}
			}

			/* Don't update variables during reset handling */
			if (!MTP_RESET) {
				ctxt->read_buf += xfer;
				ctxt->read_count -= xfer;
				ctxt->user_bufptr += xfer;
				count -= xfer;
				r += xfer;
			}

			/* if we've emptied the buffer, release the request */
			if (ctxt->read_count == 0) {
				mtp_req_put(ctxt, &ctxt->rx_idle,
					    ctxt->read_req);
				if (!MTP_RESET
				    && ((ctxt->read_req)->actual <
					TXN_MAX)) {
					/* The transfer is complete */
					copying = 0;
					ctxt->read_req = 0;
					break;
					/* Note - this leaves a request in the rx_idle queue.
					   there should be other submitted request on which to rely. */
				}
				ctxt->read_req = 0;
			} else if (!MTP_RESET && (count == 0)) {
				/* The user buffer is full; return */
				copying = 0;
				break;
			}
			continue;
		}

		/* wait for a request to complete */
		req = 0;
		ret = wait_event_interruptible(ctxt->read_wq,
					       ((req =
						 mtp_req_get(ctxt,
							     &ctxt->
							     rx_done))
						|| ctxt->read_error
						|| ctxt->releasing));

		if (ctxt->releasing) {
			ctxt->releasing = 0;

			if (!MTP_RESET) {
				mtp_debug
				    ("mtp_read: releasing after waiting for request complete)\n");
				r = -EIO;
				break;
			}
		} else if (req != 0) {
			/* if we got a 0-len one we need to put it back into
			 ** service and return; the tranfer is complete.
			 */
			if (req->actual == 0) {
				mtp_req_put(ctxt, &ctxt->rx_idle, req);
				if (!MTP_RESET)
					copying = 0;
			} else if (!MTP_RESET) {
				ctxt->read_req = req;
				ctxt->read_count = req->actual;
				ctxt->read_buf = req->buf;
			}
		}

		if (!MTP_RESET && (ret < 0)) {
			r = ret;
			break;
		}
	}

fail:
	_unlock(&ctxt->read_excl);
	mtp_debug("mtp_read: exit with %d\n", r);
	return r;
}


static ssize_t mtp_write(struct file *fp, const char __user *buf,
			 size_t count, loff_t *pos)
{
	struct mtp_context *ctxt = &_context;
	struct usb_request *req = 0;
	int r = 0, xfer;
	int ret;

	if (_lock(&ctxt->write_excl))
		return -EBUSY;

	while (count > 0) {
		if (ctxt->write_error) {
			r = -EIO;
			break;
		}

		/* get an idle tx request to use */
		req = 0;
		ret = wait_event_interruptible(ctxt->write_wq,
					       ((req =
						 mtp_req_get(ctxt,
							     &ctxt->
							     tx_idle))
						|| ctxt->write_error));

		if (ret < 0) {
			r = ret;
			break;
		}

		if (req != 0) {
			xfer = count > TXN_MAX ? TXN_MAX : count;
			if (copy_from_user(req->buf, buf, xfer)) {
				r = -EFAULT;
				break;
			}

			req->length = xfer;
			ret = usb_ep_queue(ctxt->in, req, GFP_ATOMIC);
			if (ret < 0) {
				ctxt->write_error = 1;
				r = -EIO;
				break;
			}

			r += xfer;
			buf += xfer;
			count -= xfer;

			/* zero this so we don't try to free it on error exit */
			req = 0;
		}
	}

	if (req)
		mtp_req_put(ctxt, &ctxt->tx_idle, req);

	_unlock(&ctxt->write_excl);
	return r;
}

static int mtp_open(struct inode *ip, struct file *fp)
{
	struct mtp_context *ctxt = &_context;

	if (_lock(&ctxt->open_excl))
		return -EBUSY;

	return 0;
}

static int mtp_release(struct inode *ip, struct file *fp)
{
	struct mtp_context *ctxt = &_context;

	_unlock(&ctxt->open_excl);
	return 0;
}

static struct file_operations mtp_fops = {
	.owner = THIS_MODULE,
	.read = mtp_read,
	.write = mtp_write,
	.open = mtp_open,
	.release = mtp_release,
};

static struct miscdevice mtp_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "mtp",
	.fops = &mtp_fops,
};


/*
 * Operations for the mtp_control driver: Write=driver control  Read=Connection status
 */

static ssize_t mtp_control_read(struct file *fp, char __user *buf,
				size_t count, loff_t *pos)
{
	int ret;
	char *connectStr;

	if (*pos)
		return 0;

	if (!count)
		return -EFAULT;

	if (MTP_CONNECTED)
		connectStr = "1\n";
	else
		connectStr = "0\n";

	ret = copy_to_user(buf, connectStr, 2);

	if (ret < 0)
		return -EFAULT;

	*pos += 2;
	return 2;
}

static void mtp_function_disable(struct usb_function *f);

static ssize_t mtp_control_write(struct file *fp, const char __user *buf,
				 size_t count, loff_t *pos)
{
	u8 command[5];
	struct mtp_context *ctxt = &_context;

	/* Control writes should never exceed 5 bytes (command byte + 4 byte CSR response) */
	if (count <= 5) {
		if (copy_from_user((void *) &command, buf, count))
			return -EFAULT;
	} else {
		mtp_debug("mtp_control_write() invalid length=%d\n",
			  count);
		return -EFAULT;
	}

	mtp_debug("mtp_control_write() command=%d\n", command[0]);

	switch (command[0]) {
	case MTP_COMMAND_DISABLE:
		mtp_function_disable(&ctxt->function);
		break;

	case MTP_COMMAND_DISCONNECT:
		mtp_debug("MTP_EVENT_READ_RELEASED + MTP_EVENT_EJECT\n");

		/* Wake up any userspace read of /dev/mtp for termination */
		add_event(MTP_EVENT_READ_RELEASED, NULL, 0);

		/* This event will terminate the userspace /dev/mtp_events blocking read loop */
		add_event(MTP_EVENT_EJECT, NULL, 0);

		/* Set state to disconnected */
		ctxt->connection_state = MTP_STATE_DISCONNECTED;
		break;

	case MTP_COMMAND_CONNECT:
		/* Make sure the event queue is clear */
		ctxt->events_queued = 0;
		ctxt->event_buf_index = 1;

		/* Switch to a configuration with the MTP interface */
		ctxt->connection_state = MTP_STATE_CONNECTING;
		break;

	case MTP_COMMAND_CSR_REPLY:
		if (ctxt->csr_resp_ready) {
			/* The previous response was not returned to the PC yet - log error. */
			mtp_debug
			    ("mtp_control_write: CSR response received when one already queued.\n");
		} else {
			/* Copy the response into the MTP context and note its availability. */
			ctxt->csr_resp_ready = 1;
			memcpy(ctxt->csr_buf, &(command[1]), 4);
			mtp_debug
			    ("mtp_control_write: csr = %02X %02X %02X %02X\n",
			     ctxt->csr_buf[0], ctxt->csr_buf[1],
			     ctxt->csr_buf[2], ctxt->csr_buf[3]);
		}

		break;

	case MTP_COMMAND_RESET:
		/* Set states to handle reset */
		ctxt->connection_state = MTP_STATE_RESET;
		ctxt->user_bufptr = ctxt->user_buf;
		ctxt->read_count = 0;
		ctxt->read_buf = 0;

		/* First halt the bulk IN/OUT endpoints */
		mtp_debug("MTP_COMMAND_RESET: stalling endpoints\n");
		break;
	}

	return count;
}


static int mtp_control_open(struct inode *ip, struct file *fp)
{
	if (_lock(&(_context.control_excl)))
		return -EBUSY;

	return 0;
}


static int mtp_control_release(struct inode *ip, struct file *fp)
{
	_unlock(&(_context.control_excl));
	return 0;
}


static struct file_operations mtp_control_fops = {
	.owner = THIS_MODULE,
	.open = mtp_control_open,
	.read = mtp_control_read,
	.write = mtp_control_write,
	.release = mtp_control_release,
};

static struct miscdevice mtp_control_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "mtp_control",
	.fops = &mtp_control_fops,
};


/*
 * Operations for the mtp_event driver: Data for events from driver to userspace
 */
static ssize_t mtp_event_read(struct file *fp, char __user *buf,
			      size_t count, loff_t *pos)
{
	int ret;
	struct mtp_context *ctxt = &_context;

	if (_lock(&ctxt->eventread_excl))
		return -EBUSY;

	ret =
	    wait_event_interruptible(ctxt->event_wq, ctxt->events_queued);
	if (ret < 0) {
		_unlock(&ctxt->eventread_excl);
		return ret;
	}

	if (count < ctxt->event_buf_index) {
		_unlock(&ctxt->eventread_excl);
		return -EFAULT;
	}

	ret = copy_to_user(buf, ctxt->event_buf, ctxt->event_buf_index);
	if (ret < 0) {
		_unlock(&ctxt->eventread_excl);
		return -EFAULT;
	} else {
		ret = ctxt->event_buf_index;	/* Return buffer length     */
		ctxt->events_queued = 0;	/* All events delivered     */
		ctxt->event_buf_index = 1;	/* Reset event buffer index */
	}

	_unlock(&ctxt->eventread_excl);
	return ret;
}


static int mtp_event_open(struct inode *ip, struct file *fp)
{
	if (_lock(&(_context.event_excl)))
		return -EBUSY;

	return 0;
}

static int mtp_event_release(struct inode *ip, struct file *fp)
{
	_unlock(&(_context.event_excl));
	return 0;
}

static struct file_operations mtp_event_fops = {
	.owner = THIS_MODULE,
	.open = mtp_event_open,
	.read = mtp_event_read,
	.release = mtp_event_release,
};

static struct miscdevice mtp_event_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "mtp_events",
	.fops = &mtp_event_fops,
};


/*
 * Functions for MTP driver registration and binding
 */

static void mtp_function_unbind(struct usb_configuration *c,
				struct usb_function *f)
{
	struct mtp_context *ctxt = &_context;
	struct usb_request *req;

	mtp_debug("mtp_unbind()\n");

	if (!ctxt->bound)
		return;

	while ((req = mtp_req_get(ctxt, &ctxt->rx_idle)))
		req_free(req, ctxt->out);

	while ((req = mtp_req_get(ctxt, &ctxt->tx_idle)))
		req_free(req, ctxt->in);

	if (ctxt->in) {
		usb_ep_fifo_flush(ctxt->in);
		usb_ep_disable(ctxt->in);
	}
	if (ctxt->out) {
		usb_ep_fifo_flush(ctxt->out);
		usb_ep_disable(ctxt->out);
	}

	/* Clear states and error indications */
	ctxt->connection_state = MTP_STATE_DISCONNECTED;
	ctxt->releasing = 0;
	ctxt->read_error = 0;
	ctxt->write_error = 0;

	/* readers may be blocked waiting for connection status change */
	wake_up(&ctxt->read_wq);
	ctxt->bound = 0;
}


static int mtp_function_bind(struct usb_configuration *c,
			     struct usb_function *f)
{
	struct mtp_context *ctxt = &_context;
	struct usb_request *req;
	int n;

	mtp_debug("mtp_function_bind()\n");

	intf_desc.bInterfaceNumber = usb_interface_id(c, f);

	ctxt->in = usb_ep_autoconfig(ctxt->cdev->gadget, &fs_bulk_in_desc);
	if (!ctxt->in) {
		printk(KERN_ERR
	       "mtp_function_bind() could not auto config in ep!\n");
		return -1;
	}
	if (ctxt->in && gadget_is_dualspeed(ctxt->cdev->gadget)) {
		hs_bulk_in_desc.bEndpointAddress =
		    fs_bulk_in_desc.bEndpointAddress;
	}

	ctxt->out =
	    usb_ep_autoconfig(ctxt->cdev->gadget, &fs_bulk_out_desc);
	if (!ctxt->out) {
		printk(KERN_ERR
	       "mtp_function_bind() could not auto config out ep!\n");
		return -1;
	}
	if (ctxt->out && gadget_is_dualspeed(ctxt->cdev->gadget)) {
		hs_bulk_out_desc.bEndpointAddress =
		    fs_bulk_out_desc.bEndpointAddress;
	}

	for (n = 0; n < RX_REQ_MAX; n++) {
		req = req_new(ctxt->out, TXN_MAX);
		if (req == 0) {
			ctxt->bound = 1;
			goto fail;
		}

		req->context = ctxt;
		req->complete = mtp_complete_out;
		mtp_req_put(ctxt, &ctxt->rx_idle, req);
	}

	for (n = 0; n < TX_REQ_MAX; n++) {
		req = req_new(ctxt->in, TXN_MAX);
		if (req == 0) {
			ctxt->bound = 1;
			goto fail;
		}
		req->context = ctxt;
		req->complete = mtp_complete_in;
		mtp_req_put(ctxt, &ctxt->tx_idle, req);
	}

	ctxt->ctl_tx_req = req_new(ctxt->cdev->gadget->ep0, 512);
	ctxt->bound = 1;
	return 0;

fail:
	printk(KERN_ERR
	       "mtp_function_bind() could not allocate requests\n");

	return -1;
}

static int mtp_function_set_alt(struct usb_function *f,
				unsigned intf, unsigned alt)
{
	struct mtp_context *ctxt = &_context;
	struct usb_request *req;
	int ret;

	mtp_debug("mtp_configure, intf=%d alt=%d\n", intf, alt);

	ret = usb_ep_enable(ctxt->in,
			    ep_choose(ctxt->cdev->gadget,
				      &hs_bulk_in_desc, &fs_bulk_in_desc));
	if (ret)
		return ret;
	ret = usb_ep_enable(ctxt->out,
			    ep_choose(ctxt->cdev->gadget,
				      &hs_bulk_out_desc,
				      &fs_bulk_out_desc));
	if (ret) {
		usb_ep_disable(ctxt->in);
		return ret;
	}

	usb_interface_enum_cb(MTP_TYPE_FLAG);


	/* if we have a stale request being read, recycle it */
	ctxt->read_buf = 0;
	ctxt->read_count = 0;
	if (ctxt->read_req) {
		mtp_req_put(ctxt, &ctxt->rx_idle, ctxt->read_req);
		ctxt->read_req = 0;
	}

	/* retire any completed rx requests from previous session */
	while ((req = mtp_req_get(ctxt, &ctxt->rx_done)))
		mtp_req_put(ctxt, &ctxt->rx_idle, req);

	/* Attempt to queue initial read request to catch
	   first host command before client is ready */
	req = mtp_req_get(ctxt, &ctxt->rx_idle);
	if (req) {
		req->length = TXN_MAX;
		ret = usb_ep_queue(ctxt->out, req, GFP_ATOMIC);
		if (ret < 0)
			mtp_req_put(ctxt, &ctxt->rx_idle, req);
	}


	/* Set connection state; notify userspace client */
	if (MTP_CONNECTING) {
		mtp_debug("mtp_configure: connecting\n");

		ctxt->connection_state = MTP_STATE_CONNECTED;
		add_event(MTP_EVENT_CONNECTED, NULL, 0);
	} else if (MTP_CONNECTED) {
		mtp_debug("mtp_configure: deconfiguring\n");

		add_event(MTP_EVENT_READ_RELEASED, NULL, 0);
		add_event(MTP_EVENT_DISCONNECTED, NULL, 0);

		/* Set state to disconnected */
		ctxt->connection_state = MTP_STATE_DISCONNECTED;
	}
	return 0;
}

static void mtp_ep0_complete_out(struct usb_ep *ept,
				 struct usb_request *req)
{
	unsigned char *data = req->buf;
	static struct mtp_cancel_req cancel_req;

	if (req->status != 0) {
		mtp_debug("MTP ep0 data retrieval status: fail %x\n",
			  req->status);
	} else {
		memcpy((unsigned char *) &cancel_req, data, req->length);
		mtp_debug("MTP Cancel Request code %x\n",
			  le16_to_cpu(cancel_req.CancellationCode));
		mtp_debug("MTP Cancel Request Transaction ID %x\n",
			  le32_to_cpu(cancel_req.TransactionID));

		/* The data stage (cancel_req) is not used by userspace, and can be discarded here. */
	}
}


static int mtp_function_setup(struct usb_function *f,
			      const struct usb_ctrlrequest *ctrl)
{
	int value = -EOPNOTSUPP;
	u16 w_index = le16_to_cpu(ctrl->wIndex);
	u16 w_length = le16_to_cpu(ctrl->wLength);
	struct mtp_context *ctxt = &_context;
	struct usb_composite_dev *cdev = f->config->cdev;
	struct usb_request *req = cdev->req;

	/* Handle Vendor-specific requests */
	switch (ctrl->bRequestType & USB_TYPE_MASK) {
	case USB_TYPE_VENDOR:
		if ((ctrl->bRequest == 0xFE) && (w_index == 0x0004)) {
			_ex_compat_id_descriptor.func.
			    bFirstInterfaceNumber =
			    intf_desc.bInterfaceNumber;
			memcpy(req->buf,
			       (void *) &_ex_compat_id_descriptor,
			       sizeof(_ex_compat_id_descriptor));
			value = sizeof(_ex_compat_id_descriptor);
		}
		break;

	case USB_TYPE_CLASS:
		switch (ctrl->bRequest) {
		case MTP_EVENT_CSR_CANCEL:
			/* Parse the setup message and return the expected length of buffer
			 * for the OUT stage. Schedule a callback to retrieve data.
			 */
			ctxt->ctl_tx_req->complete = mtp_ep0_complete_out;
			value = w_length;

			/* Send the control request to the userspace MTP transport/engine.
			 ** The data stage is not used by userspace, and will be discarded
			 ** by mtp_ep0_complete_out after reading.
			 */
			add_event(MTP_EVENT_CSR_CANCEL,
				  (unsigned char *) ctrl,
				  sizeof(struct usb_ctrlrequest));
			break;

		case MTP_EVENT_CSR_DEVICE_RESET:
			mtp_debug("mtp_setup: DEVICE_RESET\n");

			/* Set states for reset handling */
			ctxt->connection_state = MTP_STATE_RESET;
			ctxt->user_bufptr = ctxt->user_buf;
			ctxt->read_count = 0;
			ctxt->read_buf = 0;

			/* Halt the bulk IN/OUT endpoints */

			/* Forward the command to the userspace MTP transport/engine */
			add_event(MTP_EVENT_CSR_DEVICE_RESET,
				  (unsigned char *) ctrl,
				  sizeof(struct usb_ctrlrequest));
			value = 0;
			break;

		case MTP_EVENT_CSR_GET_DEVICE_STATUS:
			mtp_debug("mtp_setup: DEVICE_STATUS, wLength=%d\n",
				  w_length);

			if (MTP_RESET) {
				mtp_debug
				    ("mtp_setup: DEVICE_STATUS (state=RESET), wLength=%d\n",
				     w_length);

				if (1) {
					/* The PC has not yet cleared the bulk endpoints previously stalled.
					 ** Respond with transaction cancelled details
					 */
					if (req->buf) {
						/* Don't send unless a buffer of appropriate size was supplied */
						value = 12;
						((u16 *) req->buf)[0] = 12;
						((u16 *) req->buf)[1] = MTP_CSR_CODE_CANCELLED;	/* Transaction cancelled */
						((u32 *) req->buf)[1] = USB_DIR_IN;	/* Bulk in EP number     */
						((u32 *) req->buf)[2] = 0;	/* Bulk out EP number    */

					}
				} else {
					/* The PC cleared the endpoints; return to connected state and wake up reads */
					ctxt->connection_state =
					    MTP_STATE_CONNECTED;
					wake_up(&ctxt->read_wq);
				}
			}

			if (!MTP_RESET) {
				mtp_debug
				    ("mtp_setup: DEVICE_STATUS (state!=RESET), wLength=%d\n",
				     w_length);

				if (ctxt->csr_resp_ready) {
					/* If a response is available from userspace, return it. */
					if (req->buf) {
						value = 4;
						memcpy(req->buf,
						       ctxt->csr_buf, 4);
						ctxt->csr_resp_ready = 0;

						mtp_debug
						    ("mtp_setup: DEVICE_STATUS sending response\n");
					}
				} else {
					/* If a response is not available from userspace, send request to userspace
					 ** and reply with 'busy', which should result in a new PC request when the
					 ** userspace response is available.
					 */
					add_event
					    (MTP_EVENT_CSR_GET_DEVICE_STATUS,
					     (unsigned char *) ctrl,
					     sizeof(struct
						    usb_ctrlrequest));

					if (req->buf) {
						value = 4;
						((u16 *) req->buf)[0] = 4;
						((u16 *) req->buf)[1] =
						    MTP_CSR_CODE_BUSY;

						mtp_debug("mtp_setup: DEVICE_STATUS sending BUSY response\n");
					}
				}
			}
			break;

		case MTP_EVENT_CSR_GET_EVENT:
		default:
			/* Not implemented */
			break;
		}
		break;

	default:
		break;
	}

	return value;
}

static void mtp_function_disable(struct usb_function *f)
{
	struct usb_request *req = NULL;
	struct mtp_context *ctxt = &_context;

	printk(KERN_DEBUG "%s(): disabled\n", __func__);

	usb_ep_disable(ctxt->in);
	usb_ep_disable(ctxt->out);
	/*usb_ep_disable(ctxt->intr_in);*/

	/*ctxt->cur_read_req = 0;*/
	/*ctxt->read_buf = 0;*/
	/*ctxt->data_len = 0;*/

	/* drop finished requests */
	while ((req = mtp_req_get(ctxt, &ctxt->rx_done)))
		mtp_req_put(ctxt, &ctxt->rx_idle, req);

	/* readers may be blocked waiting for us to go online */
	wake_up(&ctxt->read_wq);
	wake_up(&ctxt->write_wq);
	wake_up(&ctxt->event_wq);
}

int mtp_bind_config(struct usb_configuration *c)
{
	int ret = 0;
	int status;
	struct mtp_context *ctxt = &_context;
	mtp_debug("mtp_init()\n");

	ctxt->cdev = c->cdev;
	ctxt->connection_state = MTP_STATE_DISCONNECTED;
	ctxt->releasing = 0;
	ctxt->csr_resp_ready = 0;
	ctxt->events_queued = 0;
	/* 0'th position represents number of events; data starts at 1 */
	ctxt->event_buf_index = 1;

	init_waitqueue_head(&ctxt->read_wq);
	init_waitqueue_head(&ctxt->write_wq);
	init_waitqueue_head(&ctxt->event_wq);

	atomic_set(&ctxt->open_excl, 0);
	atomic_set(&ctxt->read_excl, 0);
	atomic_set(&ctxt->write_excl, 0);
	atomic_set(&ctxt->control_excl, 0);
	atomic_set(&ctxt->event_excl, 0);
	atomic_set(&ctxt->eventread_excl, 0);

	spin_lock_init(&ctxt->lock);

	INIT_LIST_HEAD(&ctxt->rx_idle);
	INIT_LIST_HEAD(&ctxt->rx_done);
	INIT_LIST_HEAD(&ctxt->tx_idle);

	status = usb_string_id(c->cdev);
	if (status >= 0) {
		mtp_string_defs[STRING_INTERFACE].id = status;
		intf_desc.iInterface = status;
	}

	mtp_string_defs[STRING_MTP].id = mtp_ext_str_idx;


	ctxt->cdev = c->cdev;
	ctxt->function.name = "mtp";
	ctxt->function.bind = mtp_function_bind;
	ctxt->function.unbind = mtp_function_unbind;
	ctxt->function.setup = mtp_function_setup;
	ctxt->function.set_alt = mtp_function_set_alt;
	ctxt->function.disable = mtp_function_disable;
	ctxt->function.strings = mtp_strings;
	ctxt->function.descriptors = fs_mtp_descs;
	ctxt->function.hs_descriptors = hs_mtp_descs;

	ret = misc_register(&mtp_device);
	if (ret) {
		printk(KERN_ERR "mtp Can't register misc device  %d \n",
		       MISC_DYNAMIC_MINOR);
		return ret;
	}

	ret = misc_register(&mtp_control_device);
	if (ret) {
		printk(KERN_ERR "mtp Can't register misc device  %d \n",
		       MISC_DYNAMIC_MINOR);
		misc_deregister(&mtp_device);
		return ret;
	}

	ret = misc_register(&mtp_event_device);
	if (ret) {
		printk(KERN_ERR "mtp Can't register misc device  %d \n",
		       MISC_DYNAMIC_MINOR);
		misc_deregister(&mtp_device);
		misc_deregister(&mtp_control_device);
		return ret;
	}

	/* start disabled */
	ctxt->function.hidden = 1;

	ret = usb_add_function(c, &ctxt->function);
	if (ret) {
		mtp_err("MTP gadget driver failed to initialize\n");
		return ret;
	}

	return ret;
}

static struct android_usb_function mtp_function = {
	.name = "mtp",
	.bind_config = mtp_bind_config,
};

static int __init init(void)
{
	printk(KERN_INFO "f_mtp init\n");
	android_register_function(&mtp_function);
	return 0;
}

module_init(init);

