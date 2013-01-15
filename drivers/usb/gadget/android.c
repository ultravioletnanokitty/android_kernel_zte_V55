/*
 * Gadget Driver for Android
 *
 * Copyright (C) 2008 Google, Inc.
 * Copyright (c) 2009-2010, Code Aurora Forum. All rights reserved.
 * Author: Mike Lockwood <lockwood@android.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

/* #define DEBUG */
/* #define VERBOSE_DEBUG */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/utsname.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/debugfs.h>
#include <linux/types.h>
#include <linux/usb/android_composite.h>
#include <linux/usb/ch9.h>
#include <linux/usb/composite.h>
#include <linux/usb/gadget.h>

#include "gadget_chips.h"
#include "u_serial.h"
#include <linux/miscdevice.h>
#include <linux/wakelock.h>
/*
 * Kbuild is not very cooperative with respect to linking separately
 * compiled library objects into one module.  So for now we won't use
 * separate compilation ... ensuring init/exit sections work to shrink
 * the runtime footprint, and giving us at least some parts of what
 * a "gcc --combine ... part1.c part2.c part3.c ... " build would.
 */
#include "usbstring.c"
#include "config.c"
#include "epautoconf.c"
#include "composite.c"

MODULE_AUTHOR("Mike Lockwood");
MODULE_DESCRIPTION("Android Composite USB Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");

static const char longname[] = "Gadget Android";

/* Default vendor and product IDs, overridden by platform data */
#define VENDOR_ID		0x18D1
#define PRODUCT_ID		0x0001

struct android_dev {
	struct usb_composite_dev *cdev;
	struct usb_configuration *config;
	int num_products;
	struct android_usb_product *products;
	int num_functions;
	char **functions;

	int vendor_id;
	int product_id;
	int version;
};

int is_usbdebugging_enabled_before_plugoff = 0;

void schedule_cdrom_stop(void);
struct usb_ex_work
{
	struct workqueue_struct *workqueue;
	int    enable_switch;
	int    enable_linux_switch;
	int switch_pid;
	int has_switch;
	int cur_pid;
	int linux_pid;
	struct delayed_work switch_work;
	struct delayed_work linux_switch_work;
	struct delayed_work plug_work;
	spinlock_t lock;
	struct wake_lock	wlock;
};

struct usb_ex_work global_usbwork = {0};
static int create_usb_work_queue(void);
static int destroy_usb_work_queue(void);



static struct android_dev *_android_dev;

#define MAX_STR_LEN		16
/* string IDs are assigned dynamically */

#define STRING_MANUFACTURER_IDX		0
#define STRING_PRODUCT_IDX		1
#define STRING_SERIAL_IDX		2

char serial_number[MAX_STR_LEN];
/* String Table */
static struct usb_string strings_dev[] = {
	/* These dummy values should be overridden by platform data */
	[STRING_MANUFACTURER_IDX].s = "Android",
	[STRING_PRODUCT_IDX].s = "Android",
	[STRING_SERIAL_IDX].s = "0123456789ABCDEF",
	{  }			/* end of list */
};

static struct usb_gadget_strings stringtab_dev = {
	.language	= 0x0409,	/* en-us */
	.strings	= strings_dev,
};

static struct usb_gadget_strings *dev_strings[] = {
	&stringtab_dev,
	NULL,
};

static struct usb_device_descriptor device_desc = {
	.bLength              = sizeof(device_desc),
	.bDescriptorType      = USB_DT_DEVICE,
	.bcdUSB               = __constant_cpu_to_le16(0x0200),
	.bDeviceClass         = USB_CLASS_PER_INTERFACE,
	.idVendor             = __constant_cpu_to_le16(VENDOR_ID),
	.idProduct            = __constant_cpu_to_le16(PRODUCT_ID),
	.bcdDevice            = __constant_cpu_to_le16(0xffff),
	.bNumConfigurations   = 1,
};

static struct usb_otg_descriptor otg_descriptor = {
	.bLength =		sizeof otg_descriptor,
	.bDescriptorType =	USB_DT_OTG,
	.bmAttributes =		USB_OTG_SRP | USB_OTG_HNP,
	.bcdOTG               = __constant_cpu_to_le16(0x0200),
};

static const struct usb_descriptor_header *otg_desc[] = {
	(struct usb_descriptor_header *) &otg_descriptor,
	NULL,
};

static struct list_head _functions = LIST_HEAD_INIT(_functions);
static int _registered_function_count = 0;

static void android_set_default_product(int product_id);

void android_usb_set_connected(int connected)
{
	if (_android_dev && _android_dev->cdev && _android_dev->cdev->gadget) {
		if (connected)
			usb_gadget_connect(_android_dev->cdev->gadget);
		else
			usb_gadget_disconnect(_android_dev->cdev->gadget);
	}
}

static struct android_usb_function *get_function(const char *name)
{
	struct android_usb_function	*f;
	list_for_each_entry(f, &_functions, list) {
		if (!strcmp(name, f->name))
			return f;
	}
	return 0;
}

static void bind_functions(struct android_dev *dev)
{
	struct android_usb_function	*f;
	char **functions = dev->functions;
	int i;

	for (i = 0; i < dev->num_functions; i++) {
		char *name = *functions++;
		f = get_function(name);
		if (f)
			f->bind_config(dev->config);
		else
			pr_err("%s: function %s not found\n", __func__, name);
	}

	/*
	 * set_alt(), or next config->bind(), sets up
	 * ep->driver_data as needed.
	 */
	usb_ep_autoconfig_reset(dev->cdev->gadget);
}

static int __ref android_bind_config(struct usb_configuration *c)
{
	struct android_dev *dev = _android_dev;

	pr_debug("android_bind_config\n");
	dev->config = c;

	/* bind our functions if they have all registered */
	if (_registered_function_count == dev->num_functions)
		bind_functions(dev);

	return 0;
}

static int android_setup_config(struct usb_configuration *c,
		const struct usb_ctrlrequest *ctrl);

static struct usb_configuration android_config_driver = {
	.label		= "android",
	.bind		= android_bind_config,
	.setup		= android_setup_config,
	.bConfigurationValue = 1,
	.bMaxPower	= 0xFA, /* 500ma */
};

static int android_setup_config(struct usb_configuration *c,
		const struct usb_ctrlrequest *ctrl)
{
	int i;
	int ret = -EOPNOTSUPP;

	for (i = 0; i < android_config_driver.next_interface_id; i++) {
		if (android_config_driver.interface[i]->setup) {
			ret = android_config_driver.interface[i]->setup(
				android_config_driver.interface[i], ctrl);
			if (ret >= 0)
				return ret;
		}
	}
	return ret;
}

static int product_has_function(struct android_usb_product *p,
		struct usb_function *f)
{
	char **functions = p->functions;
	int count = p->num_functions;
	const char *name = f->name;
	int i;

	for (i = 0; i < count; i++) {
		if (!strcmp(name, *functions++))
			return 1;
	}
	return 0;
}

static int product_matches_functions(struct android_usb_product *p)
{
	struct usb_function		*f;
	list_for_each_entry(f, &android_config_driver.functions, list) {
		if (product_has_function(p, f) == !!f->disabled)
			return 0;
	}
	return 1;
}
#if 0
static int get_vendor_id(struct android_dev *dev)
{
	struct android_usb_product *p = dev->products;
	int count = dev->num_products;
	int i;

	if (p) {
		for (i = 0; i < count; i++, p++) {
			if (p->vendor_id && product_matches_functions(p))
				return p->vendor_id;
		}
	}
	/* use default vendor ID */
	return dev->vendor_id;
}
#endif

static int get_product_id(struct android_dev *dev)
{
	struct android_usb_product *p = dev->products;
	int count = dev->num_products;
	int i;

	if (p) {
		for (i = 0; i < count; i++, p++) {
			if (product_matches_functions(p))
				return p->product_id;
		}
	}
	/* use default product ID */
	return dev->product_id;
}

static int __devinit android_bind(struct usb_composite_dev *cdev)
{
	struct android_dev *dev = _android_dev;
	struct usb_gadget	*gadget = cdev->gadget;
	int			gcnum, id, ret, vendor_id, product_id;

	pr_debug("android_bind\n");

	/* Allocate string descriptor numbers ... note that string
	 * contents can be overridden by the composite_dev glue.
	 */
	id = usb_string_id(cdev);
	if (id < 0)
		return id;
	strings_dev[STRING_MANUFACTURER_IDX].id = id;
	device_desc.iManufacturer = id;

	id = usb_string_id(cdev);
	if (id < 0)
		return id;
	strings_dev[STRING_PRODUCT_IDX].id = id;
	device_desc.iProduct = id;

	id = usb_string_id(cdev);
	if (id < 0)
		return id;
	strings_dev[STRING_SERIAL_IDX].id = id;
	device_desc.iSerialNumber = id;

	if (gadget_is_otg(cdev->gadget))
		android_config_driver.descriptors = otg_desc;

	if (!usb_gadget_set_selfpowered(gadget))
		android_config_driver.bmAttributes |= USB_CONFIG_ATT_SELFPOWER;

	if (gadget->ops->wakeup)
		android_config_driver.bmAttributes |= USB_CONFIG_ATT_WAKEUP;

	/* register our configuration */
	ret = usb_add_config(cdev, &android_config_driver);
	if (ret) {
		pr_err("%s: usb_add_config failed\n", __func__);
		return ret;
	}

	gcnum = usb_gadget_controller_number(gadget);
	if (gcnum >= 0)
		device_desc.bcdDevice = cpu_to_le16(0x0200 + gcnum);
	else {
		/* gadget zero is so simple (for now, no altsettings) that
		 * it SHOULD NOT have problems with bulk-capable hardware.
		 * so just warn about unrcognized controllers -- don't panic.
		 *
		 * things like configuration and altsetting numbering
		 * can need hardware-specific attention though.
		 */
		pr_warning("%s: controller '%s' not recognized\n",
			longname, gadget->name);
		device_desc.bcdDevice = __constant_cpu_to_le16(0x9999);
	}

	usb_gadget_set_selfpowered(gadget);
	dev->cdev = cdev;

	#if 0	
	device_desc.idVendor = __constant_cpu_to_le16(get_vendor_id(dev));
	device_desc.idProduct = __constant_cpu_to_le16(get_product_id(dev));
	cdev->desc.idVendor = device_desc.idVendor;
	cdev->desc.idProduct = device_desc.idProduct;
	#else
	product_id = get_product_id(dev);
	if(product_id == 0x2D00 || product_id == 0x2D01)
	{
		vendor_id = 0x18D1;
	}
	else
	{
		vendor_id = 0x19D2;
	}
	device_desc.idVendor = __constant_cpu_to_le16(vendor_id);
	device_desc.idProduct = __constant_cpu_to_le16(product_id);
	cdev->desc.idVendor = device_desc.idVendor;
	cdev->desc.idProduct = device_desc.idProduct;
	#endif

	return 0;
}

static struct usb_composite_driver android_usb_driver = {
	.name		= "android_usb",
	.dev		= &device_desc,
	.strings	= dev_strings,
	.bind		= android_bind,
	.enable_function = android_enable_function,
};

static bool is_func_supported(struct android_usb_function *f)
{
	char **functions = _android_dev->functions;
	int count = _android_dev->num_functions;
	const char *name = f->name;
	int i;

	for (i = 0; i < count; i++) {
		if (!strcmp(*functions++, name))
			return true;
	}
	return false;
}

void android_register_function(struct android_usb_function *f)
{
	struct android_dev *dev = _android_dev;

	pr_debug("%s: %s\n", __func__, f->name);

	if (!is_func_supported(f))
		return;

	list_add_tail(&f->list, &_functions);
	_registered_function_count++;

	/* bind our functions if they have all registered
	 * and the main driver has bound.
	 */
	if (dev->config && _registered_function_count == dev->num_functions) {
		bind_functions(dev);
		android_set_default_product(dev->product_id);
	}
}

/**
 * android_set_function_mask() - enables functions based on selected pid.
 * @up: selected product id pointer
 *
 * This function enables functions related with selected product id.
 */
static void android_set_function_mask(struct android_usb_product *up)
{
	int index, found = 0;
	struct usb_function *func;

	list_for_each_entry(func, &android_config_driver.functions, list) {
		/* adb function enable/disable handled separetely */
		if (!strcmp(func->name, "adb") && !func->disabled)
			continue;

		for (index = 0; index < up->num_functions; index++) {
			if (!strcmp(up->functions[index], func->name)) {
				found = 1;
				break;
			}
		}

		if (found) { /* func is part of product. */
			/* if func is disabled, enable the same. */
			if (func->disabled)
				usb_function_set_enabled(func, 1);
			found = 0;
		} else { /* func is not part if product. */
			/* if func is enabled, disable the same. */
			if (!func->disabled)
				usb_function_set_enabled(func, 0);
		}
	}
}

/**
 * android_set_defaut_product() - selects default product id and enables
 * required functions
 * @product_id: default product id
 *
 * This function selects default product id using pdata information and
 * enables functions for same.
*/
static void android_set_default_product(int pid)
{
	struct android_dev *dev = _android_dev;
	struct android_usb_product *up = dev->products;
	int index;

	for (index = 0; index < dev->num_products; index++, up++) {
		if (pid == up->product_id)
			break;
	}
	android_set_function_mask(up);
}

/**
 * android_config_functions() - selects product id based on function need
 * to be enabled / disabled.
 * @f: usb function
 * @enable : function needs to be enable or disable
 *
 * This function selects first product id having required function.
 * RNDIS/MTP function enable/disable uses this.
*/
#ifdef CONFIG_USB_ANDROID_RNDIS
static void android_config_functions(struct usb_function *f, int enable)
{
	struct android_dev *dev = _android_dev;
	struct android_usb_product *up = dev->products;
	int index;

	/* Searches for product id having function */
	if (enable) {
		for (index = 0; index < dev->num_products; index++, up++) {
			if (product_has_function(up, f))
				break;
		}
		android_set_function_mask(up);
	} else
		android_set_default_product(dev->product_id);
}
#endif

void update_dev_desc(struct android_dev *dev)
{
	struct usb_function *f;
	struct usb_function *last_enabled_f = NULL;
	int num_enabled = 0;
	int has_iad = 0;

	dev->cdev->desc.bDeviceClass = USB_CLASS_PER_INTERFACE;
	dev->cdev->desc.bDeviceSubClass = 0x00;
	dev->cdev->desc.bDeviceProtocol = 0x00;

	list_for_each_entry(f, &android_config_driver.functions, list) {
		if (!f->disabled) {
			num_enabled++;
			last_enabled_f = f;
			if (f->descriptors[0]->bDescriptorType ==
					USB_DT_INTERFACE_ASSOCIATION)
				has_iad = 1;
		}
		if (num_enabled > 1 && has_iad) {
			dev->cdev->desc.bDeviceClass = USB_CLASS_MISC;
			dev->cdev->desc.bDeviceSubClass = 0x02;
			dev->cdev->desc.bDeviceProtocol = 0x01;
			break;
		}
	}

	if (num_enabled == 1) {
#ifdef CONFIG_USB_ANDROID_RNDIS
		if (!strcmp(last_enabled_f->name, "rndis")) {
#ifdef CONFIG_USB_ANDROID_RNDIS_WCEIS
			dev->cdev->desc.bDeviceClass =
					USB_CLASS_WIRELESS_CONTROLLER;
#else
			dev->cdev->desc.bDeviceClass = USB_CLASS_COMM;
#endif
		}
#endif
	}
}


static char *sysfs_allowed[] = {
	"rndis",
	"mtp",
	"adb",
	"accessory",
	"diag",
	"modem",
	"nmea",
	"rmnet",
	"rmnet_smd_sdio",
	"usb_mass_storage"
};

static int is_sysfschange_allowed(struct usb_function *f)
{
	char **functions = sysfs_allowed;
	int count = ARRAY_SIZE(sysfs_allowed);
	int i;

	for (i = 0; i < count; i++) {
		if (!strncmp(f->name, functions[i], 32))
			return 1;
	}
	return 0;
}

int android_enable_function(struct usb_function *f, int enable)
{
	struct android_dev *dev = _android_dev;
	int disable = !enable;
	int product_id;
	int vendor_id;
	printk("liuyuanyuan android_enable_function, f->name %s, !!f->disabled=%d, disable=%d\n", f->name, !!f->disabled, disable);

	if (!is_sysfschange_allowed(f))
		return -EINVAL;
	if (!!f->disabled != disable) {
		usb_function_set_enabled(f, !disable);

#ifdef CONFIG_USB_ANDROID_RNDIS
		if (!strcmp(f->name, "rndis")) {

			/* We need to specify the COMM class in the device descriptor
			 * if we are using RNDIS.
			 */
			if (enable) {
#ifdef CONFIG_USB_ANDROID_RNDIS_WCEIS
				dev->cdev->desc.bDeviceClass = USB_CLASS_MISC;
				dev->cdev->desc.bDeviceSubClass      = 0x02;
				dev->cdev->desc.bDeviceProtocol      = 0x01;
#else
				dev->cdev->desc.bDeviceClass = USB_CLASS_COMM;
#endif
			} else {
				dev->cdev->desc.bDeviceClass = USB_CLASS_PER_INTERFACE;
				dev->cdev->desc.bDeviceSubClass      = 0;
				dev->cdev->desc.bDeviceProtocol      = 0;
			}

			android_config_functions(f, enable);
		}
#endif
#ifdef CONFIG_USB_ANDROID_ACCESSORY
		if (!strncmp(f->name, "accessory", 32))
			android_config_functions(f, enable);
#endif

#ifdef CONFIG_USB_ANDROID_MTP
		if (!strcmp(f->name, "mtp"))
			android_config_functions(f, enable);
#endif

		if (!strcmp(f->name, "diag"))
		{
			android_config_functions(f, enable);
		}
		if (!strcmp(f->name, "modem"))
		{
			android_config_functions(f, enable);
		}
		if (!strcmp(f->name, "rmnet"))
		{
			android_config_functions(f, enable);
		}
		if (!strcmp(f->name, "rmnet_smd_sdio"))
		{
			android_config_functions(f, enable);
		}
		if (!strcmp(f->name, "nmea"))
		{
			android_config_functions(f, enable);
		}
	
		//device_desc.idVendor = __constant_cpu_to_le16(get_vendor_id(dev));
		product_id = get_product_id(dev);
		printk("liuyuanyuan PID=0x%x\n", product_id);
		if(product_id == 0x2D00 || product_id == 0x2D01)
		{
			vendor_id = 0x18D1;
		}
		else
		{
			vendor_id = 0x19D2;
		}
		device_desc.idVendor = __constant_cpu_to_le16(vendor_id);
		device_desc.idProduct = __constant_cpu_to_le16(product_id);

		if (dev->cdev) {
			dev->cdev->desc.idVendor = device_desc.idVendor;
			dev->cdev->desc.idProduct = device_desc.idProduct;
		}
		usb_composite_force_reset(dev->cdev);
		#if 0
		if (!strcmp(f->name, "rndis") && (enable==0) )
		{
			printk("liuyuanyuan detect rndis disable operation, enable rndis automatically in enable_mtp()\n");
			enable_mtp();
		}
		#endif
	}
	return 0;
}


void switch_from_ms_to_mtp(void)
{
	struct usb_function *f;

	printk("liuyuanyuan get rid of usb_mass_storage function in switch_from_ms_to_mtp()\n");
	list_for_each_entry(f, &android_config_driver.functions, list)
	{
		if(!strcmp(f->name, "usb_mass_storage"))
			android_enable_function(f, 0);
	}

	printk("liuyuanyuan enable mtp function in switch_from_ms_to_mtp\n");
	list_for_each_entry(f, &android_config_driver.functions, list)
	{
		if(!strcmp(f->name, "mtp"))
			android_enable_function(f, 1);
	}

	if(is_usbdebugging_enabled_before_plugoff == 1 )
	{
		list_for_each_entry(f, &android_config_driver.functions, list)
		{
			if(!strcmp(f->name, "adb"))
				android_enable_function(f, 1);
		}
	}

}
void enable_mtp(void)
{
	struct usb_function *f;

	list_for_each_entry(f, &android_config_driver.functions, list)
	{
		if(!strcmp(f->name, "mtp"))
			android_enable_function(f, 1);
	}
}
int is_diag_enabled(void)
{
	struct usb_function *f;

	printk("liuyuanyuan try to get whether diag is enabled\n");
	list_for_each_entry(f, &android_config_driver.functions, list)
	{
		if(!strcmp(f->name, "diag"))
		{			
			printk("liuyuanyuan gets /sys/class/usb_composite/diag/enable is <0x%x>\n", !f->disabled);
			return !f->disabled;
		}
	}
	printk("liuyuanyuan fatal error diag function not found\n");
	return -1;
}
int is_accessory_enabled(void)
{
	struct usb_function *f;

	printk("liuyuanyuan try to get whether diag is enabled\n");
	list_for_each_entry(f, &android_config_driver.functions, list)
	{
		if(!strcmp(f->name, "accessory"))
		{			
			printk("liuyuanyuan gets /sys/class/usb_composite/accessory/enable is <0x%x>\n", !f->disabled);
			return !f->disabled;
		}
	}
	printk("liuyuanyuan fatal error accessory function not found\n");
	return -1;
}
void switchback_to_usbmassstorage(void)
{
	struct usb_function *f;
	if( (is_diag_enabled()==0)
	&& (is_accessory_enabled()==0)  )   //diag function not enabled, go switching!
	{
		list_for_each_entry(f, &android_config_driver.functions, list)
		{
			if(!strcmp(f->name, "adb"))
				is_usbdebugging_enabled_before_plugoff = !f->disabled;
		}
		printk("liuyuanyuan is_usbdebugging_enabled_before_plugoff is <%d>\n", is_usbdebugging_enabled_before_plugoff);
	
		printk("liuyuanyuan disable rndis in switchback_to_usbmassstorage\n");
		list_for_each_entry(f, &android_config_driver.functions, list)
		{
			if(!strcmp(f->name, "rndis"))
			android_enable_function(f, 0);
		}
		
		printk("liuyuanyuan disable adb in switchback_to_usbmassstorage\n");
		list_for_each_entry(f, &android_config_driver.functions, list)
		{
			if(!strcmp(f->name, "adb"))
			android_enable_function(f, 0);
		}
		
		printk("liuyuanyuan disable mtp in switchback_to_usbmassstorage\n");
		list_for_each_entry(f, &android_config_driver.functions, list)
		{
			if(!strcmp(f->name, "mtp"))
			android_enable_function(f, 0);
		}

		printk("liuyuanyuan enable usb_mass_storage in switchback_to_usbmassstorage\n");
		list_for_each_entry(f, &android_config_driver.functions, list)
		{
			if(!strcmp(f->name, "usb_mass_storage"))
			android_enable_function(f, 1);
		}
	}
}


#ifdef CONFIG_DEBUG_FS
static int android_debugfs_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return 0;
}

static ssize_t android_debugfs_serialno_write(struct file *file, const char
				__user *buf,	size_t count, loff_t *ppos)
{
	char str_buf[MAX_STR_LEN];

	if (count > MAX_STR_LEN)
		return -EFAULT;

	if (copy_from_user(str_buf, buf, count))
		return -EFAULT;

	memcpy(serial_number, str_buf, count);

	if (serial_number[count - 1] == '\n')
		serial_number[count - 1] = '\0';

	strings_dev[STRING_SERIAL_IDX].s = serial_number;

	return count;
}
const struct file_operations android_fops = {
	.open	= android_debugfs_open,
	.write	= android_debugfs_serialno_write,
};

struct dentry *android_debug_root;
struct dentry *android_debug_serialno;

static int android_debugfs_init(struct android_dev *dev)
{
	android_debug_root = debugfs_create_dir("android", NULL);
	if (!android_debug_root)
		return -ENOENT;

	android_debug_serialno = debugfs_create_file("serial_number", 0222,
						android_debug_root, dev,
						&android_fops);
	if (!android_debug_serialno) {
		debugfs_remove(android_debug_root);
		android_debug_root = NULL;
		return -ENOENT;
	}
	return 0;
}

static void android_debugfs_cleanup(void)
{
       debugfs_remove(android_debug_serialno);
       debugfs_remove(android_debug_root);
}
#endif
static int __init android_probe(struct platform_device *pdev)
{
	struct android_usb_platform_data *pdata = pdev->dev.platform_data;
	struct android_dev *dev = _android_dev;
	int result;

	dev_dbg(&pdev->dev, "%s: pdata: %p\n", __func__, pdata);

	pm_runtime_set_active(&pdev->dev);
	pm_runtime_enable(&pdev->dev);

	result = pm_runtime_get(&pdev->dev);
	if (result < 0) {
		dev_err(&pdev->dev,
			"Runtime PM: Unable to wake up the device, rc = %d\n",
			result);
		return result;
	}

	if (pdata) {
		dev->products = pdata->products;
		dev->num_products = pdata->num_products;
		dev->functions = pdata->functions;
		dev->num_functions = pdata->num_functions;
		if (pdata->vendor_id) {
			dev->vendor_id = pdata->vendor_id;
			device_desc.idVendor =
				__constant_cpu_to_le16(pdata->vendor_id);
		}
		if (pdata->product_id) {
			dev->product_id = pdata->product_id;
			device_desc.idProduct =
				__constant_cpu_to_le16(pdata->product_id);
		}
		if (pdata->version)
			dev->version = pdata->version;

		if (pdata->product_name)
			strings_dev[STRING_PRODUCT_IDX].s = pdata->product_name;
		if (pdata->manufacturer_name)
			strings_dev[STRING_MANUFACTURER_IDX].s =
					pdata->manufacturer_name;
		#if 0
		if (pdata->serial_number)
			strings_dev[STRING_SERIAL_IDX].s = pdata->serial_number;
		#else
		printk("liuyuanyuan pdata->serial_number is <%s>, pdata->product_id is <0x%x>\n", pdata->serial_number, pdata->product_id);
		if (pdata->serial_number)
		{
			if(pdata->product_id == 0x0249)
			{
				strings_dev[STRING_SERIAL_IDX].s = "1234567890ABCDEF";
			}
			else
			{
				strings_dev[STRING_SERIAL_IDX].s = pdata->serial_number;
			}
		}
		#endif
	}
#ifdef CONFIG_DEBUG_FS
	result = android_debugfs_init(dev);
	if (result)
		pr_debug("%s: android_debugfs_init failed\n", __func__);
#endif
	return usb_composite_register(&android_usb_driver);
}

static int andr_runtime_suspend(struct device *dev)
{
	dev_dbg(dev, "pm_runtime: suspending...\n");
	return 0;
}

static int andr_runtime_resume(struct device *dev)
{
	dev_dbg(dev, "pm_runtime: resuming...\n");
	return 0;
}

static struct dev_pm_ops andr_dev_pm_ops = {
	.runtime_suspend = andr_runtime_suspend,
	.runtime_resume = andr_runtime_resume,
};

static struct platform_driver android_platform_driver = {
	.driver = { .name = "android_usb", .pm = &andr_dev_pm_ops},
};

static int __init init(void)
{
	struct android_dev *dev;

	pr_debug("android init\n");

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	create_usb_work_queue();

	/* set default values, which should be overridden by platform data */
	dev->product_id = PRODUCT_ID;
	_android_dev = dev;

	return platform_driver_probe(&android_platform_driver, android_probe);
}
module_init(init);

static void __exit cleanup(void)
{
#ifdef CONFIG_DEBUG_FS
	android_debugfs_cleanup();
#endif
	usb_composite_unregister(&android_usb_driver);
	platform_driver_unregister(&android_platform_driver);
	kfree(_android_dev);
	_android_dev = NULL;

	destroy_usb_work_queue();

	
}
module_exit(cleanup);



static void usb_switch_work(struct work_struct *w)
{
	struct usb_ex_work *p = container_of(w, struct usb_ex_work, switch_work.work);
	//unsigned long flags;
	//if (!_android_dev) {

		//printk(KERN_ERR"usb:%s: %d: _android_dev == NULL\n",
		      // __FUNCTION__, __LINE__);
		//return ;
	//}
	//if (!p->enable_switch) {
		//return ;
	//}
	//if (p->has_switch) {
		//printk("usb:rms:%s %d: already switch pid 0x%x switch_pid 0x%x\n",
		      // __FUNCTION__, __LINE__, current_pid(), p->switch_pid);
		//return ;
	//}
	//spin_lock_irqsave(&p->lock, flags);
//	p->cur_pid = ui->composition->product_id;
	//p->has_switch = 1;
	//spin_unlock_irqrestore(&p->lock, flags);
//	DBG("auto switch usb mode");
	//printk("usb:rms:%s %d: pid 0x%x switch_pid 0x%x\n",
	       //__FUNCTION__, __LINE__, current_pid(), p->switch_pid);
	//enable_cdrom(0);

	//mutex_lock(&_android_dev->lock);
	wake_lock(&p->wlock);
	//android_switch_composition((unsigned short)p->switch_pid);
	switch_from_ms_to_mtp();
	wake_unlock(&p->wlock);
	//mutex_unlock(&_android_dev->lock);

	return ;
}
void schedule_cdrom_stop(void)
{
	
	//if (NULL == global_usbwork.workqueue) {
		//return ;
	//}
	queue_delayed_work(global_usbwork.workqueue, &global_usbwork.switch_work, HZ/10);

	return;
}
EXPORT_SYMBOL(schedule_cdrom_stop);
static int create_usb_work_queue(void)
{
	struct usb_ex_work *p = &global_usbwork;
	if (p->workqueue) {
		printk(KERN_ERR"usb:workqueue has created");
		return 0;
	}
	//spin_lock_init(&p->lock);
	//p->enable_switch = 1;
	//p->enable_linux_switch = 0;
	//p->switch_pid = PRODUCT_ID_MS_ADB;
	//p->linux_pid = PRODUCT_ID_MS_ADB;
	//p->cur_pid = PRODUCT_ID_MS_CDROM;
	//p->has_switch = 0;
	p->workqueue = create_singlethread_workqueue("usb_workqueue");
	//if (NULL == p->workqueue) {
		//printk(KERN_ERR"usb:workqueue created fail");
		//p->enable_switch = 0;
		//return -1;
	//}
	INIT_DELAYED_WORK(&p->switch_work, usb_switch_work);
	//INIT_DELAYED_WORK(&p->linux_switch_work, usb_switch_os_work);
	//INIT_DELAYED_WORK(&p->plug_work, usb_plug_work);
	wake_lock_init(&p->wlock,
		       WAKE_LOCK_SUSPEND, "usb_switch_wlock");
	return 0;
}
static int destroy_usb_work_queue(void)
{
	struct usb_ex_work *p = &global_usbwork;
	if (NULL != p->workqueue) {
		destroy_workqueue(p->workqueue);
		p->workqueue = NULL;
	}
	wake_lock_destroy(&p->wlock);
	memset(&global_usbwork, 0, sizeof(global_usbwork));
	return 0;
}

