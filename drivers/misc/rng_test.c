#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/miscdevice.h>

struct rng_test_priv {
	struct miscdevice mdev;
	struct platform_device *pdev;
	int open_cnt;
	int close_cnt;
	int read_cnt;
};

static inline struct rng_test_priv * to_priv(struct file * file)
{
	struct miscdevice *miscdev = file->private_data;
	return container_of(miscdev, struct rng_test_priv, mdev);
}

static int rng_test_open(struct inode * inode, struct file * file)
{
	struct rng_test_priv * priv = to_priv(file);
	priv->open_cnt++;
	return 0;
}

static int rng_test_release(struct inode * inode, struct file * file)
{
	struct rng_test_priv * priv = to_priv(file);
	priv->close_cnt++;
	return 0;
}

static ssize_t rng_test_read(struct file * file, char * buffer, size_t length, loff_t *offset)
{
	struct rng_test_priv * priv = to_priv(file);
	dev_info(&priv->pdev->dev, "rng_test_read\n");
	return length;
}

static struct file_operations rng_test_fops = {
	.open = rng_test_open,
	.read = rng_test_read,
	.release = rng_test_release,
};

static int rng_test_probe(struct platform_device *pdev)
{
	struct rng_test_priv * priv = 0;
	int ret;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if(!priv){
		dev_err(&pdev->dev, "Failed to allocate memory\n");
		return -ENOMEM;
	}
	platform_set_drvdata(pdev, priv);

	priv->pdev = pdev;
	priv->mdev.minor = MISC_DYNAMIC_MINOR;
	priv->mdev.name = "rng-tester";
	priv->mdev.fops = &rng_test_fops;
	priv->mdev.parent = NULL;

	ret = misc_register(&priv->mdev);
	
	if(ret){
		dev_err(&pdev->dev, "Failed to register miscdev\n");
		return ret;
	}

	dev_info(&pdev->dev, "Device probed\n");
	
	return 0;
}

static int rng_test_remove(struct platform_device *pdev)
{
	struct rng_test_priv * priv = (struct rng_test_priv*)platform_get_drvdata(pdev);
	return 0;
}

static struct of_device_id rng_test_of_match[] = {
	{
		.compatible = "rng-test",	
	},
	{},
};

static struct platform_driver rng_test_platform = {
	.probe = rng_test_probe,
	.remove = rng_test_remove,
	.driver = {
		.name = "RNG tester",
		.owner = THIS_MODULE,
		.of_match_table = rng_test_of_match,
	},
};

static int __init rng_test_init(void)
{
	printk(KERN_ERR "RNG Tester registered\n");
	return platform_driver_register(&rng_test_platform);
}

static void __exit rng_test_exit(void)
{
	platform_driver_unregister(&rng_test_platform);
}

module_init(rng_test_init);
module_exit(rng_test_exit);
