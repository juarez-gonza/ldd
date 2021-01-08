#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <asm/uaccess.h>

#include "scull.h"
MODULE_LICENSE("GPL");

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
	ssize_t retval = -ENOMEM;

	if (mutex_lock_interruptible(&dev->mtx))
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
	}

	if (count > quantum - qt_offs)
		count = quantum - qt_offs;

	printk(KERN_DEBUG "START OF DATA BEFORE WRITTEN: %p\n", dev->data);
	printk(KERN_DEBUG "%p == %p ? %d\n", dev->data, dptr, dev->data == dptr ? 1 : 0);
	if (copy_from_user(dptr->data[qt_idx] + qt_offs, buff, count)) {
		retval = -EFAULT;
		goto out;
	}

	*f_pos += count;
	retval = count;
	if (dev->size < *f_pos)
		dev->size = *f_pos;
	printk(KERN_DEBUG "START OF DATA AFTER WRITTEN: %p\n", dev->data);
out:
	mutex_unlock(&dev->mtx);
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

	if (mutex_lock_interruptible(&dev->mtx))
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
	printk(KERN_DEBUG "NOT STOPPING IN SCULL_FOLLOW\n");

	if (dptr == NULL || !dptr->data || !dptr->data[qt_idx])
		goto out;
	printk(KERN_DEBUG "DATA NOT NULL\n");

	if (count > quantum - qt_offs)
		count = quantum - qt_offs;

	if (copy_to_user(buff, dptr->data[qt_idx] + qt_offs, count)) {
		retval = -EFAULT;
		goto out;
	}

	*f_pos += count;
	retval = count;
	printk(KERN_DEBUG "DATA READ SUCCESSFULLY\n");
out:
	mutex_unlock(&dev->mtx);
	return retval;
}

int scull_release(struct inode *inode, struct file *filp)
{
	struct scull_dev *dev = filp->private_data;
	printk("START OF DATA AFTER RELEASE: %p\n", dev->data);
	return 0;
}

int scull_open(struct inode *inode, struct file *filp)
{
	struct scull_dev *dev;

	dev = container_of(inode->i_cdev, struct scull_dev, cdev);
	filp->private_data = dev;

	if ((filp->f_flags & O_ACCMODE) == O_WRONLY) {
		if (mutex_lock_interruptible(&dev->mtx))
			return -ERESTARTSYS;
		printk(KERN_DEBUG "FLAGS FOR WRITE ONLY\n");
		scull_trim(dev);
		mutex_unlock(&dev->mtx);
	}

	return 0;
}

struct scull_qset *scull_follow(struct scull_dev *dev, int item_idx)
{
	struct scull_qset *qs;
	qs = dev->data;
	printk("BEGGINING DATA: %d, POSITION: %p\n", qs == NULL ? 0 : 1, qs);
	if (!qs) {
		qs = dev->data = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
		if (qs == NULL)
			return NULL;
		memset(qs, 0, sizeof(struct scull_qset));
	}
	printk("START OF DATA %p", qs);
	while (item_idx--) {
		if (!qs->next) {
			qs->next = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
			if (qs->next == NULL)
				return NULL;
			memset(qs->next, 0, sizeof(struct scull_qset));
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
	for (dptr = dev->data; dptr; dptr = next) {
		if (dptr->data) {
			for (i = 0; i < qset; i++)
				kfree(dptr->data[i]);
			kfree(dptr->data);
			dptr->data = NULL;
		}
		next = dptr->next;
		kfree(dptr);
	}
	dev->size = 0;
	dev->quantum = scull_quantum;
	dev->qset = scull_qset;
	dev->data = NULL;
	return 0;
}

void scull_setup_cdev(struct scull_dev *dev, int index)
{
	int err, devno;
	devno = MKDEV(scull_major, scull_minor + index);

	cdev_init(&dev->cdev, &scull_fops);
	dev->cdev.owner = THIS_MODULE;
	dev->cdev.ops = &scull_fops;

	err = cdev_add(&dev->cdev, devno, 1);
	if (err)
		printk(KERN_NOTICE "Error %d adding scull%d\n", err, index);
	printk("cdev added succesfully. Major: %d, Minor: %d\n",
		scull_major, scull_minor + index);
}

static void scull_exit(void)
{
	int i;
	dev_t dev;

	i = 0;
	if (scull_devs) {
		while (i < scull_nr_devs) {
			scull_trim(scull_devs + i);
			cdev_del(&scull_devs[i].cdev);
			i++;
		}
		kfree(scull_devs);
		scull_devs = NULL;
	}
	dev = MKDEV(scull_major, scull_minor);
	unregister_chrdev_region(dev, scull_nr_devs);
	printk("Scull exit successfully. Major: %d, Minor: %d\n",
		scull_major, scull_minor);
}

int __init scull_init(void)
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
		scull_major = MAJOR(dev);
	}
	if (result < 0) {
		printk(KERN_WARNING "scull: can't get major %d\n", scull_major);
		return result;
	}

	scull_devs = kmalloc(scull_nr_devs * sizeof(struct scull_dev), GFP_KERNEL);
	if (!scull_devs) {
		result = -ENOMEM;
		goto fail_malloc;
	}
	memset(scull_devs, 0, scull_nr_devs * sizeof(struct scull_dev));

	i = 0;
	while (i < scull_nr_devs) {
		scull_devs[i].qset = scull_qset;
		scull_devs[i].quantum = scull_quantum;
		mutex_init(&scull_devs[i].mtx);
		scull_setup_cdev(&scull_devs[i], i);
		i++;
	}
	printk("Scull init successfully. Major: %d, Minor: %d\n",
		scull_major, scull_minor);
	return 0;

fail_malloc:
	scull_exit();
	return result;
}

module_init(scull_init);
module_exit(scull_exit);
