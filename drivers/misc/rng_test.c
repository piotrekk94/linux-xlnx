#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/of_dma.h>
#include <linux/platform_device.h>
#include <linux/miscdevice.h>
#include <linux/delay.h>
#include <linux/completion.h>
#include <linux/uaccess.h>

#define BUF_SIZE 1024*8
#define REG_CTL 0
#define REG_SEED0 4
#define REG_SEED1 8
#define REG_PKT 12
#define LFSR_UPDATE 1
#define LFSR_EN 2
#define FIFO_RST 4

struct rng_test_priv {
	struct miscdevice mdev;
	struct platform_device *pdev;
	void * base;
	struct dma_chan * dma;
	dma_addr_t buf_phys;
	void * buf_virt;
	struct completion dma_done;
};

static inline void writeReg(struct rng_test_priv * priv, int off, unsigned int val)
{
	writel(val, priv->base + off);
}

static inline unsigned int readReg(struct rng_test_priv * priv, int off)
{
	return readl(priv->base + off);
}

static inline struct rng_test_priv * to_priv(struct file * file)
{
	struct miscdevice *miscdev = file->private_data;
	return container_of(miscdev, struct rng_test_priv, mdev);
}

static int rng_test_open(struct inode * inode, struct file * file)
{
	struct rng_test_priv * priv = to_priv(file);
	writeReg(priv, REG_CTL, LFSR_EN);
	return 0;
}

static int rng_test_release(struct inode * inode, struct file * file)
{
	struct rng_test_priv * priv = to_priv(file);
	writeReg(priv, REG_CTL, 0);
	return 0;
}

static void rng_test_dma_done(void *data)
{
	struct rng_test_priv *priv = (struct rng_test_priv*)data;
	complete(&priv->dma_done);
}

static ssize_t rng_test_read(struct file * file, char * buffer, size_t length, loff_t *offset)
{
	struct rng_test_priv * priv = to_priv(file);
	struct dma_interleaved_template *xt = 0;
	struct dma_async_tx_descriptor *tx = 0;
	dma_cookie_t cookie;

	if(length != BUF_SIZE){
		return -EIO;
	}

	xt = kzalloc(sizeof(*xt), GFP_KERNEL);

	xt->dst_start = priv->buf_phys;
	xt->numf = 1;
	xt->frame_size = 1;
	xt->sgl[0].size = BUF_SIZE;
	xt->sgl[0].icg = 0;
	xt->dir = DMA_DEV_TO_MEM;
	xt->src_sgl = false;
	xt->src_inc = false;
	xt->dst_sgl = false;
	xt->dst_inc = true;

	tx = dmaengine_prep_interleaved_dma(priv->dma, xt, DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if(!tx){
		dev_err(&priv->pdev->dev, "dmaengine_prep_interleaved_dma error\n");
		return -EINVAL;
	}

	tx->callback = rng_test_dma_done;
	tx->callback_param = priv;

	cookie = dmaengine_submit(tx);
	if(dma_submit_error(cookie)){	
		dev_err(&priv->pdev->dev, "dmaengine_submit error\n");
		return -EINVAL;
	}

	dma_async_issue_pending(priv->dma);

	wait_for_completion_interruptible_timeout(&priv->dma_done, msecs_to_jiffies(10000));

	kfree(xt);

	copy_to_user(buffer, priv->buf_virt, BUF_SIZE);

	return BUF_SIZE;
}

static struct file_operations rng_test_fops = {
	.open = rng_test_open,
	.read = rng_test_read,
	.release = rng_test_release,
};


static void rng_test_config(struct rng_test_priv * priv)
{
	writeReg(priv, REG_CTL, FIFO_RST);
	writeReg(priv, REG_PKT, (BUF_SIZE/8) - 2);
	writeReg(priv, REG_SEED0, 21);
	writeReg(priv, REG_SEED1, 37);
	writeReg(priv, REG_CTL, FIFO_RST | LFSR_UPDATE);
	writeReg(priv, REG_CTL, FIFO_RST);
	usleep_range(100, 200);
	writeReg(priv, REG_CTL, 0);
}

static int rng_test_probe(struct platform_device *pdev)
{
	struct rng_test_priv * priv = 0;
	int ret;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if(!priv){
		dev_err(&pdev->dev, "Failed to allocate memory\n");
		ret = -ENOMEM;
		goto err_priv;
	}

	platform_set_drvdata(pdev, priv);

	priv->base = of_iomap(pdev->dev.of_node, 0);
	if(!priv->base){
		dev_err(&pdev->dev, "Failed to map registers\n");
		ret = -ENOMEM;
		goto err_iomap;
	}

	priv->dma = dma_request_slave_channel(&pdev->dev, "axidma");
	if(IS_ERR(priv->dma)){
		dev_err(&pdev->dev, "Failed to request DMA channel\n");
		ret = PTR_ERR(priv->dma);
		goto err_dma;
	}

	priv->buf_virt = dma_alloc_coherent(&pdev->dev, BUF_SIZE, &priv->buf_phys, GFP_KERNEL);
	if(!priv->buf_virt){
		dev_err(&pdev->dev, "Failed to allocate DMA buffer\n");
		ret = -ENOMEM;
		goto err_dmabuf;
	}

	priv->pdev = pdev;
	priv->mdev.minor = MISC_DYNAMIC_MINOR;
	priv->mdev.name = "rng-tester";
	priv->mdev.fops = &rng_test_fops;
	priv->mdev.parent = NULL;

	ret = misc_register(&priv->mdev);
	
	if(ret){
		dev_err(&pdev->dev, "Failed to register miscdev\n");
		goto err_misc;
	}

	init_completion(&priv->dma_done);

	dev_info(&pdev->dev, "Device probed\n");
	
	rng_test_config(priv);

	return 0;

err_misc:
	dma_free_coherent(&pdev->dev, BUF_SIZE, priv->buf_virt, priv->buf_phys);
err_dmabuf:
	dma_release_channel(priv->dma);
err_dma:
	iounmap(priv->base);
err_iomap:
	kfree(priv);
err_priv:
	return ret;
}

static int rng_test_remove(struct platform_device *pdev)
{
	struct rng_test_priv * priv = (struct rng_test_priv*)platform_get_drvdata(pdev);
	misc_deregister(&priv->mdev);
	dma_free_coherent(&pdev->dev, BUF_SIZE, priv->buf_virt, priv->buf_phys);
	dma_release_channel(priv->dma);
	iounmap(priv->base);
	kfree(priv);
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
