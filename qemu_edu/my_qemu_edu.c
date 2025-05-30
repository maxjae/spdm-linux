/* many things copied from  https://cirosantilli.com/linux-kernel-module-cheat#qemu-edu */
#include <linux/cdev.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/kernel.h>
#include <linux/device.h> // for dev_* debugging messages
#include <asm-generic/io.h> // for iowrite*/ioread*
#include <linux/mm.h> // for disagg_test_check_dma_values

#define QEMU_VENDOR_ID 0x1234
#define QEMU_EDU_DEVICE_ID 0x11e8
#define PCI_BAR 0
#define MY_DRIVER_NAME "my_qemu_edu_driver"
#define CDEV_NAME "my_qemu_edu"

/* Registers. */
#define IO_IRQ_STATUS 0x24
#define IO_IRQ_ACK 0x64
#define IO_DMA_SRC 0x80
#define IO_DMA_DST 0x88
#define IO_DMA_CNT 0x90
#define IO_DMA_CMD 0x98

/* Constants */
#define DMA_BASE 0x40000
#define DMA_CMD 0x1
#define DMA_FROM_DEV 0x2
#define DMA_IRQ 0x4


static int major;
static struct pci_dev *pdev;
static void __iomem *mmio;

static struct pci_device_id my_pci_ids[] = {
    { PCI_DEVICE(QEMU_VENDOR_ID, QEMU_EDU_DEVICE_ID) },
    { 0, }
};
MODULE_DEVICE_TABLE(pci, my_pci_ids);


/* Cdev file operations */

static ssize_t my_read(struct file *filep, char __user *buf, size_t len, loff_t *off)
{
    // use ioread* and copy_to_user
    return 0;
}

static ssize_t my_write(struct file *filep, const char __user *buf, size_t len, loff_t *off)
{
    // use iowrite* and copy_from_user
    return 0;
}

static struct file_operations my_fops = {
    .owner = THIS_MODULE,
    .read = my_read,
    .write = my_write,
};

/* Irq */

static irqreturn_t my_irq_handler(int irq, void *dev)
{
    int devi;
    irqreturn_t ret;
    u32 irq_status;

    devi = *(int *)dev;
    if (devi == major) {
	irq_status = ioread32(mmio + IO_IRQ_STATUS);
	pr_info("my_irq_handler irq = %d, dev = %d, irq_status = %llx\n",
		irq, devi, (unsigned long long) irq_status);
	iowrite32(irq_status, mmio + IO_IRQ_ACK);
	ret = IRQ_HANDLED;
    } else {
	ret = IRQ_NONE;
    }
    return ret;
}

/* Pci specific code */

/* https://www.kernel.org/doc/html/latest/PCI/pci.html#device-initialization-steps */
static int my_pci_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
    dev_info(&dev->dev, "my_pci_probe\n");

    pdev = dev;
    major = register_chrdev(0, CDEV_NAME, &my_fops);

    if (pci_enable_device(dev) < 0) {
	dev_err(&dev->dev, "Error: pci_enable_device failed\n");
	goto error;
    }

    if (pci_request_region(dev, PCI_BAR, MY_DRIVER_NAME) < 0) {
	dev_err(&dev->dev, "Error: pci_request_region failed\n");
	goto error_requ_reg;
    }

    mmio = pci_iomap(dev, PCI_BAR, pci_resource_len(dev, PCI_BAR));

    /* IRQ setup */
    pci_set_master(dev);

    if (pci_alloc_irq_vectors(dev, 1, 1, PCI_IRQ_MSI) < 0) {
	dev_err(&(dev->dev), "Error: pci_alloc_irq_vectors failed\n");
	goto error_irq_vectors;
    }

    dev->irq = pci_irq_vector(dev, 0);

    if (request_irq(dev->irq, my_irq_handler, 0, CDEV_NAME, &major) < 0) {
	dev_err(&(dev->dev), "Error: request_irq failed\n");
	goto error_requ_irq;
    }

	/* Optional sanity checks. The PCI is ready now, all of this could also be called from fops. */
	{
		unsigned i;
		u8 val;

		/* Check that we are using MEM instead of IO.
		 *
		 * In QEMU, the type is defiened by either:
		 *
		 * - PCI_BASE_ADDRESS_SPACE_IO
		 * - PCI_BASE_ADDRESS_SPACE_MEMORY
		 */
		if ((pci_resource_flags(dev, PCI_BAR) & IORESOURCE_MEM) != IORESOURCE_MEM) {
			dev_err(&(dev->dev), "pci_resource_flags\n");
			goto error;
		}

		/* 1Mb, as defined by the "1 << 20" in QEMU's memory_region_init_io. Same as pci_resource_len. */
		resource_size_t start = pci_resource_start(dev, PCI_BAR);
		resource_size_t end = pci_resource_end(dev, PCI_BAR);
		pr_info("The starting address of BAR %d is %lx\n", PCI_BAR, (unsigned long)(start));
		pr_info("length %llx\n", (unsigned long long)(end + 1 - start));
		pr_info("EDU MMIO virtual address starts at: %lx\n", (unsigned long) mmio);

		/* The PCI standardized 64 bytes of the configuration space, see LDD3. */
		for (i = 0; i < 64u; ++i) {
			pci_read_config_byte(dev, i, &val);
			pr_info("config %x %x\n", i, val);
		}
		pr_info("dev->irq %x\n", dev->irq);

		pr_info("QEMU EDU: Address: %llu\n", virt_to_phys(mmio));

		/* Initial value of the IO memory. */
		// for (long long j = 0; j < 2500; j++) {
		for (i = 0; i < 0x28; i += 4) {
			pr_info("io %x %x\n", i, ioread32((void*)(mmio + i)));
		}
		// }

		pr_info("Inversion test\n");
		unsigned edu_id = ioread32((void*) mmio);
		iowrite32(edu_id, (void*)(mmio + 4));
		edu_id = ioread32((void*)(mmio + 4));
		pr_info("Inverted value %x\n", edu_id);

		

		/* DMA test.
		 *
		 * TODO:
		 *
		 * - deal with interrupts properly.
		 * - printf / gdb in QEMU source says dma_buf is not being set correctly
		 *
		 * Resources:
		 *
		 * - http://elixir.free-electrons.com/linux/v4.12/source/Documentation/DMA-API-HOWTO.txt
		 * - http://www.makelinux.net/ldd3/chp-15-sect-4
		 * - https://stackoverflow.com/questions/32592734/are-there-any-dma-linux-kernel-driver-example-with-pcie-for-fpga/44716747#44716747
		 * - https://stackoverflow.com/questions/17913679/how-to-instantiate-and-use-a-dma-driver-linux-module
		 * - https://stackoverflow.com/questions/5539375/linux-kernel-device-driver-to-dma-from-a-device-into-user-space-memory
		 * - RPI userland /dev/mem https://github.com/Wallacoloo/Raspberry-Pi-DMA-Example
		 * - https://stackoverflow.com/questions/34188369/easiest-way-to-use-dma-in-linux
		 */
		{
		    dev_info(&(dev->dev), "DMA Test 1\n");
		    dma_addr_t dma_handle;
		    enum { SIZE = 256 };
		    void *actual = kmalloc(SIZE, GFP_KERNEL);
		    void *expected = kmalloc(SIZE, GFP_KERNEL);

		    memset(actual, 0xba, SIZE);

		    dma_handle = dma_map_single(&(dev->dev), actual, SIZE, DMA_BIDIRECTIONAL);
		    if (dma_mapping_error(&(dev->dev), dma_handle)) {
			dev_info(&(dev->dev), "my_pci_probe: dma_alloc_coherent failed\n");
			return 0;
		    }

		    // Proide device with information about the DMA transfer
		    writeq((u64)dma_handle, mmio + IO_DMA_SRC);
		    writeq(DMA_BASE, mmio + IO_DMA_DST);
		    writeq(SIZE, mmio + IO_DMA_CNT);
		    iowrite32(DMA_CMD, mmio + IO_DMA_CMD);
		    while(ioread32(mmio + IO_DMA_CMD) & 0x1) {}

		    // Update values in the mapped region and tell device to also update its internal memory with it
		    memset(actual, 0xcc, SIZE / 2);
		    dma_sync_single_for_device(&(dev->dev), dma_handle, SIZE / 2, DMA_BIDIRECTIONAL);
		    writeq((u64)dma_handle, mmio + IO_DMA_SRC);
		    writeq(DMA_BASE, mmio + IO_DMA_DST);
		    writeq(SIZE, mmio + IO_DMA_CNT);
		    iowrite32(DMA_CMD, mmio + IO_DMA_CMD);
		    while(ioread32(mmio + IO_DMA_CMD) & 0x1) {}

		    // Now instruct device to write internal buffer back to mapped region
		    writeq(DMA_BASE, mmio + IO_DMA_SRC);
		    writeq((u64)dma_handle, mmio + IO_DMA_DST);
		    writeq(SIZE, mmio + IO_DMA_CNT);
		    iowrite32(DMA_CMD | DMA_FROM_DEV, mmio + IO_DMA_CMD);
		    while(ioread32(mmio + IO_DMA_CMD) & 0x1) {}

		    memset(actual, 0x00, SIZE);

		    dma_sync_single_for_cpu(&(dev->dev), dma_handle, SIZE, DMA_BIDIRECTIONAL);

		    memset(expected, 0xcc, SIZE / 2);
		    memset(expected + SIZE/2, 0xba, SIZE / 2);
		    if (memcmp(expected, actual, SIZE) != 0)
			goto fail;

		    pr_info("DMA test 1 passed");
		    kfree(actual);
		    kfree(expected);
		    goto end;

		fail:
		    pr_info("DMA test 1 failed");
		    kfree(actual);
		    kfree(expected);
		end:
		    dma_unmap_single(&(dev->dev), dma_handle, SIZE, DMA_BIDIRECTIONAL);
		}
		{
		    // Primarily tests the free_list allocator
		    dev_info(&(dev->dev), "DMA Test 2\n");
		    dma_addr_t dma_handle1, dma_handle2, dma_handle3, dma_handle4, dma_handle5;
		    size_t initial_dma_size = (1 << 20) - (1 << 12);
		    enum { SIZE1 = 512, SIZE2 = 8193, SIZE3 = 256, SIZE4 = 5000, SIZE5 = 4090 };
		    void *actual1 = kmalloc(SIZE1, GFP_KERNEL);
		    void *actual2 = kmalloc(SIZE2, GFP_KERNEL);
		    void *actual3 = kmalloc(SIZE3, GFP_KERNEL);
		    void *actual4 = kmalloc(SIZE4, GFP_KERNEL);
		    void *actual5 = kmalloc(SIZE5, GFP_KERNEL);

		    memset(actual1, 0x11, SIZE1);
		    memset(actual2, 0x22, SIZE2);
		    memset(actual3, 0x33, SIZE3);
		    memset(actual4, 0x44, SIZE4);
		    memset(actual5, 0x55, SIZE5);

		    if (!disagg_test_check_dma_values(1, 0, initial_dma_size))
			goto end2;
		    
		    // First page
		    dma_handle1 = dma_map_single(&(dev->dev), actual1, SIZE1, DMA_BIDIRECTIONAL);
		    if (dma_mapping_error(&(dev->dev), dma_handle1)) {
			    goto end2;
		    }
		    if (!disagg_test_check_dma_values(1, 0, initial_dma_size - (1 << 12)))
			goto end2;

		    // next 3 pages
		    dma_handle2 = dma_map_single(&(dev->dev), actual2, SIZE2, DMA_BIDIRECTIONAL);
		    if (dma_mapping_error(&(dev->dev), dma_handle2)) {
			goto end2;
		    }
		    if (!disagg_test_check_dma_values(1, 0, initial_dma_size - 4 * (1 << 12)))
			goto end2;

		    // another page
		    dma_handle3 = dma_map_single(&(dev->dev), actual3, SIZE3, DMA_BIDIRECTIONAL);
		    if (dma_mapping_error(&(dev->dev), dma_handle3)) {
			goto end2;
		    }
		    if (!disagg_test_check_dma_values(1, 0, initial_dma_size - 5 * (1 << 12)))
			goto end2;


		    // unmap the middle one
		    dma_unmap_single(&(dev->dev), dma_handle2, SIZE2, DMA_BIDIRECTIONAL);
		    if (!disagg_test_check_dma_values(2, 0, 3 * (1 << 12)) 
			    || !disagg_test_check_dma_values(2, 1, initial_dma_size - 5 * (1 << 12)))
			goto end2;

		    // Map 2 pages
		    dma_handle4 = dma_map_single(&(dev->dev), actual4, SIZE4, DMA_BIDIRECTIONAL);
		    if (dma_mapping_error(&(dev->dev), dma_handle4)) {
			goto end2;
		    }
		    if (!disagg_test_check_dma_values(2, 0, 1 * (1 << 12)) 
			    || !disagg_test_check_dma_values(2, 1, initial_dma_size - 5 * (1 << 12)))
			goto end2;

		    // Map 3 pages again
		    dma_handle2 = dma_map_single(&(dev->dev), actual2, SIZE2, DMA_BIDIRECTIONAL);
		    if (dma_mapping_error(&(dev->dev), dma_handle2)) {
			goto end2;
		    }
		    if (!disagg_test_check_dma_values(2, 0, 1 * (1 << 12)) 
			    || !disagg_test_check_dma_values(2, 1, initial_dma_size - 8 * (1 << 12)))
			goto end2;

		    dma_unmap_single(&(dev->dev), dma_handle1, SIZE1, DMA_BIDIRECTIONAL);
		    dma_unmap_single(&(dev->dev), dma_handle2, SIZE2, DMA_BIDIRECTIONAL);
		    dma_unmap_single(&(dev->dev), dma_handle3, SIZE3, DMA_BIDIRECTIONAL);
		    dma_unmap_single(&(dev->dev), dma_handle4, SIZE4, DMA_BIDIRECTIONAL);

		    if (!disagg_test_check_dma_values(1, 0, initial_dma_size))
			goto end2;

		    // Check if authsize is considered during allocation
		    dma_handle5 = dma_map_single(&(dev->dev), actual5, SIZE5, DMA_BIDIRECTIONAL);
		    if (dma_mapping_error(&(dev->dev), dma_handle5)) {
			    goto end2;
		    }
		    if (!disagg_test_check_dma_values(1, 0, initial_dma_size - 1 * (1 << 12)))
			goto end2;
		    dma_unmap_single(&(dev->dev), dma_handle5, SIZE5, DMA_BIDIRECTIONAL);
		    if (!disagg_test_check_dma_values(1, 0, initial_dma_size))
			goto end2;

		    pr_info("DMA test 2 passed");
		    goto end2_good;

		end2:	
		    pr_info("DMA test 2 failed");
		end2_good:
		    kfree(actual1);
		    kfree(actual2);
		    kfree(actual3);
		    kfree(actual4);
		    kfree(actual5);
		}
		{
		    dev_info(&(dev->dev), "DMA Test 3\n");
		    dma_addr_t dma_handle1, dma_handle2;
		    enum { SIZE1 = 2048, SIZE2 = 2048 };
		    void *actual1 = kmalloc(SIZE1, GFP_KERNEL);
		    void *actual2 = kmalloc(SIZE2, GFP_KERNEL);
		    void *expected = kmalloc(SIZE1, GFP_KERNEL);

		    memset(actual1, 0x11, SIZE1);
		    memset(actual2, 0x22, SIZE2);
		    
		    dma_handle1 = dma_map_single(&(dev->dev), actual1, SIZE1, DMA_BIDIRECTIONAL);
		    if (dma_mapping_error(&(dev->dev), dma_handle1)) {
			pr_info("DMA test 3 failed with mapping error 1");
			goto kfree_3;
		    }
		    dma_handle2 = dma_map_single(&(dev->dev), actual2, SIZE2, DMA_BIDIRECTIONAL);
		    if (dma_mapping_error(&(dev->dev), dma_handle2)) {
			pr_info("DMA test 3 failed with mapping error 2");
			goto unmap_3_1;
		    }

		    // Write first buffer to device's dma buffer 
		    writeq((u64)dma_handle1, mmio + IO_DMA_SRC);
		    writeq(DMA_BASE, mmio + IO_DMA_DST);
		    writeq(SIZE1, mmio + IO_DMA_CNT);
		    iowrite32(DMA_CMD, mmio + IO_DMA_CMD);
		    while(ioread32(mmio + IO_DMA_CMD) & 0x1) {}

		    // Write second buffer to device's dma buffer 
		    writeq((u64)dma_handle2, mmio + IO_DMA_SRC);
		    writeq(DMA_BASE + SIZE1, mmio + IO_DMA_DST);
		    writeq(SIZE2, mmio + IO_DMA_CNT);
		    iowrite32(DMA_CMD, mmio + IO_DMA_CMD);
		    while(ioread32(mmio + IO_DMA_CMD) & 0x1) {}

		    // Let device write 256 bytes of second buffer into first
		    writeq(DMA_BASE + SIZE1, mmio + IO_DMA_SRC);
		    writeq(dma_handle1 + 512, mmio + IO_DMA_DST);
		    writeq(256, mmio + IO_DMA_CNT);
		    iowrite32(DMA_CMD | DMA_FROM_DEV, mmio + IO_DMA_CMD);
		    while(ioread32(mmio + IO_DMA_CMD) & 0x1) {}

		    dma_sync_single_for_cpu(&(dev->dev), dma_handle1, SIZE1, DMA_BIDIRECTIONAL);

		    // Check if buffer 1 was updated
		    memset(expected, 0x11, SIZE1);
		    memset(expected + 512, 0x22, 256);
		    if (memcmp(expected, actual1, SIZE1) != 0) {
			pr_info("first partial buffer update failed");
			goto fail3;
		    }

		    // Update whole buffer 2, but do only a partial sync
		    memset(actual2, 0xff, SIZE2);
		    dma_sync_single_for_device(&(dev->dev), dma_handle2 + 1024, 4, DMA_BIDIRECTIONAL);

		    writeq((u64)dma_handle2, mmio + IO_DMA_SRC);
		    writeq(DMA_BASE + SIZE1, mmio + IO_DMA_DST);
		    writeq(SIZE2, mmio + IO_DMA_CNT);
		    iowrite32(DMA_CMD, mmio + IO_DMA_CMD);
		    while(ioread32(mmio + IO_DMA_CMD) & 0x1) {}

		    writeq(DMA_BASE + SIZE1, mmio + IO_DMA_SRC);
		    writeq(dma_handle1, mmio + IO_DMA_DST);
		    writeq(SIZE2, mmio + IO_DMA_CNT);
		    iowrite32(DMA_CMD | DMA_FROM_DEV, mmio + IO_DMA_CMD);
		    while(ioread32(mmio + IO_DMA_CMD) & 0x1) {}

		    dma_sync_single_for_cpu(&(dev->dev), dma_handle1, SIZE1, DMA_BIDIRECTIONAL);

		    memset(expected, 0x22, SIZE2);
		    memset(expected + 1024, 0xff, 4);
		    if (memcmp(expected, actual1, SIZE2) != 0) {
			pr_info("second partial buffer update failed");
			goto fail3;
		    }

		    // test partical cpu sync
		    memset(actual1, 0xee, SIZE1);
		    dma_sync_single_for_device(&(dev->dev), dma_handle1, SIZE1, DMA_BIDIRECTIONAL);

		    memset(actual2, 0xbb, SIZE2);
		    dma_sync_single_for_device(&(dev->dev), dma_handle2, SIZE2, DMA_BIDIRECTIONAL);

		    writeq((u64)dma_handle1, mmio + IO_DMA_SRC);
		    writeq(DMA_BASE, mmio + IO_DMA_DST);
		    writeq(SIZE1, mmio + IO_DMA_CNT);
		    iowrite32(DMA_CMD, mmio + IO_DMA_CMD);
		    while(ioread32(mmio + IO_DMA_CMD) & 0x1) {}

		    writeq(DMA_BASE, mmio + IO_DMA_SRC);
		    writeq(dma_handle2, mmio + IO_DMA_DST);
		    writeq(SIZE2, mmio + IO_DMA_CNT);
		    iowrite32(DMA_CMD | DMA_FROM_DEV, mmio + IO_DMA_CMD);
		    while(ioread32(mmio + IO_DMA_CMD) & 0x1) {}
		    
		    dma_sync_single_for_cpu(&(dev->dev), dma_handle2 + 43, 11, DMA_BIDIRECTIONAL);

		    memset(expected, 0xbb, SIZE2);
		    memset(expected + 43, 0xee, 11);
		    if (memcmp(expected, actual2, SIZE2) != 0) {
			pr_info("third partial buffer update failed");
			goto fail3;
		    }


		    pr_info("DMA test 3 passed");
		    goto unmap_3;

		fail3:
		    pr_info("DMA test 3 failed");
		unmap_3:
		    dma_unmap_single(&(dev->dev), dma_handle2, SIZE2, DMA_BIDIRECTIONAL);
		unmap_3_1:
		    dma_unmap_single(&(dev->dev), dma_handle1, SIZE1, DMA_BIDIRECTIONAL);
		kfree_3:
		    kfree(actual2);
		    kfree(actual1);
		    kfree(expected);
		}
		{
		    // test non-page alligned mappings
		    dev_info(&(dev->dev), "DMA Test 4\n");
		    dma_addr_t dma_handle1, dma_handle2;
		    enum { SIZE1 = 2048, SIZE2 = 2048 };
		    void *actual1 = kmalloc(SIZE1, GFP_KERNEL);
		    void *actual2 = kmalloc(SIZE2, GFP_KERNEL);
		    void *expected = kmalloc(SIZE1, GFP_KERNEL);

		    memset(actual1, 0x11, SIZE1);
		    memset(actual1 + 30, 0xdd, 20);
		    memset(actual2, 0x22, SIZE2);
		    memset(actual2 + 1500, 0xaa, 13);

		    dma_handle1 = dma_map_single(&(dev->dev), actual1 + 30, SIZE1 - 30, DMA_BIDIRECTIONAL);
		    if (dma_mapping_error(&(dev->dev), dma_handle1)) {
			pr_info("DMA test 4 failed with mapping error 1");
			goto kfree_4;
		    }
		    dma_handle2 = dma_map_single(&(dev->dev), actual2 + 1500, SIZE2 - 1500, DMA_BIDIRECTIONAL);
		    if (dma_mapping_error(&(dev->dev), dma_handle2)) {
			pr_info("DMA test 4 failed with mapping error 2");
			goto unmap_4_1;
		    }

		    // Change buffer outside of mapping and check if it affects DMA
		    memset(actual1 + 30, 0xab, 5);
		    dma_sync_single_for_device(&(dev->dev), dma_handle1, SIZE1 - 30, DMA_BIDIRECTIONAL);

		    writeq((u64)dma_handle1, mmio + IO_DMA_SRC);
		    writeq(DMA_BASE, mmio + IO_DMA_DST);
		    writeq(100, mmio + IO_DMA_CNT);
		    iowrite32(DMA_CMD, mmio + IO_DMA_CMD);
		    while(ioread32(mmio + IO_DMA_CMD) & 0x1) {}

		    writeq(DMA_BASE, mmio + IO_DMA_SRC);
		    writeq(dma_handle2 + 7, mmio + IO_DMA_DST);
		    writeq(50, mmio + IO_DMA_CNT);
		    iowrite32(DMA_CMD | DMA_FROM_DEV, mmio + IO_DMA_CMD);
		    while(ioread32(mmio + IO_DMA_CMD) & 0x1) {}

		    dma_sync_single_for_cpu(&(dev->dev), dma_handle2, SIZE2 - 1500, DMA_BIDIRECTIONAL);

		    // Results in a chaotic memory buffer
		    memset(expected, 0x22, SIZE2);
		    memset(expected + 1500, 0xaa, 13);
		    memset(expected + 1500 + 7, 0x11, 50);
		    memset(expected + 1500 + 7, 0xdd, 20);
		    memset(expected + 1500 + 7, 0xab, 5);
		    if (memcmp(expected, actual2, SIZE2) != 0) {
			pr_info("chaotic memory buffer update failed");
			goto fail4;
		    }


		    pr_info("DMA test 4 passed");
		    goto unmap_4;
		    goto fail4;

		fail4:
		    pr_info("DMA test 4 failed");
		unmap_4:
		    dma_unmap_single(&(dev->dev), dma_handle2, SIZE2 - 1500, DMA_BIDIRECTIONAL);
		unmap_4_1:
		    dma_unmap_single(&(dev->dev), dma_handle1, SIZE1 - 30, DMA_BIDIRECTIONAL);
		kfree_4:
		    kfree(actual2);
		    kfree(actual1);
		    kfree(expected);
		}
	}
    return 0;

error_requ_irq:
    pci_free_irq_vectors(dev);
error_irq_vectors:
    pci_iounmap(dev, mmio);
    pci_release_region(dev, PCI_BAR);
error_requ_reg:
    pci_disable_device(dev);
error:
    return 1;
}

static void my_pci_remove(struct pci_dev *dev)
{
    dev_info(&dev->dev, "my_pci_remove\n");
    free_irq(pci_irq_vector(dev, 0), &major);
    pci_free_irq_vectors(dev);
    pci_iounmap(dev, mmio);
    pci_disable_device(dev);
    pci_release_region(dev, PCI_BAR); /* has to be called after disabling device */
    unregister_chrdev(major, CDEV_NAME);
}


static struct pci_driver my_pci_driver = {
    .name = MY_DRIVER_NAME,
    .id_table = my_pci_ids,
    .probe = my_pci_probe,
    .remove = my_pci_remove,
};


/* Module handling */
static int __init my_init(void)
{
    if (pci_register_driver(&my_pci_driver) < 0) {
	pr_err("my_init: pci_reigster_driver failed\n");
	return 1;
    }
    return 0;
}

static void __exit my_exit(void)
{
    pci_unregister_driver(&my_pci_driver);
};

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Driver for the qemu EDU device");
module_init(my_init);
module_exit(my_exit);
