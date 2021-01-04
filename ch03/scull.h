#define SCULL_MAJOR 0
#define SCULL_DEVS 4
#define SCULL_QSET 500
#define SCULL_QUANTUM 4000

static struct scull_dev {
	struct scull_qset *data;
	int quantum;
	int qset;
	unsigned long size;
	unsigned int access_key;
	struct mutex mtx;
	struct cdev cdev;
};

static struct scull_qset {
	void **data;
	struct scull_qset *next;
};

ssize_t scull_write(struct file *filp, const char __user *buff,
		size_t count, loff_t *f_pos);

ssize_t scull_read(struct file *filp, char __user *buff,
		size_t count, loff_t *f_pos);

int scull_release(struct inode *inode, struct file *filp);

int scull_open(struct inode *inode, struct file *filp);

static struct scull_qset *scull_follow(struct scull_dev *dev, int item);

static int scull_trim(struct scull_dev *dev);

static void scull_setup_cdev(struct scull_dev *dev, int index);
