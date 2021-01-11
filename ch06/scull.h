#include <linux/wait.h>
#include <asm/atomic.h>
#ifndef SCULL_MAJOR
#define SCULL_MAJOR 0
#endif

#ifndef SCULL_NR_DEVS
#define SCULL_NR_DEVS 4
#endif

#ifndef SCULL_BUFFERSIZE
#define SCULL_BUFFERSIZE 500
#endif

struct scull_dev {
	wait_queue_head_t in_q, out_q;
	atomic_t nreaders, nwriters;
	char *buffer, *end;
	char *rp, *wp;
	int buffersize;
	struct mutex mtx;
	struct cdev cdev;
};


static ssize_t scull_write(struct file *filp, const char __user *buff,
		size_t count, loff_t *f_pos);

static ssize_t scull_read(struct file *filp, char __user *buff,
		size_t count, loff_t *f_pos);

static int scull_release(struct inode *inode, struct file *filp);

static int scull_open(struct inode *inode, struct file *filp);

static void scull_setup_cdev(struct scull_dev *dev, int index);
