// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal Huawei "THP" (touch host processing) host interface for the
 * Huawei MatePad 11 (DBY-W09) on mainline Linux.
 *
 * The touch controller (Novatek, part of the NT36523-family display driver
 * IC) only delivers raw frames over SPI; Huawei's closed aptouch_daemon turns
 * them into touches.  This driver reimplements just the kernel interface that
 * daemon uses, as recorded with strace on the stock 4.19 kernel:
 *
 *   /dev/thp               GET_FRAME, SPI_SYNC, RESET, SET_TIMEOUT,
 *                          SET_BLOCK, SET_IRQ, CLEAR_FRAME_BUFFER (ioctl 0xB8)
 *   /dev/input_mt_wrapper  SET_COORDINATES -> input events, READ_STATUS +
 *                          poll, READ_INPUT_CONFIG, READ_SCENE_INFO,
 *                          GET_PLATFORM_TYPE, GET_APP_INFO,
 *                          READ_FINGER_RESOLUTION_MAGNIFICATION (0xB9/0xBB)
 *                          M-Pencil: REPORT_PEN -> "huawei,ts_pen",
 *                          GET/SET_STYLUS3_CONNECT_STATUS,
 *                          GET/SET_CALLBACK_EVENTS (0xBA/0xBB)
 *   /sys/touchscreen/touch_chip_info
 *
 * The pen's position and tilt come from the panel frames (the daemon), its
 * tip pressure over BLE (the daemon reads the pen's hidraw node). A user
 * space Bluetooth helper sets the stylus3 status when the pen connects and
 * forwards the daemon's callback events to the pen over GATT.
 *
 * Structure layouts, ioctl numbers and semantics follow Huawei's GPL
 * huawei_thp_core.c / huawei_thp_mt_wrapper.c / novatek_thp.c.
 */

#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include <drm/drm_panel.h>

/* /dev/thp */
#define THP_IO_TYPE			0xB8
#define THP_IOCTL_CMD_GET_FRAME		_IOWR(THP_IO_TYPE, 0x01, struct thp_get_frame_data)
#define THP_IOCTL_CMD_RESET		_IOW(THP_IO_TYPE, 0x02, u32)
#define THP_IOCTL_CMD_SET_TIMEOUT	_IOW(THP_IO_TYPE, 0x03, u32)
#define THP_IOCTL_CMD_SPI_SYNC		_IOWR(THP_IO_TYPE, 0x04, struct thp_spi_sync_data)
#define THP_IOCTL_CMD_FINISH_NOTIFY	_IO(THP_IO_TYPE, 0x05)
#define THP_IOCTL_CMD_SET_BLOCK		_IOW(THP_IO_TYPE, 0x06, u32)
#define THP_IOCTL_CMD_SET_IRQ		_IOW(THP_IO_TYPE, 0x07, u32)
#define THP_IOCTL_CMD_GET_FRAME_COUNT	_IOW(THP_IO_TYPE, 0x08, u32)
#define THP_IOCTL_CMD_CLEAR_FRAME_BUFFER _IOW(THP_IO_TYPE, 0x09, u32)
#define THP_IOCTL_CMD_GET_IRQ_GPIO_VALUE _IOW(THP_IO_TYPE, 0x0A, u32)
#define THP_IOCTL_CMD_SET_SPI_SPEED	_IOW(THP_IO_TYPE, 0x0B, u32)

/* /dev/input_mt_wrapper */
#define MT_IO_TYPE			0xB9
#define MT_SET_IO_TYPE			0xBB
#define MT_IOCTL_SET_COORDINATES	_IOWR(MT_IO_TYPE, 0x01, struct thp_touch_data)
#define MT_IOCTL_READ_STATUS		_IOR(MT_IO_TYPE, 0x02, u32)
#define MT_IOCTL_READ_INPUT_CONFIG	_IOR(MT_IO_TYPE, 0x03, struct thp_input_config)
#define MT_IOCTL_READ_SCENE_INFO	_IOR(MT_IO_TYPE, 0x04, struct thp_scene_info)
#define MT_IOCTL_GET_PLATFORM_TYPE	_IOR(MT_IO_TYPE, 0x0e, u32)
#define MT_IOCTL_GET_APP_INFO		_IOWR(MT_IO_TYPE, 0x11, struct thp_app_info)
#define MT_IOCTL_READ_MAGNIFICATION	_IOWR(MT_IO_TYPE, 0x12, u32)
#define MT_GET_IO_TYPE			0xBA
#define MT_IOCTL_REPORT_PEN		_IOWR(MT_IO_TYPE, 0x0c, struct thp_pen_data)
#define MT_IOCTL_GET_STYLUS3_STATUS	_IOWR(MT_GET_IO_TYPE, 0x05, u32)
#define MT_IOCTL_GET_CALLBACK_EVENTS	_IOWR(MT_GET_IO_TYPE, 0x07, struct thp_callback_event)
#define MT_IOCTL_SET_STYLUS3_STATUS	_IOWR(MT_SET_IO_TYPE, 0x08, u32)
#define MT_IOCTL_SET_CALLBACK_EVENTS	_IOWR(MT_SET_IO_TYPE, 0x09, struct thp_callback_event)
#define MT_IOCTL_SET_PALM_SUPPRESSION	_IOWR(MT_SET_IO_TYPE, 0x0c, u32)
#define MT_IOCTL_SET_ADSORPTION_STATUS	_IOWR(MT_SET_IO_TYPE, 0x0d, u32)

#define THP_MAX_FRAME_SIZE	(8 * 1024 + 16)
#define THP_SYNC_DATA_MAX	(4096 * 32)
#define THP_LIST_MAX_FRAMES	20
#define THP_DEFAULT_TIMEOUT_MS	200
#define THP_WAIT_MAX_TIME_MS	2000u
#define THP_MAX_FINGERS		10
#define NVT_GET_FRAME_CMD	0x20	/* spi_read_mask(0x20) */

/* Status bits the daemon reads (huawei_thp.h, enum thp_status_type). */
#define THP_STATUS_POWER		0
#define THP_STATUS_CHARGER		2
#define THP_STATUS_WINDOW_UPDATE	6
#define THP_STATUS_TOUCH_SCENE		7
#define THP_STATUS_STYLUS3		15
#define THP_STATUS_STYLUS3_PALM_SUPPRESSION 17
#define STYLUS3_CONNECTED_MASK		BIT(1)
#define STYLUS3_PALM_SUPPRESSION_MASK	BIT(4)

/* huawei,thp_pen_input in the stock tree; tilt in degrees. */
#define THP_PEN_MAX_X			12800
#define THP_PEN_MAX_Y			20480
#define THP_PEN_MAX_PRESSURE		16384
#define THP_PEN_MAX_TILT		90
/* 12800 x 20480 over the 147.6 x 236.2 mm panel; libinput wants units/rad. */
#define THP_PEN_RES_XY			87
#define THP_PEN_RES_TILT		57

/* novatek_thp.c, N9Z series: firmware command 0xC2 = Bluetooth pen status. */
#define NVT_N9Z_CMD_ADDR		0x2FE50
#define NVT_N9Z_CMD_STAMP_ADDR		0x2FE77
#define NVT_CMD_PEN_STATUS		0xC2
#define NVT_CMD_RETRIES			3
#define NVT_STAMP_POLLS			5
#define NVT_PEN_STATUS_AFTER_RESET_MS	200

struct thp_get_frame_data {
	char __user *buf;
	char __user *tv;	/* struct timeval: two longs */
	unsigned int size;
};

struct thp_spi_sync_data {
	char __user *tx;
	char __user *rx;
	unsigned int size;
};

struct thp_finger {
	unsigned char down;
	unsigned char valid;
	int x;
	int y;
	int pressure;
	int tracking_id;
	int hand_side;
	int major;
	int minor;
	int orientation;
	unsigned int tool_type;
};

struct thp_touch_data {
	struct thp_finger touch[THP_MAX_FINGERS];
	int state;
	int t_num;
	int down_num;
};

struct thp_input_config {
	int abs_max_x;
	int abs_max_y;
	int abs_max_z;
	int tracking_id_max;
	int major_max;
	int minor_max;
	int orientation_min;
	int orientation_max;
	int tool_type_max;
};

struct thp_scene_info {
	int type;
	int status;
	int parameter;
};

struct thp_app_info {
	unsigned int len;
	char __user *buf;
};

struct thp_callback_event {
	int event_class;
	int event_code;
	char extra_info[32];
};

struct thp_pen_tool {
	int pen_inrange_status;	/* BTN_TOOL_* value */
	int tip_status;		/* BTN_TOUCH */
	int x;
	int y;
	int pressure;
	signed char tilt_x;
	signed char tilt_y;
	int tool_type;		/* BTN_TOOL_PEN or BTN_TOOL_RUBBER */
};

struct thp_pen_button {
	int status;		/* PEN_BUTTON_PRESS / _RELEASE, 0 = none */
	int key;
};

#define THP_PEN_BUTTONS		2
#define THP_PEN_BUTTON_PRESS	BIT(6)

struct thp_pen_data {
	struct thp_pen_tool tool;
	struct thp_pen_button buttons[THP_PEN_BUTTONS];
};

struct thp_timeval {
	long tv_sec;
	long tv_usec;
};

struct thp_frame {
	struct list_head list;
	struct thp_timeval tv;
	u8 data[THP_MAX_FRAME_SIZE];
};

struct thp {
	struct spi_device *spi;
	struct gpio_desc *rst_gpio;
	int irq;
	const char *chip_info;
	u32 magnification;
	struct thp_input_config input_cfg;

	struct mutex spi_lock;		/* frame reads vs. SPI_SYNC */
	u8 *frame_tx, *frame_rx;
	u8 *sync_tx, *sync_rx;

	struct mutex frame_lock;
	struct list_head frames;
	unsigned int frame_count;
	unsigned int frame_size;
	wait_queue_head_t frame_waitq;
	bool frame_ready;
	unsigned int timeout_ms;
	bool block;

	struct mutex irq_lock;
	bool irq_enabled;
	bool in_reset;
	atomic_t open_count;

	u32 status;
	atomic_t status_updated;
	wait_queue_head_t status_waitq;

	/* The touch part lives and dies with the panel (one display/touch IC). */
	struct drm_panel_follower panel_follower;
	bool suspended;

	struct input_dev *input;
	struct input_dev *pen;

	/* M-Pencil (huawei_thp_mt_wrapper.c stylus3 interface) */
	u32 stylus3_status;
	struct completion stylus3_done;		/* GET_STYLUS3 waits for a SET */
	struct thp_callback_event callback_event;
	bool callback_pending;
	struct completion callback_done;	/* GET_CALLBACK waits for a SET */
	spinlock_t callback_lock;
	bool nvt_pen_cmd;			/* send_bt_status_to_fw */
	struct mutex nvt_cmd_lock;
	u8 *cmd_tx, *cmd_rx;

	struct miscdevice thp_misc;
	struct miscdevice mt_misc;
	struct platform_device *sysfs_pdev;
};

static struct thp *g_thp;	/* the misc devices carry no private data */

static void thp_set_irq(struct thp *t, bool enable)
{
	mutex_lock(&t->irq_lock);
	if (t->irq_enabled != enable) {
		if (enable)
			enable_irq(t->irq);
		else
			disable_irq(t->irq);
		t->irq_enabled = enable;
	}
	mutex_unlock(&t->irq_lock);
}

static void thp_wake_frames(struct thp *t)
{
	t->frame_ready = true;
	wake_up_interruptible(&t->frame_waitq);
}

static void thp_clear_frames(struct thp *t)
{
	struct thp_frame *f, *tmp;

	list_for_each_entry_safe(f, tmp, &t->frames, list) {
		list_del(&f->list);
		kfree(f);
	}
	t->frame_count = 0;
}

static int thp_spi_xfer(struct thp *t, const void *tx, void *rx, size_t len)
{
	struct spi_transfer xfer = { .tx_buf = tx, .rx_buf = rx, .len = len };
	int ret;

	mutex_lock(&t->spi_lock);
	ret = spi_sync_transfer(t->spi, &xfer, 1);
	mutex_unlock(&t->spi_lock);
	return ret;
}

/* Point the Novatek SPI window at addr (write 0x7F = address bits 22..7). */
static int thp_nvt_set_page(struct thp *t, u32 addr)
{
	t->cmd_tx[0] = 0x7F | 0x80;
	t->cmd_tx[1] = (addr >> 15) & 0xFF;
	t->cmd_tx[2] = (addr >> 7) & 0xFF;
	return thp_spi_xfer(t, t->cmd_tx, NULL, 3);
}

/* novatek_thp.c touch_driver_read_fw_stamp() */
static int thp_nvt_read_stamp(struct thp *t, u8 *stamp)
{
	int ret = thp_nvt_set_page(t, NVT_N9Z_CMD_STAMP_ADDR);

	if (ret)
		return ret;
	memset(t->cmd_tx, 0, 3);
	t->cmd_tx[0] = NVT_N9Z_CMD_STAMP_ADDR & 0x7F;
	ret = thp_spi_xfer(t, t->cmd_tx, t->cmd_rx, 3);
	if (!ret)
		*stamp = t->cmd_rx[2];
	return ret;
}

/*
 * novatek_thp.c touch_driver_nt_bt_handler(): tell the touch firmware whether
 * a Bluetooth pen is connected (command 0xC2, args 02 01 / 02 00), as the
 * stock driver does for send_bt_status_to_fw.  The firmware acknowledges by
 * copying the command's stamp to the stamp register.
 */
static int thp_nvt_send_pen_status(struct thp *t, bool after_reset)
{
	bool connected = t->stylus3_status & STYLUS3_CONNECTED_MASK;
	u8 *tx = t->cmd_tx;
	u8 stamp = 0, idx;
	int ret = -EIO, try, i;

	if (!t->nvt_pen_cmd || t->suspended)
		return 0;
	if (after_reset)
		msleep(NVT_PEN_STATUS_AFTER_RESET_MS);

	mutex_lock(&t->nvt_cmd_lock);
	for (try = 0; try < NVT_CMD_RETRIES; try++) {
		ret = thp_nvt_read_stamp(t, &stamp);
		if (!ret)
			ret = thp_nvt_set_page(t, NVT_N9Z_CMD_ADDR);
		if (ret)
			continue;
		memset(tx, 0, 7);
		tx[0] = (NVT_N9Z_CMD_ADDR & 0x7F) | 0x80;
		tx[1] = NVT_CMD_PEN_STATUS;
		tx[2] = 0xBB;			/* handshake */
		tx[3] = 0x02;			/* argument: stylus status */
		tx[4] = connected ? 0x01 : 0x00;
		tx[5] = stamp - 1;		/* "previous number", as the HAL */
		tx[6] = (u8)(~(tx[1] + tx[2] + tx[3] + tx[4] + tx[5]) + 1);
		idx = tx[5];
		ret = thp_spi_xfer(t, tx, NULL, 7);
		if (ret)
			continue;
		ret = -EIO;
		for (i = 0; i < NVT_STAMP_POLLS; i++) {
			usleep_range(1000, 1500);
			if (!thp_nvt_read_stamp(t, &stamp) && stamp == idx) {
				ret = 0;
				break;
			}
		}
		if (!ret)
			break;
	}
	mutex_unlock(&t->nvt_cmd_lock);
	if (ret)
		dev_warn(&t->spi->dev, "pen status (%s) not acknowledged: %d\n",
			 connected ? "connected" : "disconnected", ret);
	else
		dev_dbg(&t->spi->dev, "pen status %s sent\n",
			connected ? "connected" : "disconnected");
	return ret;
}

static irqreturn_t thp_irq_thread(int irq, void *data)
{
	struct thp *t = data;
	struct thp_frame *f;
	struct timespec64 ts;
	unsigned int len = min_t(unsigned int, t->frame_size, THP_MAX_FRAME_SIZE);

	if (t->in_reset || t->suspended)
		return IRQ_HANDLED;

	/* novatek_thp.c touch_driver_get_frame: command 0x20, then the frame. */
	memset(t->frame_tx, 0, len);
	t->frame_tx[0] = NVT_GET_FRAME_CMD;
	if (thp_spi_xfer(t, t->frame_tx, t->frame_rx, len))
		return IRQ_HANDLED;

	f = kmalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return IRQ_HANDLED;
	memcpy(f->data, t->frame_rx, len);
	ktime_get_real_ts64(&ts);
	f->tv.tv_sec = ts.tv_sec;
	f->tv.tv_usec = ts.tv_nsec / NSEC_PER_USEC;

	mutex_lock(&t->frame_lock);
	if (t->frame_count >= THP_LIST_MAX_FRAMES) {
		struct thp_frame *old = list_first_entry(&t->frames, struct thp_frame, list);

		list_del(&old->list);
		kfree(old);
		t->frame_count--;
	}
	list_add_tail(&f->list, &t->frames);
	t->frame_count++;
	mutex_unlock(&t->frame_lock);

	thp_wake_frames(t);
	return IRQ_HANDLED;
}

static long thp_ioctl_get_frame(struct thp *t, void __user *argp)
{
	struct thp_get_frame_data d;
	struct thp_frame *f;
	long rc = 0;

	if (copy_from_user(&d, argp, sizeof(d)))
		return -EFAULT;
	if (!d.buf || !d.tv || !d.size || d.size > THP_MAX_FRAME_SIZE)
		return -EINVAL;
	if (t->suspended)
		return -ETIMEDOUT;	/* as thp_ioctl_get_frame() while suspended */

	t->frame_size = d.size;
	thp_set_irq(t, true);

	if (list_empty(&t->frames) && t->block) {
		long left;

		t->frame_ready = false;
		if (!t->timeout_ms) {
			wait_event_interruptible(t->frame_waitq, t->frame_ready);
		} else {
			left = wait_event_interruptible_timeout(t->frame_waitq, t->frame_ready,
						msecs_to_jiffies(t->timeout_ms));
			if (left == 0)
				rc = -ETIMEDOUT;
		}
	}

	mutex_lock(&t->frame_lock);
	if (list_empty(&t->frames)) {
		mutex_unlock(&t->frame_lock);
		return rc ? rc : -ENODATA;
	}
	f = list_first_entry(&t->frames, struct thp_frame, list);
	if (copy_to_user(d.buf, f->data, d.size) ||
	    copy_to_user(d.tv, &f->tv, sizeof(f->tv))) {
		mutex_unlock(&t->frame_lock);
		return -EFAULT;
	}
	list_del(&f->list);
	kfree(f);
	t->frame_count--;
	mutex_unlock(&t->frame_lock);
	return 0;
}

static long thp_ioctl_spi_sync(struct thp *t, void __user *argp)
{
	struct thp_spi_sync_data d;
	int rc;

	if (copy_from_user(&d, argp, sizeof(d)))
		return -EFAULT;
	if (!d.size || d.size > THP_SYNC_DATA_MAX || !d.tx)
		return -EINVAL;
	if (t->suspended)
		return 0;		/* as thp_ioctl_spi_sync() while suspended */
	if (copy_from_user(t->sync_tx, d.tx, d.size))
		return -EFAULT;
	rc = thp_spi_xfer(t, t->sync_tx, t->sync_rx, d.size);
	if (rc)
		return rc;
	if (d.rx && copy_to_user(d.rx, t->sync_rx, d.size))
		return -EFAULT;
	return 0;
}

static long thp_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct thp *t = g_thp;
	void __user *argp = (void __user *)arg;
	u32 v;

	switch (cmd) {
	case THP_IOCTL_CMD_GET_FRAME:
		return thp_ioctl_get_frame(t, argp);
	case THP_IOCTL_CMD_SPI_SYNC:
		return thp_ioctl_spi_sync(t, argp);
	case THP_IOCTL_CMD_RESET:
		/* Huawei: the argument is the reset line's level (1 = running). */
		gpiod_set_value_cansleep(t->rst_gpio, !!arg);
		/* thp_ioctl_reset(): re-send the pen status once the IC runs. */
		if (arg)
			thp_nvt_send_pen_status(t, true);
		t->frame_ready = false;
		t->in_reset = !arg;
		return 0;
	case THP_IOCTL_CMD_SET_TIMEOUT:
		v = min_t(u32, arg, THP_WAIT_MAX_TIME_MS);
		if (v != t->timeout_ms) {
			t->timeout_ms = v;
			thp_wake_frames(t);
		}
		return 0;
	case THP_IOCTL_CMD_SET_BLOCK:
		t->block = !!arg;
		thp_wake_frames(t);
		return 0;
	case THP_IOCTL_CMD_SET_IRQ:
		thp_set_irq(t, !!arg);
		return 0;
	case THP_IOCTL_CMD_CLEAR_FRAME_BUFFER:
		mutex_lock(&t->frame_lock);
		thp_clear_frames(t);
		mutex_unlock(&t->frame_lock);
		return 0;
	case THP_IOCTL_CMD_GET_FRAME_COUNT:
		v = t->frame_count;
		return copy_to_user(argp, &v, sizeof(v)) ? -EFAULT : 0;
	case THP_IOCTL_CMD_GET_IRQ_GPIO_VALUE:
		v = 0;
		return copy_to_user(argp, &v, sizeof(v)) ? -EFAULT : 0;
	case THP_IOCTL_CMD_SET_SPI_SPEED:
		if (arg) {
			t->spi->max_speed_hz = arg;
			return spi_setup(t->spi);
		}
		return 0;
	default:
		/* Huawei's core also answers unknown commands with 0. */
		dev_dbg(&t->spi->dev, "unhandled thp ioctl 0x%x\n", cmd);
		return 0;
	}
}

static int thp_open(struct inode *inode, struct file *file)
{
	struct thp *t = g_thp;

	if (atomic_cmpxchg(&t->open_count, 0, 1))
		return -EBUSY;
	t->block = true;
	t->timeout_ms = THP_DEFAULT_TIMEOUT_MS;
	t->frame_size = THP_MAX_FRAME_SIZE;
	t->in_reset = false;
	/* Opening makes the daemon (re)initialise: as thp_open() does. */
	t->status |= BIT(THP_STATUS_WINDOW_UPDATE) | BIT(THP_STATUS_TOUCH_SCENE);
	mutex_lock(&t->frame_lock);
	thp_clear_frames(t);
	mutex_unlock(&t->frame_lock);
	return 0;
}

static int thp_release(struct inode *inode, struct file *file)
{
	struct thp *t = g_thp;

	thp_wake_frames(t);
	thp_set_irq(t, false);
	atomic_set(&t->open_count, 0);
	return 0;
}

static const struct file_operations thp_fops = {
	.owner = THIS_MODULE,
	.open = thp_open,
	.release = thp_release,
	.unlocked_ioctl = thp_ioctl,
};

static void thp_report(struct thp *t, const struct thp_touch_data *d)
{
	struct input_dev *in = t->input;
	int i;

	for (i = 0; i < THP_MAX_FINGERS; i++) {
		const struct thp_finger *f = &d->touch[i];

		if (!f->valid)
			continue;
		input_report_abs(in, ABS_MT_POSITION_X, f->x);
		input_report_abs(in, ABS_MT_POSITION_Y, f->y);
		input_report_abs(in, ABS_MT_PRESSURE, f->pressure);
		input_report_abs(in, ABS_MT_TRACKING_ID, f->tracking_id);
		input_report_abs(in, ABS_MT_TOUCH_MAJOR, f->major);
		input_report_abs(in, ABS_MT_TOUCH_MINOR, f->minor);
		input_report_abs(in, ABS_MT_ORIENTATION, f->orientation);
		input_report_abs(in, ABS_MT_TOOL_TYPE, f->tool_type);
		input_report_abs(in, ABS_MT_BLOB_ID, f->hand_side);
		input_mt_sync(in);
	}
	if (d->t_num > 0) {
		input_report_key(in, BTN_TOUCH, 1);
	} else {
		input_mt_sync(in);
		input_report_key(in, BTN_TOUCH, 0);
	}
	input_sync(in);
}

static void thp_status_changed(struct thp *t);

/* thp_mt_wrapper_ioctl_report_pen() / thp_report_pen_event() */
static void thp_report_pen(struct thp *t, const struct thp_pen_data *d)
{
	struct input_dev *in = t->pen;
	int i;

	for (i = 0; i < THP_PEN_BUTTONS; i++) {
		const struct thp_pen_button *b = &d->buttons[i];

		if (b->status && b->key > 0 && b->key < KEY_CNT &&
		    test_bit(b->key, in->keybit))
			input_report_key(in, b->key, b->status == THP_PEN_BUTTON_PRESS);
	}
	input_report_abs(in, ABS_X, d->tool.x);
	input_report_abs(in, ABS_Y, d->tool.y);
	input_report_abs(in, ABS_PRESSURE, d->tool.pressure);
	input_report_abs(in, ABS_TILT_X, d->tool.tilt_x);
	input_report_abs(in, ABS_TILT_Y, d->tool.tilt_y);
	input_report_key(in, BTN_TOUCH, d->tool.tip_status);
	if (d->tool.tool_type == BTN_TOOL_PEN || d->tool.tool_type == BTN_TOOL_RUBBER)
		input_report_key(in, d->tool.tool_type, d->tool.pen_inrange_status);
	input_sync(in);
}

/* thp_ioctl_set_stylus3_connect_status(): from the Bluetooth helper. */
static int thp_set_stylus3_status(struct thp *t, u32 status)
{
	bool was = t->status & BIT(THP_STATUS_STYLUS3);
	bool now = status & STYLUS3_CONNECTED_MASK;

	t->stylus3_status = status;
	if (now)
		t->status |= BIT(THP_STATUS_STYLUS3);
	else
		t->status &= ~BIT(THP_STATUS_STYLUS3);
	complete(&t->stylus3_done);
	if (was != now)
		thp_status_changed(t);
	dev_info(&t->spi->dev, "Bluetooth pen %s\n", now ? "connected" : "disconnected");
	thp_nvt_send_pen_status(t, false);
	return 0;
}

static long mt_ioctl_pen(struct thp *t, unsigned int cmd, void __user *argp)
{
	struct thp_callback_event ev;
	struct thp_pen_data pen;
	unsigned long flags;
	u32 v;
	int ret;

	switch (cmd) {
	case MT_IOCTL_REPORT_PEN:
		if (copy_from_user(&pen, argp, sizeof(pen)))
			return -EFAULT;
		thp_report_pen(t, &pen);
		return 0;
	case MT_IOCTL_SET_STYLUS3_STATUS:
		if (copy_from_user(&v, argp, sizeof(v)))
			return -EFAULT;
		return thp_set_stylus3_status(t, v);
	case MT_IOCTL_GET_STYLUS3_STATUS:
		/* The daemon blocks here until the status is set. */
		ret = wait_for_completion_interruptible(&t->stylus3_done);
		if (ret)
			return ret;
		v = t->stylus3_status;
		return copy_to_user(argp, &v, sizeof(v)) ? -EFAULT : 0;
	case MT_IOCTL_SET_CALLBACK_EVENTS:
		/* From the daemon, for the pen (via the Bluetooth helper). */
		if (copy_from_user(&ev, argp, sizeof(ev)))
			return -EFAULT;
		spin_lock_irqsave(&t->callback_lock, flags);
		if (t->callback_pending) {
			spin_unlock_irqrestore(&t->callback_lock, flags);
			return -EBUSY;	/* "callback event not handle, need retry" */
		}
		t->callback_event = ev;
		t->callback_pending = true;
		spin_unlock_irqrestore(&t->callback_lock, flags);
		complete(&t->callback_done);
		return 0;
	case MT_IOCTL_GET_CALLBACK_EVENTS:
		/* The Bluetooth helper blocks here until the daemon posts one. */
		ret = wait_for_completion_interruptible(&t->callback_done);
		spin_lock_irqsave(&t->callback_lock, flags);
		ev = t->callback_event;
		t->callback_pending = false;
		spin_unlock_irqrestore(&t->callback_lock, flags);
		if (ret)
			memset(&ev, 0, sizeof(ev));	/* interrupted: an empty event */
		return copy_to_user(argp, &ev, sizeof(ev)) ? -EFAULT : 0;
	case MT_IOCTL_SET_PALM_SUPPRESSION:
		if (copy_from_user(&v, argp, sizeof(v)))
			return -EFAULT;
		if (v & STYLUS3_PALM_SUPPRESSION_MASK)
			t->status |= BIT(THP_STATUS_STYLUS3_PALM_SUPPRESSION);
		else
			t->status &= ~BIT(THP_STATUS_STYLUS3_PALM_SUPPRESSION);
		return 0;
	case MT_IOCTL_SET_ADSORPTION_STATUS:
		return 0;	/* pen on the magnet: not used here */
	}
	return -ENOIOCTLCMD;
}

static long mt_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct thp *t = g_thp;
	void __user *argp = (void __user *)arg;
	struct thp_touch_data *td;
	struct thp_scene_info scene = { 0 };
	struct thp_app_info app;
	u32 v;
	long rc;

	switch (cmd) {
	case MT_IOCTL_SET_COORDINATES:
		td = kmalloc(sizeof(*td), GFP_KERNEL);
		if (!td)
			return -ENOMEM;
		rc = copy_from_user(td, argp, sizeof(*td)) ? -EFAULT : 0;
		if (!rc)
			thp_report(t, td);
		kfree(td);
		return rc;
	case MT_IOCTL_READ_STATUS:
		v = t->status;
		if (copy_to_user(argp, &v, sizeof(v)))
			return -EFAULT;
		atomic_dec_if_positive(&t->status_updated);
		return 0;
	case MT_IOCTL_READ_INPUT_CONFIG:
		return copy_to_user(argp, &t->input_cfg, sizeof(t->input_cfg)) ? -EFAULT : 0;
	case MT_IOCTL_READ_SCENE_INFO:
		return copy_to_user(argp, &scene, sizeof(scene)) ? -EFAULT : 0;
	case MT_IOCTL_GET_PLATFORM_TYPE:
		v = 0;	/* "cd->platform_type 0" on the stock kernel */
		return copy_to_user(argp, &v, sizeof(v)) ? -EFAULT : 0;
	case MT_IOCTL_READ_MAGNIFICATION:
		v = t->magnification;
		return copy_to_user(argp, &v, sizeof(v)) ? -EFAULT : 0;
	case MT_IOCTL_GET_APP_INFO:
		if (copy_from_user(&app, argp, sizeof(app)))
			return -EFAULT;
		if (!app.buf || !app.len || app.len > 256)
			return -EINVAL;
		return clear_user(app.buf, app.len) ? -EFAULT : 0;
	default:
		rc = mt_ioctl_pen(t, cmd, argp);
		if (rc != -ENOIOCTLCMD)
			return rc;
		dev_dbg(&t->spi->dev, "unhandled mt_wrapper ioctl 0x%x\n", cmd);
		return -EINVAL;
	}
}

static __poll_t mt_poll(struct file *file, poll_table *wait)
{
	struct thp *t = g_thp;

	poll_wait(file, &t->status_waitq, wait);
	return atomic_read(&t->status_updated) > 0 ? EPOLLIN | EPOLLRDNORM : 0;
}

static void thp_status_changed(struct thp *t)
{
	atomic_inc(&t->status_updated);
	wake_up_interruptible(&t->status_waitq);
}

/* novatek_thp.c touch_driver_sw_reset_idle(): write 0xAA to 0x3F0FE. */
static void thp_nvt_sw_reset_idle(struct thp *t)
{
	const u32 addr = 0x3F0FE;
	u8 *buf = t->frame_tx;

	buf[0] = 0x7F | 0x80;
	buf[1] = (addr >> 15) & 0xFF;
	buf[2] = (addr >> 7) & 0xFF;
	thp_spi_xfer(t, buf, NULL, 3);
	buf[0] = (addr & 0x7F) | 0x80;
	buf[1] = 0xAA;
	thp_spi_xfer(t, buf, NULL, 2);
	msleep(10);
}

/*
 * thp_suspend()/thp_resume() on the stock kernel run from lcd_kit's panel
 * power events; here the panel driver's follower callbacks do it.  Clearing
 * and setting THP_STATUS_POWER makes aptouch_daemon re-initialise the chip.
 */
static int thp_panel_disabling(struct drm_panel_follower *follower)
{
	struct thp *t = container_of(follower, struct thp, panel_follower);

	t->suspended = true;
	t->status &= ~BIT(THP_STATUS_POWER);
	thp_wake_frames(t);
	thp_status_changed(t);
	return 0;
}

static int thp_panel_enabled(struct drm_panel_follower *follower)
{
	struct thp *t = container_of(follower, struct thp, panel_follower);

	if (!t->suspended)
		return 0;
	thp_nvt_sw_reset_idle(t);
	gpiod_set_value_cansleep(t->rst_gpio, 1);
	msleep(1);			/* resume_reset_after_delay_ms */
	mutex_lock(&t->frame_lock);
	thp_clear_frames(t);
	mutex_unlock(&t->frame_lock);
	t->suspended = false;
	t->status |= BIT(THP_STATUS_POWER);
	thp_status_changed(t);
	return 0;
}

static const struct drm_panel_follower_funcs thp_panel_follower_funcs = {
	.panel_enabled = thp_panel_enabled,
	.panel_disabling = thp_panel_disabling,
};

static const struct file_operations mt_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = mt_ioctl,
	.poll = mt_poll,
};

static ssize_t touch_chip_info_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", g_thp->chip_info);
}
static DEVICE_ATTR_RO(touch_chip_info);

static ssize_t thp_status_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "status=0x%x\n", g_thp->status);
}
static DEVICE_ATTR_RO(thp_status);

static ssize_t roi_data_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	return 0;
}
static DEVICE_ATTR_RO(roi_data);

/*
 * aptouch_daemon waits until hostprocessing and daemon_version exist; on the
 * stock kernel daemon_version and touch_window are root-only (EACCES for the
 * daemon's UID 1000), which is kept here.
 */
static ssize_t hostprocessing_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "hostprocessing\n");
}
static DEVICE_ATTR_RO(hostprocessing);

static ssize_t daemon_version_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "3\n");
}

static ssize_t daemon_version_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	return count;
}
static DEVICE_ATTR(daemon_version, 0600, daemon_version_show, daemon_version_store);

static ssize_t touch_window_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "0 0 0 0 0\n");
}
static DEVICE_ATTR(touch_window, 0600, touch_window_show, NULL);

static struct attribute *thp_sysfs_attrs[] = {
	&dev_attr_touch_chip_info.attr,
	&dev_attr_thp_status.attr,
	&dev_attr_roi_data.attr,
	&dev_attr_hostprocessing.attr,
	&dev_attr_daemon_version.attr,
	&dev_attr_touch_window.attr,
	NULL
};
ATTRIBUTE_GROUPS(thp_sysfs);

static int thp_input_register(struct thp *t)
{
	struct input_dev *in;
	int mx = t->input_cfg.abs_max_x * t->magnification - 1;
	int my = t->input_cfg.abs_max_y * t->magnification - 1;

	in = devm_input_allocate_device(&t->spi->dev);
	if (!in)
		return -ENOMEM;
	in->name = "input_mt_wrapper";	/* the name the daemon and 4.19 setup use */
	__set_bit(EV_SYN, in->evbit);
	__set_bit(EV_KEY, in->evbit);
	__set_bit(EV_ABS, in->evbit);
	__set_bit(BTN_TOUCH, in->keybit);
	__set_bit(BTN_TOOL_FINGER, in->keybit);
	__set_bit(INPUT_PROP_DIRECT, in->propbit);
	input_set_abs_params(in, ABS_X, 0, mx, 0, 0);
	input_set_abs_params(in, ABS_Y, 0, my, 0, 0);
	input_set_abs_params(in, ABS_PRESSURE, 0, t->input_cfg.abs_max_z, 0, 0);
	input_set_abs_params(in, ABS_MT_POSITION_X, 0, mx, 0, 0);
	input_set_abs_params(in, ABS_MT_POSITION_Y, 0, my, 0, 0);
	input_set_abs_params(in, ABS_MT_PRESSURE, 0, t->input_cfg.abs_max_z, 0, 0);
	input_set_abs_params(in, ABS_MT_TRACKING_ID, 0, t->input_cfg.tracking_id_max - 1, 0, 0);
	input_set_abs_params(in, ABS_MT_TOUCH_MAJOR, 0, t->input_cfg.major_max, 0, 0);
	input_set_abs_params(in, ABS_MT_TOUCH_MINOR, 0, t->input_cfg.minor_max, 0, 0);
	input_set_abs_params(in, ABS_MT_ORIENTATION, t->input_cfg.orientation_min,
			     t->input_cfg.orientation_max, 0, 0);
	input_set_abs_params(in, ABS_MT_TOOL_TYPE, 0, t->input_cfg.tool_type_max, 0, 0);
	input_set_abs_params(in, ABS_MT_BLOB_ID, 0, THP_MAX_FINGERS, 0, 0);
	t->input = in;
	return input_register_device(in);
}

/* thp_input_pen_device_register(), with the axis resolutions libinput needs. */
static int thp_pen_register(struct thp *t)
{
	struct input_dev *pen;

	pen = devm_input_allocate_device(&t->spi->dev);
	if (!pen)
		return -ENOMEM;
	pen->name = "huawei,ts_pen";	/* TS_PEN_DEV_NAME */
	__set_bit(EV_SYN, pen->evbit);
	__set_bit(EV_KEY, pen->evbit);
	__set_bit(EV_ABS, pen->evbit);
	__set_bit(BTN_TOUCH, pen->keybit);
	__set_bit(BTN_TOOL_PEN, pen->keybit);
	__set_bit(BTN_TOOL_RUBBER, pen->keybit);
	__set_bit(BTN_STYLUS, pen->keybit);
	__set_bit(BTN_STYLUS2, pen->keybit);
	__set_bit(INPUT_PROP_DIRECT, pen->propbit);
	input_set_abs_params(pen, ABS_X, 0, THP_PEN_MAX_X, 0, 0);
	input_set_abs_params(pen, ABS_Y, 0, THP_PEN_MAX_Y, 0, 0);
	input_set_abs_params(pen, ABS_PRESSURE, 0, THP_PEN_MAX_PRESSURE, 0, 0);
	input_set_abs_params(pen, ABS_TILT_X, -THP_PEN_MAX_TILT, THP_PEN_MAX_TILT, 0, 0);
	input_set_abs_params(pen, ABS_TILT_Y, -THP_PEN_MAX_TILT, THP_PEN_MAX_TILT, 0, 0);
	input_abs_set_res(pen, ABS_X, THP_PEN_RES_XY);
	input_abs_set_res(pen, ABS_Y, THP_PEN_RES_XY);
	input_abs_set_res(pen, ABS_TILT_X, THP_PEN_RES_TILT);
	input_abs_set_res(pen, ABS_TILT_Y, THP_PEN_RES_TILT);
	t->pen = pen;
	return input_register_device(pen);
}

static int thp_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct thp *t;
	int ret;

	if (g_thp)
		return -EBUSY;
	t = devm_kzalloc(dev, sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;
	t->spi = spi;
	mutex_init(&t->spi_lock);
	mutex_init(&t->frame_lock);
	mutex_init(&t->irq_lock);
	INIT_LIST_HEAD(&t->frames);
	init_waitqueue_head(&t->frame_waitq);
	init_waitqueue_head(&t->status_waitq);
	init_completion(&t->stylus3_done);
	init_completion(&t->callback_done);
	spin_lock_init(&t->callback_lock);
	mutex_init(&t->nvt_cmd_lock);
	t->nvt_pen_cmd = of_property_read_bool(dev->of_node, "huawei,send-pen-status-to-fw");

	if (of_property_read_string(dev->of_node, "huawei,chip-info", &t->chip_info))
		return dev_err_probe(dev, -EINVAL, "huawei,chip-info missing\n");
	t->magnification = 8;
	of_property_read_u32(dev->of_node, "huawei,finger-magnification", &t->magnification);
	t->input_cfg = (struct thp_input_config){
		.abs_max_x = 1600, .abs_max_y = 2560, .abs_max_z = 100,
		.tracking_id_max = THP_MAX_FINGERS, .major_max = 1, .minor_max = 1,
		.orientation_min = -90, .orientation_max = 90, .tool_type_max = 1,
	};
	of_property_read_u32(dev->of_node, "touchscreen-size-x", &t->input_cfg.abs_max_x);
	of_property_read_u32(dev->of_node, "touchscreen-size-y", &t->input_cfg.abs_max_y);

	/* Level 1 = controller running, as Huawei's RESET ioctl expects. */
	t->rst_gpio = devm_gpiod_get(dev, "rst", GPIOD_OUT_HIGH);
	if (IS_ERR(t->rst_gpio))
		return dev_err_probe(dev, PTR_ERR(t->rst_gpio), "rst gpio\n");

	t->frame_tx = devm_kzalloc(dev, THP_MAX_FRAME_SIZE, GFP_KERNEL);
	t->frame_rx = devm_kzalloc(dev, THP_MAX_FRAME_SIZE, GFP_KERNEL);
	t->sync_tx = devm_kzalloc(dev, THP_SYNC_DATA_MAX, GFP_KERNEL);
	t->sync_rx = devm_kzalloc(dev, THP_SYNC_DATA_MAX, GFP_KERNEL);
	t->cmd_tx = devm_kzalloc(dev, 16, GFP_KERNEL);
	t->cmd_rx = devm_kzalloc(dev, 16, GFP_KERNEL);
	if (!t->frame_tx || !t->frame_rx || !t->sync_tx || !t->sync_rx ||
	    !t->cmd_tx || !t->cmd_rx)
		return -ENOMEM;

	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret)
		return dev_err_probe(dev, ret, "spi setup\n");

	t->irq = spi->irq;
	ret = devm_request_threaded_irq(dev, t->irq, NULL, thp_irq_thread,
					IRQF_ONESHOT | IRQF_NO_AUTOEN, "thp", t);
	if (ret)
		return dev_err_probe(dev, ret, "irq\n");

	t->status = BIT(THP_STATUS_POWER) | BIT(THP_STATUS_CHARGER) |
		    BIT(THP_STATUS_WINDOW_UPDATE) | BIT(THP_STATUS_TOUCH_SCENE);
	atomic_set(&t->status_updated, 1);
	g_thp = t;

	if (drm_is_panel_follower(dev)) {
		t->panel_follower.funcs = &thp_panel_follower_funcs;
		ret = drm_panel_add_follower(dev, &t->panel_follower);
		if (ret) {
			g_thp = NULL;
			return dev_err_probe(dev, ret, "panel follower\n");
		}
	}

	ret = thp_input_register(t);
	if (ret)
		goto err;
	ret = thp_pen_register(t);
	if (ret)
		goto err;

	t->thp_misc = (struct miscdevice){
		.minor = MISC_DYNAMIC_MINOR, .name = "thp", .fops = &thp_fops,
	};
	t->mt_misc = (struct miscdevice){
		.minor = MISC_DYNAMIC_MINOR, .name = "input_mt_wrapper", .fops = &mt_fops,
	};
	ret = misc_register(&t->thp_misc);
	if (ret)
		goto err;
	ret = misc_register(&t->mt_misc);
	if (ret)
		goto err_thp_misc;

	/* /sys/touchscreen -> /sys/devices/platform/huawei_thp, as on 4.19. */
	t->sysfs_pdev = platform_device_register_full(&(struct platform_device_info){
		.name = "huawei_thp", .id = PLATFORM_DEVID_NONE,
	});
	if (IS_ERR(t->sysfs_pdev)) {
		ret = PTR_ERR(t->sysfs_pdev);
		goto err_mt_misc;
	}
	ret = sysfs_create_groups(&t->sysfs_pdev->dev.kobj, thp_sysfs_groups);
	if (ret)
		goto err_pdev;
	ret = sysfs_create_link(NULL, &t->sysfs_pdev->dev.kobj, "touchscreen");
	if (ret)
		dev_warn(dev, "no /sys/touchscreen link: %d\n", ret);

	spi_set_drvdata(spi, t);
	dev_info(dev, "THP host interface for %s, SPI %u Hz, irq %d\n",
		 t->chip_info, spi->max_speed_hz, t->irq);
	return 0;

err_pdev:
	platform_device_unregister(t->sysfs_pdev);
err_mt_misc:
	misc_deregister(&t->mt_misc);
err_thp_misc:
	misc_deregister(&t->thp_misc);
err:
	if (t->panel_follower.funcs)
		drm_panel_remove_follower(&t->panel_follower);
	g_thp = NULL;
	return ret;
}

static void thp_remove(struct spi_device *spi)
{
	struct thp *t = spi_get_drvdata(spi);

	if (t->panel_follower.funcs)
		drm_panel_remove_follower(&t->panel_follower);
	sysfs_remove_link(NULL, "touchscreen");
	sysfs_remove_groups(&t->sysfs_pdev->dev.kobj, thp_sysfs_groups);
	platform_device_unregister(t->sysfs_pdev);
	misc_deregister(&t->mt_misc);
	misc_deregister(&t->thp_misc);
	mutex_lock(&t->frame_lock);
	thp_clear_frames(t);
	mutex_unlock(&t->frame_lock);
	g_thp = NULL;
}

static const struct of_device_id thp_of_match[] = {
	{ .compatible = "huawei,dby-w09-thp" },
	{ }
};
MODULE_DEVICE_TABLE(of, thp_of_match);

static const struct spi_device_id thp_spi_ids[] = {
	{ "dby-w09-thp" },
	{ }
};
MODULE_DEVICE_TABLE(spi, thp_spi_ids);

static struct spi_driver thp_driver = {
	.driver = {
		.name = "huawei-thp-lite",
		.of_match_table = thp_of_match,
	},
	.id_table = thp_spi_ids,
	.probe = thp_probe,
	.remove = thp_remove,
};
module_spi_driver(thp_driver);

MODULE_DESCRIPTION("Huawei THP host interface (aptouch_daemon) for the MatePad 11");
MODULE_LICENSE("GPL");
