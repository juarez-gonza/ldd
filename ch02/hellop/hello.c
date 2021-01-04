#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
MODULE_LICENSE("DUAL BSD/GPL");

static char* whom = "world";
static int howmany = 1;

module_param(howmany, int, 0);
module_param(whom, charp, 0);

static int hello_init(void)
{
	int i = 0;
	while (i++ < howmany)
		printk(KERN_ALERT "Hello, %s\n", whom);
	return 0;
}

static void hello_exit(void)
{
	printk(KERN_ALERT "Goodbye, cruel %s\n", whom);
}

module_init(hello_init);
module_exit(hello_exit);
