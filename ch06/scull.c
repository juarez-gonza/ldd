#include <linux/init.h>
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


int scull_major = SCULL_MAJOR;
int scull_minor = 0;
int scull_nr_devs = SCULL_NR_DEVS;
int scull_buffersize = SCULL_BUFFERSIZE;

struct scull_dev *scull_devs = NULL;

module_param(scull_major, int, S_IRUGO);
module_param(scull_minor, int, S_IRUGO);
module_param(scull_nr_devs, int, S_IRUGO);
module_param(scull_buffersize, int, S_IRUGO);

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

static int inline freespace(struct scull_dev *dev)
{
	if (dev->wp == dev->rp)
		return dev->buffersize - 1;
	return ((dev->rp + dev->buffersize - dev->wp) % dev->buffersize) - 1;
}

static ssize_t scull_write(struct file *filp, const char __user *buff,
		size_t count, loff_t *f_pos)
{
	struct scull_dev *dev;
	ssize_t retval;
	dev = filp->private_data;
	if (mutex_lock_interruptible(&dev->mtx))
		return -ERESTARTSYS;

	/*
	 * Simple way to perform a sleep (less configurable)
	 * while (freespace(dev) == 0) {
		mutex_unlock(&dev->mtx);
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (wait_event_interruptible(dev->out_q, freespace(dev) != 0))
			return -ERESTARTSYS;
		if (mutex_lock_interruptible(&dev->mtx))
			return -ERESTARTSYS;
	}
	*/

	// complex way to perform a sleep
	while (!freespace(dev)) {
		DEFINE_WAIT(wait);

		mutex_unlock(&dev->mtx);
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;

		prepare_to_wait(&dev->out_q, &wait, TASK_INTERRUPTIBLE);
		if (!freespace(dev))
			schedule();
		finish_wait(&dev->out_q, &wait);
		if (signal_pending(current))
			return -ERESTARTSYS;

		if (mutex_lock_interruptible(&dev->mtx))
			return -ERESTARTSYS;
	}

	if (dev->wp < dev->rp)
		count = min((size_t)(dev->rp - dev->wp), count);
	else
		count = min((size_t)(dev->end - dev->wp), count);

	if (copy_from_user(dev->buffer, buff, count)) {
		retval = -EFAULT;
		goto out;
	}
	dev->wp += count;
	if (dev->wp == dev->end)
		dev->wp = dev->buffer;
	retval = count;

out:
	mutex_unlock(&dev->mtx);
	wake_up_interruptible(&dev->in_q);
	return retval;
}

static ssize_t scull_read(struct file *filp, char __user *buff,
		size_t count, loff_t *f_pos)
{
	struct scull_dev *dev;
	ssize_t retval;
	dev = filp->private_data;
	if (mutex_lock_interruptible(&dev->mtx))
		return -ERESTARTSYS;

	/*
	 * Simple way to perform a sleep (less configurable)
	 * while (dev->rp == dev->wp) {
		mutex_unlock(&dev->mtx);
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (wait_event_interruptible(dev->in_q, dev->rp != dev->wp))
			return -ERESTARTSYS;
		if (mutex_lock_interruptible(&dev->mtx))
			return -ERESTARTSYS;
	}
	*/

	// comples way to perform a sleep
	while (dev->rp == dev->wp) {
		DEFINE_WAIT(wait);

		mutex_unlock(&dev->mtx);
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;

		prepare_to_wait(&dev->in_q, &wait, TASK_INTERRUPTIBLE);
		if (dev->rp == dev->wp)
			schedule();
		finish_wait(&dev->in_q, &wait);
		if (signal_pending(current))
			return -ERESTARTSYS;

		if (mutex_lock_interruptible(&dev->mtx))
			return -ERESTARTSYS;
	}

	if (dev->rp < dev->wp)
		count = min((size_t)(dev->wp - dev->rp), count);
	else
		count = min((size_t)(dev->end - dev->rp), count);

	if (copy_to_user(buff, dev->buffer, count)) {
		retval = -EFAULT;
		goto out;
	}
	dev->rp += count;
	if (dev->rp == dev->end)
		dev->rp = dev->buffer;
	retval = count;

out:
	mutex_unlock(&dev->mtx);
	wake_up_interruptible(&dev->out_q);
	return retval;
}

static int scull_release(struct inode *inode, struct file *filp)
{
	struct scull_dev *dev;
	dev = filp->private_data;

	if ((filp->f_flags & O_ACCMODE) == O_WRONLY) {
		atomic_dec(&dev->nwriters);
	}
	if ((filp->f_flags & O_ACCMODE) == O_RDONLY) {
		atomic_dec(&dev->nreaders);
	}
	return 0;
}

static int scull_open(struct inode *inode, struct file *filp)
{
	struct scull_dev *dev;

	dev = container_of(inode->i_cdev, struct scull_dev, cdev);
	filp->private_data = dev;

	if ((filp->f_flags & O_ACCMODE) == O_WRONLY) {
		atomic_inc(&dev->nwriters);
	}
	if ((filp->f_flags & O_ACCMODE) == O_RDONLY) {
		atomic_inc(&dev->nreaders);
	}

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
			kfree(scull_devs[i].buffer);
			scull_devs[i].buffer = NULL;
			scull_devs[i].buffersize = 0;
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

	for (i = 0; i < scull_nr_devs; i++) {
		mutex_init(&scull_devs[i].mtx);
		init_waitqueue_head(&scull_devs[i].in_q);
		init_waitqueue_head(&scull_devs[i].out_q);
		atomic_set(&scull_devs[i].nreaders, 0);
		atomic_set(&scull_devs[i].nwriters, 0);

		scull_devs[i].buffersize = scull_buffersize;
		scull_devs[i].buffer =
			kmalloc(sizeof(char *) * scull_buffersize,
				GFP_KERNEL);
		if (!scull_devs[i].buffer)
			goto fail_buff_alloc;
		memset(scull_devs[i].buffer, 0,sizeof(char *) * scull_buffersize);
		scull_devs[i].end = scull_devs[i].buffer + scull_buffersize;

		scull_setup_cdev(&scull_devs[i], i);
	}
	printk("Scull init successfully. Major: %d, Minor: %d\n",
		scull_major, scull_minor);
	return 0;

fail_buff_alloc:
	kfree(scull_devs);
fail_malloc:
	scull_exit();
	return result;
}

module_init(scull_init);
module_exit(scull_exit);
