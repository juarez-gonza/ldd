#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <asm/uaccess.h>

#include "scull.h"

struct scull_dev *scull_devs = NULL;

int scull_major = SCULL_MAJOR;
int scull_minor = 0;
int scull_nr_devs = SCULL_DEVS;
int scull_qset = SCULL_QSET;
int scull_quantum = SCULL_QUANTUM;

module_param(scull_major, int, S_IRUGO);
module_param(scull_minor, int, S_IRUGO);
module_param(scull_nr_devs, int, S_IRUGO);
module_param(scull_qset, int, S_IRUGO);
module_param(scull_quantum, int, S_IRUGO);

struct file_operations scull_fops = {
	.owner =	THIS_MODULE,
	/*
	.llseek =	scull_llseek,
	.ioctl =	scull_ioctl,
	*/
	.release =	scull_release,
	.open =		scull_open,
	.read =		scull_read,
	.write =	scull_write,
};

ssize_t scull_write(struct file *filp, const char __user *buff,
		size_t count, loff_t *f_pos)
{
	struct scull_dev *dev = filp->private_data;
	struct scull_qset *dptr;
	int quantum = dev->quantum;
	int qset = dev->qset;
	int itemsize = quantum * qset;
	int item_idx, item_offs, qt_idx, qt_offs;
	ssize_t retval = 0;

	if (down_interruptible(&dev->sem))
		return -ERESTARTSYS;

	item_idx = (long)*f_pos / itemsize;
	item_offs = (long)*f_pos % itemsize;
	qt_idx = item_offs / quantum;
	qt_offs = item_offs % quantum;

	dptr = scull_follow(dev, item_idx);
	if (dptr == NULL)
		goto out;
	if (!dptr->data) {
		dptr->data = kmalloc(qset * sizeof(char *), GFP_KERNEL);
		if (!dptr->data)
			goto out;
		memset(dptr->data, 0, qset * sizeof(char *));
	}
	if (!dptr->data[qt_idx]) {
		dptr->data[qt_idx] = kmalloc(quantum, GFP_KERNEL);
		if (!dptr->data[qt_idx])
			goto out;
		memset(dptr->data[qt_idx], 0, quantum);
	}

	if (count > quantum - qt_offs)
		count = quantum - qt_offs;

	if (copy_from_user(dptr->data[qt_idx] + qt_offs, buff, count)) {
		retval = -EFAULT;
		goto out;
	}

	*f_pos += count;
	retval = count;
	if (dev->size < *f_pos)
		dev->size = *f_pos;

out:
	up(&dev->sem);
	return retval;
}

ssize_t scull_read(struct file *filp, char __user *buff,
		size_t count, loff_t *f_pos)
{
	struct scull_dev *dev = filp->private_data;
	struct scull_qset *dptr;
	int quantum = dev->quantum;
	int qset = dev->qset;
	int itemsize = quantum * qset;
	int item_idx, item_offs, qt_idx, qt_offs;
	ssize_t retval = 0;

	if (down_interruptible(&dev->sem))
		return -ERESTARTSYS;
	if (*f_pos >= dev->size)
		goto out;
	if (*f_pos + count > dev->size)
		count = dev->size - *f_pos;

	item_idx = (long)*f_pos / itemsize;
	item_offs = (long)*f_pos % itemsize;
	qt_idx = item_offs / quantum;
	qt_offs = item_offs % quantum;

	dptr = scull_follow(dev, item_idx);

	if (!dptr || !dptr->data || !dptr->data[qt_idx])
		goto out;

	if (count > quantum - qt_offs)
		count = quantum - qt_offs;

	if (copy_to_user(buff, dptr->data[qt_idx] + qt_offs, count)) {
		retval = -EFAULT;
		goto out;
	}

	*f_pos += count;
	retval = count;
out:
	up(&dev->sem);
	return retval;
}

int scull_release(struct inode *inode, struct file *filp)
{
	return 0;
}

int scull_open(struct inode *inode, struct file *filp)
{
	struct scull_dev *dev;

	dev = container_of(inode->i_cdev, struct scull_dev, cdev);
	filp->private_data = dev;

	if ((filp->f_flags & O_ACCMODE) == O_WRONLY)
		scull_trim(dev);

	return 0;
}

static struct scull_qset *scull_follow(struct scull_dev *dev, int item_idx)
{
	struct scull_qset *qs;
	qs = dev->data;
	if (!qs) {
		qs = kmalloc(sizeof(*qs), GFP_KERNEL);
		if (qs == NULL)
			return NULL;
		memset(qs, 0, sizeof(*qs));
	}
	while (item_idx--) {
		if (!qs->next) {
			qs->next = kmalloc(sizeof(*qs), GFP_KERNEL);
			if (qs->next == NULL)
				return NULL;
			memset(qs->next, 0, sizeof(*qs));
		}
		qs = qs->next;
	}
	return qs;
}

static int scull_trim(struct scull_dev *dev)
{
	struct scull_qset *next, *dptr;
	int qset, i;

	qset = dev->qset;
	dptr = dev->data;
	while (dptr) {
		if (dptr->data) {
			i = 0;
			while (i < qset) {
				kfree(dptr->data[i++]);
			}
			kfree(dptr->data);
			dptr->data = NULL;
		}
		next = dptr->next;
		kfree(dptr);
		dptr = next;
	}
	dev->size = 0;
	dev->quantum = scull_quantum;
	dev->qset = scull_qset;
	dev->data = NULL;
	return 0;
}

static void scull_setup_cdev(struct scull_dev *dev, int index)
{
	int err, devno;
	devno = MKDEV(scull_major, scull_minor + index);

	cdev_init(&dev->cdev, &scull_fops);
	dev->cdev.owner = THIS_MODULE;
	dev->cdev.ops = &scull_fops;

	err = cdev_add(&dev->cdev, devno, 1);
	if (err)
		printk(KERN_NOTICE "Error %d adding scull%d\n", err, index);
}

static int __init scull_init(void)
{
	dev_t dev;
	int result;
	int i;

	if (scull_major) {
		dev = MKDEV(scull_major, scull_minor);
		result = register_chrdev_region(dev, scull_nr_devs, "scull");
	} else {
		result = alloc_chrdev_region(&dev, scull_minor, scull_nr_devs,
			"scull");
	}
	if (result < 0) {
		printk(KERN_WARNING "scull: can't get major %d\n", scull_major);
		return result;
	}

	scull_devs = kmalloc(scull_nr_devs * sizeof(*scull_devs), GFP_KERNEL);
	if (!scull_devs) {
		result = -ENOMEM;
		goto fail_malloc;
	}
	memset(scull_devs, 0, scull_nr_devs * sizeof(*scull_devs));

	i = 0;
	while (i++ < scull_nr_devs) {
		scull_devs[i].qset = scull_qset;
		scull_devs[i].quantum = scull_quantum;
		scull_setup_cdev(&scull_devs[i], i);
	}
	printk("Scull init successfully");
	return 0;

fail_malloc:
	unregister_chrdev_region(dev, scull_nr_devs);
	return result;
}

static void scull_exit(void)
{
	int i, dev;

	i = 0;
	while (i++ < scull_nr_devs) {
		cdev_del(&scull_devs[i].cdev);
		scull_trim(scull_devs + i);
	}
	kfree(scull_devs);
	scull_devs = NULL;

	dev = MKDEV(scull_major, scull_minor);
	unregister_chrdev_region(dev, scull_nr_devs);

	printk("Scull exit successfully");
}

module_init(scull_init);
module_exit(scull_exit);
