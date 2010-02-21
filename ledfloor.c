/* Copyright 2010 Benjamin Poirier, benjamin.poirier@gmail.com
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <asm/atomic.h>
#include <asm/uaccess.h>
#include <linux/cdev.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/wait.h>

#include "ledfloor.h"

#ifdef CONFIG_AVR32
#include <asm/io.h>
#include <asm/sysreg.h>
#include <linux/gpio.h>
#include <mach/at32ap700x.h>

/* Out of arch/avr32/mach-at32ap/pio.h */
#define PIO_SODR 0x0030 // Set Output Data Register
#define PIO_CODR 0x0034 // Clear Output Data Register
#define PIO_ODSR 0x0038 // Output Data Status Register
#define PIO_OWER 0x00a0 // Output Write Enable Register
#define PIO_OWSR 0x00a8 // Output Write Status Register

#endif


static struct platform_device *ledfloor_gpio_device;
static struct class *ledfloor_class;
static struct ledfloor_dev_t {
	const struct ledfloor_config *config;

	uint8_t buffer[LFCOLS * 3 * LFROWS];

	dev_t devid;
	struct cdev cdev;
	wait_queue_head_t wq;
	atomic_t fnum;
} dev = {
	.wq = __WAIT_QUEUE_HEAD_INITIALIZER(dev.wq),
	.fnum = ATOMIC_INIT(0),
};
static struct ledfloor_config
{
	int blank;
	int latch;
	int clk;
	int data[24];
} ledfloor_config_data = {
#ifdef CONFIG_AVR32
	.blank = GPIO_PIN_PA(29),
	.latch = GPIO_PIN_PA(30),
	.clk = GPIO_PIN_PA(31),
	.data = {
		GPIO_PIN_PB(0),
		GPIO_PIN_PB(1),
		GPIO_PIN_PB(2),
		GPIO_PIN_PB(3),
		GPIO_PIN_PB(4),
		GPIO_PIN_PB(5),
		GPIO_PIN_PB(6),
		GPIO_PIN_PB(7),
		GPIO_PIN_PB(8),
		GPIO_PIN_PB(9),
		GPIO_PIN_PB(10),
		GPIO_PIN_PB(11),
		GPIO_PIN_PB(12),
		GPIO_PIN_PB(13),
		GPIO_PIN_PB(14),
		GPIO_PIN_PB(15),
		GPIO_PIN_PB(16),
		GPIO_PIN_PB(17),
		GPIO_PIN_PB(18),
		GPIO_PIN_PB(19),
		GPIO_PIN_PB(20),
		GPIO_PIN_PB(21),
		GPIO_PIN_PB(22),
		GPIO_PIN_PB(23),
	},
#else
#endif
};


#ifdef CONFIG_AVR32
static int clk_mask, latch_mask;
static void *clk_reg_set, *clk_reg_clear;
static void *latch_reg_set, *latch_reg_clear;

static int __init gpio_init(const struct ledfloor_config *config)
{
	unsigned int i;
	int errno;

	if ((errno = gpio_direction_output(config->blank, 0))) {
		printk(KERN_ERR "ledfloor gpio_init, failed to "
			"register blank line\n");
		return errno;
	}
	if ((errno = gpio_direction_output(config->latch, 0))) {
		printk(KERN_ERR "ledfloor gpio_init, failed to "
			"register latch line\n");
		return errno;
	}
	if ((errno = gpio_direction_output(config->clk, 0))) {
		printk(KERN_ERR "ledfloor gpio_init, failed to "
			"register clock line\n");
		return errno;
	}

	for (i = 0; i < ARRAY_SIZE(config->data); i++)
	{
		if ((errno = gpio_direction_output(config->data[i], 0))) {
			printk(KERN_ERR "ledfloor gpio_init, failed to "
				"register data line %d\n", i);
			return errno;
		}
	}

	clk_mask = 1 << (config->clk % 32);
	clk_reg_set = (void*) (0xffe02800 + ((config->clk >> 5) * 0x400) +
		PIO_SODR);
	clk_reg_clear = (void*) (0xffe02800 + ((config->clk >> 5) * 0x400) +
		PIO_CODR);

	latch_mask = 1 << (config->latch % 32);
	latch_reg_set = (void*) (0xffe02800 + ((config->latch >> 5) * 0x400) +
		PIO_SODR);
	latch_reg_clear = (void*) (0xffe02800 + ((config->latch >> 5) * 0x400)
		+ PIO_CODR);

	return 0;
}


static inline void lfclock(const struct ledfloor_config *config) {
	__raw_writel(clk_mask, clk_reg_clear);
	__raw_writel(clk_mask, clk_reg_set);
	__raw_writel(clk_mask, clk_reg_clear);
}

static inline void lflatch(const struct ledfloor_config *config) {
	__raw_writel(latch_mask, latch_reg_clear);
	__raw_writel(latch_mask, latch_reg_set);
	__raw_writel(latch_mask, latch_reg_clear);
}

/* This is SUPER sketchy, it bypasses the whole gpio framework, but it's way
 * faster
 */
static void write_frame(uint8_t *buffer, const struct ledfloor_config *config)
{
	unsigned int i;
	unsigned long start;
	u32 write_mask;

	start = sysreg_read(COUNT);

	// LED "B" is active low
	gpio_set_value(GPIO_PIN_PE(19), 0);
	write_mask = __raw_readl((void*) (0xffe02800 + ((config->data[0] >> 5)
				* 0x400) + PIO_OWSR));
	__raw_writel((1 << LFROWS) - 1, (void*) (0xffe02800 + ((config->data[0] >>
					5) * 0x400) + PIO_OWER));
	for (i = 0; i < LFCOLS * 3; i++) {
		u32 values = 0;
		unsigned int row, bit;

		// todo: ajuster l'offset dans le buffer
		for (bit = 0; bit < 12; bit++) {
			for (row = 0; row < LFROWS; row++) {
				values <<= buffer[(LFROWS - row - 1) * LFCOLS * 3
					+ i] & (1 << bit);
			}

			__raw_writel(values, (void*)
				(0xffe02800 + ((config->data[0] >> 5) * 0x400)
				 + PIO_ODSR));
			lfclock(config);
		}
	}
	lflatch(config);
	__raw_writel(write_mask, (void*) (0xffe02800 + ((GPIO_PIOB_BASE >> 5) * 0x400) + PIO_OWSR));
	gpio_set_value(GPIO_PIN_PE(19), 1);

	printk(KERN_INFO "ledfloor write_frame in %lu cycles\n",
		sysreg_read(COUNT) - start);
}
#else
static int __init gpio_init(const struct ledfloor_config *config)
{
	printk(KERN_INFO "ledfloor gpio_init\n");
	return 0;
}
static void write_frame(uint8_t *buffer, const struct ledfloor_config *config)
{
	printk(KERN_INFO "ledfloor write_frame\n");
}
#endif


static int ledfloor_open(struct inode *inode, struct file *filp)
{
	struct ledfloor_dev_t *dev = container_of(inode->i_cdev, struct
		ledfloor_dev_t, cdev);

	filp->private_data = dev;

	return 0;
}


static int ledfloor_release(struct inode *inode, struct file *filp)
{
	return 0;
}


static ssize_t ledfloor_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
	struct ledfloor_dev_t *dev = filp->private_data;
	int i = atomic_read(&dev->fnum);

	if (*f_pos >= LFCOLS * 3 * LFROWS) {
		return 0;
	}
	if (*f_pos + count > LFCOLS * 3 * LFROWS) {
		count = LFCOLS * 3 * LFROWS - *f_pos;
	}

	if (!(filp->f_flags & O_NONBLOCK) && *f_pos == 0 &&
		wait_event_interruptible(dev->wq, atomic_read(&dev->fnum) != i)) {
		return -ERESTARTSYS;
	}

	if (copy_to_user(buf, &dev->buffer[*f_pos], count)) {
		return -EFAULT;
	}

	*f_pos += count;
	BUG_ON(*f_pos > LFCOLS * 3 * LFROWS);
	if (*f_pos == LFCOLS * 3 * LFROWS) {
		*f_pos = 0;
	}

	return count;
}


static ssize_t ledfloor_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
	struct ledfloor_dev_t *dev = filp->private_data;
	size_t left_to_write = count;

	if (*f_pos >= LFCOLS * 3 * LFROWS) {
		return 0;
	}

	while (left_to_write)
	{
		size_t copy_count = left_to_write;

		if (*f_pos + copy_count > LFCOLS * 3 * LFROWS) {
			copy_count = LFCOLS * 3 * LFROWS - *f_pos;
		}

		if (copy_from_user(&dev->buffer[*f_pos], buf, copy_count)) {
			return -EFAULT;
		}
		left_to_write -= copy_count;
		*f_pos += copy_count;
		BUG_ON(*f_pos > LFCOLS * 3 * LFROWS);
		if (*f_pos == LFCOLS * 3 * LFROWS) {
			*f_pos = 0;
			write_frame(dev->buffer, dev->config);
			atomic_inc(&dev->fnum);
			wake_up_interruptible(&dev->wq);
		}
	}

	return count;
}

struct file_operations ledfloor_fops = {
	.owner = THIS_MODULE,
	//.llseek = ledfloor_llseek,
	.read = ledfloor_read,
	.write = ledfloor_write,
	//.ioctl = ledfloor_ioctl,
	.open = ledfloor_open,
	.release = ledfloor_release,
};


static int __init platform_ledfloor_probe(struct platform_device *pdev)
{
	int ret;
	dev.config = pdev->dev.platform_data;
	
	dev_notice(&pdev->dev, "probe() called\n");
	
	ret= gpio_init(dev.config);
	if (ret < 0) {
		dev_warn(&pdev->dev, "gpio_init() failed\n");
		return ret;
	}

	memset(dev.buffer, 0, LFCOLS * 3 * LFROWS);

	ret = alloc_chrdev_region(&dev.devid, 0, 1, "ledfloor");
	if (ret < 0) {
		dev_warn(&pdev->dev, "ledfloor: can't get major number\n");
		return ret;
	}

	cdev_init(&dev.cdev, &ledfloor_fops);
	dev.cdev.owner = THIS_MODULE;
	dev.cdev.ops = &ledfloor_fops;

	ret = cdev_add(&dev.cdev, dev.devid, 1);
	if (ret < 0) {
		printk(KERN_WARNING "ledfloor: can't add device\n");
		return ret;
	}

	device_create(ledfloor_class, NULL, dev.devid, NULL, "ledfloor%d",
		MINOR(dev.devid));

	return 0;
}


static int __exit platform_ledfloor_remove(struct platform_device *pdev)
{
	dev_notice(&pdev->dev, "remove() called\n");

	device_destroy(ledfloor_class, dev.devid);
	cdev_del(&dev.cdev);
	unregister_chrdev_region(dev.devid, 1);

	return 0;
}


#ifdef CONFIG_PM
static int
platform_ledfloor_suspend(struct platform_device *pdev, pm_message_t state)
{
	/* Add code to suspend the device here */

	dev_notice(&pdev->dev, "suspend() called\n");

	return 0;
}

static int platform_ledfloor_resume(struct platform_device *pdev)
{
	/* Add code to resume the device here */

	dev_notice(&pdev->dev, "resume() called\n");

	return 0;
}
#else
/* No need to do suspend/resume if power management is disabled */
# define platform_ledfloor_suspend NULL
# define platform_ledfloor_resume NULL
#endif


static struct platform_driver ledfloor_driver = {
	.probe		= &platform_ledfloor_probe,
	.remove		= __exit_p(&platform_ledfloor_remove),
	.suspend	= &platform_ledfloor_suspend,
	.resume		= &platform_ledfloor_resume,
	.driver	= {
		.name	= "ledfloor",
	},
};

static int __init ledfloor_init(void)
{
	int ret;

	printk(KERN_INFO "ledfloor init\n");
	ledfloor_class = class_create(THIS_MODULE, "ledfloor");

	ret = -ENOMEM;
	ledfloor_gpio_device = platform_device_alloc("ledfloor", 0);
	if (!ledfloor_gpio_device) {
		goto fail;
	}

	/*
	 * The data is copied into a new dynamically allocated
	 * structure, so it's ok to pass variables defined on
	 * the stack here.
	 */
	ret = platform_device_add_data(ledfloor_gpio_device,
		&ledfloor_config_data, sizeof(ledfloor_config_data));
	if (ret) {
		goto fail;
	}

	printk(KERN_INFO "ledfloor registering device \"%s.%d\"...\n",
		ledfloor_gpio_device->name, ledfloor_gpio_device->id);
	ret = platform_device_add(ledfloor_gpio_device);
	if (ret) {
		goto fail;
	}

	return platform_driver_register(&ledfloor_driver);
fail:
	/*
	 * The device was never registered, so we may free it
	 * directly. Any dynamically allocated resources and
	 * platform data will be freed automatically.
	 */
	platform_device_put(ledfloor_gpio_device);

	class_destroy(ledfloor_class);

	return ret;
}
module_init(ledfloor_init);


static void __exit ledfloor_exit(void)
{
	printk(KERN_INFO "ledfloor exit\n");

	platform_driver_unregister(&ledfloor_driver);
	class_destroy(ledfloor_class);

	printk(KERN_INFO "ledfloor removing device \"%s.%d\"...\n",
		ledfloor_gpio_device->name, ledfloor_gpio_device->id);

	platform_device_del(ledfloor_gpio_device);
}
module_exit(ledfloor_exit);


MODULE_DESCRIPTION("LED dance floor framebuffer through avr32 GPIO pins");
MODULE_AUTHOR("Benjamin Poirier <benjamin.poirier@gmail.com>");
MODULE_LICENSE("GPL");
