// ivshmem_driver.c

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <misc/qemu_ivshmem.h>
#include <crypto/aead.h>
#include <linux/scatterlist.h>

#define DRIVER_NAME "ivshmem_driver"

struct disagg_crypto {
    struct crypto_aead *tfm; // Handle to transformation object
    struct aead_request *req; // AEAD request which registers with the tfm object
    struct crypto_wait wait; // Used to make calls to crypto API synchronous
    size_t authsize;
    u8 *iv;
    u64 *counter; // for freshness, used as the iv
    struct scatterlist sg[3]; // used by both encryption and decryption
    struct scatterlist sg_enc[3]; // encryption output
    struct scatterlist sg_dec[2]; // decryption input
    u8 *buf_enc; // one-time allocated buffer for encryption output (including AD and auth)
    u8 *buf_dec; // one-time allocated buffer for decryption input
    size_t size_buffers; // size of buffers (both have same size)
};

struct ivshmem_dev {
    struct pci_dev *pdev;
    void __iomem *shmem;
    size_t shmem_size;
    struct disagg_crypto crypto;
};

static struct ivshmem_dev *ivs_dev_global;

#ifdef CONFIG_DISAGG_DEBUG_MMIO_SEC
static void my_print_hexdump(const char *prefix, const void *buf, size_t len) {
    print_hex_dump(KERN_INFO, prefix, DUMP_PREFIX_NONE, 32, 1, buf, len, false);
}
#endif

static int disagg_init_crypto(struct disagg_crypto *crypto, u8* key, int keylen)
{
    struct crypto_aead *tfm = NULL;
    struct aead_request *req = NULL;
    u8 *iv;
    int iv_size;
    crypto->authsize = 16; // size of the authentication code
    int adlen = 0; // No ad in our case

    // Create transformation object
    tfm = crypto_alloc_aead("gcm(aes)", 0, 0);
    if (IS_ERR(tfm)) {
	pr_err("disagg_init_crypto: AES/GCM alloc_aead failed\n");
	return 1;
    }

    // Init IV
    iv_size = crypto_aead_ivsize(tfm);
    pr_info("iv_size: %d", iv_size);
    if (iv_size < sizeof(crypto->counter)) {
	pr_info("Error: iv_size too small for this implementation");
	goto error_free_aead;
    }
    iv = kmalloc(iv_size, GFP_KERNEL);
    if (iv == NULL) {
	pr_err("disagg_init_crypto: kmalloc of IV-space failed\n");
	goto error_free_aead;
    }
    memset((void *) iv, 0x0, iv_size);
    // IV will alias the counter, allows freshness
    crypto->counter = (u64 *) iv;
    *crypto->counter = 0;

    // Set key 
    if (crypto_aead_setkey(tfm, key, keylen) < 0) {
	pr_err("disagg_init_crypto: setkey failed\n");
	goto error_free_aead;
    }

    // Set size of authentication code
    if (crypto_aead_setauthsize(tfm, crypto->authsize) < 0) {
	pr_err("disagg_init_crypto: setauthsize failed\n");
	goto error_free_aead;
    }

    // alloc buffers used in enc/dec
    crypto->size_buffers = crypto->authsize + 64;
    crypto->buf_enc = kmalloc(crypto->size_buffers, GFP_KERNEL); // extra 64 bytes for guest_message_header should be enough
    if (!crypto->buf_enc) {
	pr_err("disagg_init_crypto: kmalloc failed\n");
	goto error_free_aead;
    }
    crypto->buf_dec = kmalloc(crypto->size_buffers, GFP_KERNEL); // extra 64 bytes for guest_message_header should be enough
    if (!crypto->buf_dec) {
	pr_err("disagg_init_crypto: kmalloc failed\n");
	goto error_free_buf;
    }

    crypto_aead_clear_flags(tfm, ~0);

    // Obtain the request structures
    req = aead_request_alloc(tfm, GFP_KERNEL);
    if (req == NULL) {
	pr_err("disagg_init_crypto: request_alloc failed\n");
	goto error_free_buf2;
    }

    // Init wait object
    crypto_init_wait(&crypto->wait);

    // Set callback function which will never be called, because we wait synchronously
    aead_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG, crypto_req_done, &crypto->wait);

    // Set size of associated data
    // no AD in our case
    aead_request_set_ad(req, adlen);

    crypto->tfm = tfm;
    crypto->req = req;
    crypto->iv = iv;
    sg_set_buf(&crypto->sg_enc[0], crypto->buf_enc, crypto->size_buffers);
    sg_set_buf(&crypto->sg_dec[0], crypto->buf_dec, crypto->size_buffers);

    return 0;

error_free_buf2:
    kfree(crypto->buf_dec);
error_free_buf:
    kfree(crypto->buf_enc);
error_free_aead:
    crypto_free_aead(tfm);
    kfree(iv);
    return 1;
}

static int ivshmem_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    struct ivshmem_dev *ivs_dev;
    int err;

    ivs_dev = kzalloc(sizeof(*ivs_dev), GFP_KERNEL);
    if (!ivs_dev)
        return -ENOMEM;

    err = pci_enable_device(pdev);
    if (err)
        goto free_dev;

    err = pci_request_regions(pdev, DRIVER_NAME);
    if (err)
        goto disable_device;

    ivs_dev->shmem_size = pci_resource_len(pdev, 2);
    ivs_dev->shmem = pci_iomap(pdev, 2, ivs_dev->shmem_size);
    if (!ivs_dev->shmem) {
        err = -ENOMEM;
        goto release_regions;
    }

    ivs_dev->pdev = pdev;
    pci_set_drvdata(pdev, ivs_dev);
    ivs_dev_global = ivs_dev;

    pr_info("ivshmem: Shared memory size: %zu bytes\n", ivs_dev->shmem_size);

    // Initialize the GCM AEAD objects
    int keylen = 32;
    u8 *key = kmalloc(keylen, GFP_KERNEL);
    if (!key) {
	err = -ENOMEM;
	goto release_regions;
    }
    memset(key, 0x00, keylen); // init with dummy value
    if (disagg_init_crypto(&ivs_dev_global->crypto, key, keylen) != 0) {
	goto free_key;
    }
    if (disagg_dma_allocator_init(key, keylen, ivs_dev->shmem + DMA_REGION_OFFSET, DMA_SIZE) != 0) {
	goto free_key;
    }
    kfree(key);

    return 0;

free_key:
    kfree(key);
release_regions:
    pci_release_regions(pdev);
disable_device:
    pci_disable_device(pdev);
free_dev:
    kfree(ivs_dev);
    return err;
}

static void ivshmem_remove(struct pci_dev *pdev)
{
    struct ivshmem_dev *ivs_dev = pci_get_drvdata(pdev);
    struct disagg_crypto *crypto = &ivs_dev->crypto;

    pci_iounmap(pdev, ivs_dev->shmem);
    pci_release_regions(pdev);
    pci_disable_device(pdev);
    kfree(ivs_dev);

    aead_request_free(crypto->req);
    crypto_free_aead(crypto->tfm);
    kfree(crypto->iv);

    ivs_dev_global = NULL;
}

static const struct pci_device_id ivshmem_ids[] = {
    { PCI_DEVICE(0x1af4, 0x1110) },  // QEMU ivshmem device
    { 0 }
};
MODULE_DEVICE_TABLE(pci, ivshmem_ids);

static struct pci_driver ivshmem_driver = {
    .name = DRIVER_NAME,
    .id_table = ivshmem_ids,
    .probe = ivshmem_probe,
    .remove = ivshmem_remove,
};

static void wait_for_read_doorbell_set(void)
{
    while (readb(ivs_dev_global->shmem + READ_DOORBELL_OFFSET) == 0)
        cpu_relax();
}

static void wait_for_write_doorbell_clear(void)
{
    while (readb(ivs_dev_global->shmem + WRITE_DOORBELL_OFFSET) != 0)
        cpu_relax();
}

ssize_t ivshmem_read(void *buf, size_t count, loff_t offset)
{
    if (!ivs_dev_global || !ivs_dev_global->shmem)
        return -ENODEV;

    if (offset >= ivs_dev_global->shmem_size - TOTAL_DOORBELL_SIZE)
        return 0;

    if (offset + count > ivs_dev_global->shmem_size - TOTAL_DOORBELL_SIZE)
        count = ivs_dev_global->shmem_size - TOTAL_DOORBELL_SIZE - offset;

    wait_for_read_doorbell_set();

    memcpy_fromio(buf, ivs_dev_global->shmem + TOTAL_DOORBELL_SIZE + offset, count);

    writeb(0, ivs_dev_global->shmem + READ_DOORBELL_OFFSET);

    return count;
}
EXPORT_SYMBOL(ivshmem_read);

// another shared memory read to a non-mmio region (no need for doorbells)
// does really read at offset (not at offset + TOTAL_DOORBELL_SIZE like the other read)
ssize_t ivshmem_read_nonblocking(void *buf, size_t count, loff_t offset)
{
    if (!ivs_dev_global || !ivs_dev_global->shmem)
        return -ENODEV;

    if (offset >= ivs_dev_global->shmem_size - TOTAL_DOORBELL_SIZE)
        return 0;

    if (offset + count > ivs_dev_global->shmem_size - TOTAL_DOORBELL_SIZE)
        count = ivs_dev_global->shmem_size - TOTAL_DOORBELL_SIZE - offset;

    memcpy_fromio(buf, ivs_dev_global->shmem + offset, count);

    return count;
}
EXPORT_SYMBOL(ivshmem_read_nonblocking);

ssize_t ivshmem_write(const void *buf, size_t count, loff_t offset)
{
    if (!ivs_dev_global || !ivs_dev_global->shmem)
        return -ENODEV;

    if (offset >= ivs_dev_global->shmem_size - TOTAL_DOORBELL_SIZE)
        return -ENOSPC;

    if (offset + count > ivs_dev_global->shmem_size - TOTAL_DOORBELL_SIZE)
        count = ivs_dev_global->shmem_size - TOTAL_DOORBELL_SIZE - offset;

    wait_for_write_doorbell_clear();

    memcpy_toio(ivs_dev_global->shmem + TOTAL_DOORBELL_SIZE + offset, buf, count);

    writeb(1, ivs_dev_global->shmem + WRITE_DOORBELL_OFFSET);

    return count;
}
EXPORT_SYMBOL(ivshmem_write);

static int __init ivshmem_init(void)
{
    return pci_register_driver(&ivshmem_driver);
}

static void __exit ivshmem_exit(void)
{
    pci_unregister_driver(&ivshmem_driver);
}

module_init(ivshmem_init);
module_exit(ivshmem_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Harshavardhan Unnibhavi");
MODULE_DESCRIPTION("QEMU ivshmem PCI driver with polling synchronization");

