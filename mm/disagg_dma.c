#include <linux/mm.h>
#include <misc/qemu_ivshmem.h>
#include <crypto/aead.h>
#include <linux/scatterlist.h>
#include <linux/pci.h> // for dev_is_pci

disagg_dma_allocator_t disagg_dma_allocator;

#ifdef CONFIG_DISAGG_DEBUG_DMA_SEC
static void my_print_hexdump(const char *prefix, const void *buf, size_t len) {
    print_hex_dump(KERN_INFO, prefix, DUMP_PREFIX_NONE, 32, 1, buf, len, false);
}
#endif

static void *proxyDMA_to_vmShmem(u64 proxyDMA) {
    if (disagg_dma_allocator.proxyDMA_start > (u64) disagg_dma_allocator.vmShmem_start)
	return (void *) proxyDMA - ((void *)disagg_dma_allocator.proxyDMA_start - disagg_dma_allocator.vmShmem_start);
    else
	return (void *) proxyDMA + (disagg_dma_allocator.vmShmem_start - (void *) disagg_dma_allocator.proxyDMA_start);
}

/*
 * Adds the memory region (@proxyDMA, @size) to the free list.
 * Expects the region to not be in the list.
 * Inserts in a sorted manner.
 * Coalesces with neighbours if possible.
 */
static void add_region_to_free_list(u64 proxyDMA, size_t size) {
    struct list_head *next = &disagg_dma_allocator.free_list; // will be the right/next neighbour; means the region has to be inserted before
    struct list_head *prev; // Will be the left/prev neighbour
    u8 set = 0; // Flag to indicate if the region is already inserted into the list, one way or another
    struct memory_region *next_region = NULL;
    struct memory_region *prev_region = NULL;
    size = PAGE_ALIGN(size); // Normally only used to align address, but should also work for this

    list_for_each(next, &disagg_dma_allocator.free_list) {
	struct memory_region *data = list_entry(next, struct memory_region, list);

	if (data->proxyDMA > proxyDMA) {
	    // Found right spot
	    break; 
	}
    }

    prev = next->prev;

    // Now coalesce with the neighbours if possible
    // First previous, then next
    // I know those cascading ifs are terrible, but cannot think of another way right now. (TODO)
    if (!list_is_head(prev, &disagg_dma_allocator.free_list)) {
	prev_region = list_entry(prev, struct memory_region, list);

	if (prev_region->proxyDMA + prev_region->size == proxyDMA) {
	    prev_region->size += size;

	    set = 1;
	}
    } 
    
    if (!list_is_head(next, &disagg_dma_allocator.free_list)) {
	next_region = list_entry(next, struct memory_region, list);

	if (next_region->proxyDMA == proxyDMA + size) {
	    if (set == 1) {
		prev_region->size += next_region->size;
		list_del(next);
		kfree(next_region);
	    } else {
		next_region->size += size;
		next_region->proxyDMA = proxyDMA;
		set = 1;
	    }
	}
    }

    if (set == 0) {
	// No coalescing happened, insert it alone-standing
	struct memory_region *new = kmalloc(sizeof(struct memory_region), GFP_KERNEL);
	if (new == NULL) {
	    pr_err("kmalloc failed");
	    return;
	}

	new->proxyDMA = proxyDMA;
	new->size = size;
	list_add(&new->list, prev);
    }
}

/*
 * Removes the specified @size from @region 
 * If @size == @region->size then it removes the entry completely from the list
 * Expects that @size <= @region->size
 */
static void remove_region(struct memory_region *region, size_t size) {
    if (size == region->size) {
	list_del(&region->list);
	kfree(region);
    } else {
	region->size -= size;
	region->proxyDMA += size;
    }
}

/*
 * Searches for at least a size long free area.
 * Just a simple first fit.
 * @return 0 for success
 * @return in @proxyDMA the address
 */
static int find_free_region(size_t size, dma_addr_t *proxyDMA) {

    struct list_head *crt = &disagg_dma_allocator.free_list;
    
    if (list_empty(crt)) {
	pr_err("find_free_region: no buffer available");
	return 1;
    }

    size = PAGE_ALIGN(size); // Normally only used to align address, but should also work for this

    list_for_each(crt, &disagg_dma_allocator.free_list) {
	struct memory_region *data = list_entry(crt, struct memory_region, list);
	
	// Found big enough free buffer
	if (data->size >= size) {
	    *proxyDMA = data->proxyDMA;
	    remove_region(data, size);
	    return 0;
	}
    }

    return 1;
}


/*
 * Looks for entry containing the specified range (addr, size)
 * @return NULL for no corresponding entry, the entry otherwise
 * (Many things copied from https://www.kernel.org/doc/html/latest/core-api/rbtree.html)
 */
static struct disagg_dma_entry *disagg_find_entry(dma_addr_t proxyDMA, size_t size)
{
    struct rb_node *crt_node = disagg_dma_allocator.entry_root.rb_node;

    while (crt_node) {
	struct disagg_dma_entry *entry = container_of(crt_node, struct disagg_dma_entry, node);

	if (proxyDMA < entry->proxyDMA)
	    crt_node = crt_node->rb_left;
	else if (proxyDMA + size > entry->proxyDMA + entry->size)
	    crt_node = crt_node->rb_right;
	else 
	    return entry;
    }

    return NULL;
}

/*
 * Inserts the entry into the rb-tree.
 * Expects that the tree does not already contain an entry with this region.
 * (Many things copied from https://www.kernel.org/doc/html/latest/core-api/rbtree.html)
 */
static void disagg_insert_entry(struct disagg_dma_entry *new_entry)
{
    struct rb_node **crt = &(disagg_dma_allocator.entry_root.rb_node);
    struct rb_node *parent = NULL;

    while (*crt) {
	struct disagg_dma_entry *data = container_of(*crt, struct disagg_dma_entry, node);
	parent = *crt;

	// This simple compare is enough as we expect the entry to be unique
	if (new_entry->proxyDMA < data->proxyDMA)
	    crt = &((*crt)->rb_left);
	else 
	    crt = &((*crt)->rb_right);
    }

    rb_link_node(&new_entry->node, parent, crt);
    rb_insert_color(&new_entry->node, &disagg_dma_allocator.entry_root);
}

bool disagg_is_dev(struct device *dev) 
{
    struct pci_dev *pdev;

    if (dev_is_pci(dev)) {
	pdev = container_of(dev, struct pci_dev, dev);
    } else {
	return false;
    }

    if (unlikely((pdev->vendor == 0x1234) && (pdev->device == 0x11e8))) {
	return true;
    } else {
	return false;
    }
}

// Requests proxies dma address and writes it into field of disagg_dma_allocator
// Those addresse can then be used to convert from proxyDMA to vmShmem
static int obtain_proxy_address(void) {
    struct guest_message_header hdr;
    u8 *resp = kmalloc(sizeof(void *) * 2, GFP_KERNEL);
    if (resp == NULL) {
	pr_err("kmalloc_failed");
	return 1;
    }

    hdr.address = 0;
    hdr.operation = DISAGG_DEV_OP_ADDR_INIT;
    hdr.length = 8;
    ivshmem_write(&hdr, sizeof(hdr), 0);

    ivshmem_read(resp, 8, 0);

    disagg_dma_allocator.proxyDMA_start = *((u64 *) resp);

    return 0;
}

int disagg_dma_allocator_init(u8 *key, int keylen, void *vmShmem_start, size_t dma_area_size)
{
	pr_info("disagg_dma_allocator_init");
        disagg_dma_allocator.vmShmem_start = vmShmem_start;
	disagg_dma_allocator.dma_area_size = dma_area_size;
	disagg_dma_allocator.entry_root = RB_ROOT;
	INIT_LIST_HEAD(&disagg_dma_allocator.free_list);
	spin_lock_init(&disagg_dma_allocator.lock);

	disagg_dma_allocator.crypto.authsize = 16; // size of the authentication code
	struct crypto_aead *tfm = NULL;
	struct aead_request *req = NULL;
	u8 *iv = NULL;
	int iv_size = 0;
	int adlen = 0; // No ad in our case

	// Create transformation object
	tfm = crypto_alloc_aead("gcm(aes)", 0, 0);
	if (IS_ERR(tfm)) {
	    pr_err("disagg_dma_allocator_init: AES/GCM alloc_aead failed\n");
	    return 1;
	}

	// Init IV
	iv_size = crypto_aead_ivsize(tfm);
	pr_info("iv_size: %d", iv_size);
	if (iv_size < sizeof(disagg_dma_allocator.crypto.counter)) {
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
	disagg_dma_allocator.crypto.counter = (u64 *) iv;
	*disagg_dma_allocator.crypto.counter = 0;

	// Init and set key 
	memset((void *) key, 0x00, keylen);
	if (crypto_aead_setkey(tfm, key, keylen) < 0) {
	    pr_err("disagg_init_crypto: setkey failed\n");
	    goto error_free_aead;
	}

	// Set size of authentication code
	if (crypto_aead_setauthsize(tfm, disagg_dma_allocator.crypto.authsize) < 0) {
	    pr_err("disagg_init_crypto: setauthsize failed\n");
	    goto error_free_aead;
	}

	crypto_aead_clear_flags(tfm, ~0);

	// Obtain the request structures
	req = aead_request_alloc(tfm, GFP_KERNEL);
	if (req == NULL) {
	    pr_err("disagg_init_crypto: request_alloc failed\n");
	    goto error_free_aead;
	}

	// Init wait object
	crypto_init_wait(&disagg_dma_allocator.crypto.wait);

	// Set callback function which will never be called, because we wait synchronously
	aead_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG, crypto_req_done, &disagg_dma_allocator.crypto.wait);

	// Set size of associated data
	// no AD in our case
	aead_request_set_ad(req, adlen);

	disagg_dma_allocator.crypto.tfm = tfm;
	disagg_dma_allocator.crypto.req = req;
	disagg_dma_allocator.crypto.iv = iv;

	if (obtain_proxy_address() != 0) {
	    pr_err("get_proxy_addresses failed\n");
	    goto error_free_aead;
	}

	// Add the initial free memory region, which contains the whole free dma area
	struct memory_region *first_region = kmalloc(sizeof(struct memory_region), GFP_KERNEL);
	if (first_region == NULL) {
	    pr_err("kmalloc failed");
	    goto error_free_aead;
	}
	first_region->proxyDMA = disagg_dma_allocator.proxyDMA_start;
	first_region->size = disagg_dma_allocator.dma_area_size;
	list_add(&first_region->list, &disagg_dma_allocator.free_list);

	return 0;
error_free_aead:
	crypto_free_aead(tfm);
	kfree(iv);
	return 1;
}

static int disagg_dma_encrypt(void *from, void *to, size_t size)
{
#ifdef CONFIG_DISAGG_DEBUG_DMA_SEC
    pr_info("disagg_dma_encrypt:\n");
    pr_info("counter: %llu", *disagg_dma_allocator.crypto.counter);
    my_print_hexdump("Plaintext: ", from, size);
#endif

    // We use a simple memcpy, even though one should rather use memcpy_toio().
    // But because the crypto API presumably also only does a simple copy, we 
    // do the same here.
    memcpy(to, from, size);

#ifdef CONFIG_DISAGG_DEBUG_DMA_SEC
    pr_info("cipher-size (only encrypted data): %ld\n", size);
    my_print_hexdump("ciphertext: ", to, size);
    my_print_hexdump("Auth tag: ", to + size, disagg_dma_allocator.crypto.authsize);
    pr_info("\n");
#endif

    return 0;
}

static int disagg_dma_decrypt(void *from, void *to, size_t size)
{
#ifdef CONFIG_DISAGG_DEBUG_DMA_SEC
    pr_info("disagg_dma_decrypt:\n");
    pr_info("counter: %llu", *disagg_dma_allocator.crypto.counter);
    pr_info("cipher-size (only encrypted data): %ld\n", size);
    my_print_hexdump("ciphertext: ", from, size);
    my_print_hexdump("Auth Tag: ", from + size, disagg_dma_allocator.crypto.authsize);
#endif

    // We use a simple memcpy, even though one should rather use memcpy_fromio().
    // But because the crypto API presumably also only does a simple copy, we 
    // do the same here.
    memcpy(to, from, size);

#ifdef CONFIG_DISAGG_DEBUG_DMA_SEC
    my_print_hexdump("Plaintext: ", to, size);
    pr_info("\n");
#endif

    return 0;
}

/* Allocates a dmu buffer from the shmem region */
dma_addr_t disagg_dma_map_page_attrs(struct device *dev, struct page *page, size_t offset, size_t size, enum dma_data_direction dir, unsigned long attrs) 
{
    struct guest_message_header hdr;
    struct disagg_dma_entry *new_entry;
    void *vmDMA = page_to_virt(page) + offset;
    void *vmShmem;
    dma_addr_t proxyDMA;
    u8 resp;

#ifdef CONFIG_DISAGG_DEBUG_DMA_SEC
    pr_info("disagg_dma_map_page_attrs\n");
#endif

    if (disagg_dma_allocator.vmShmem_start == NULL) {
	    pr_err("disagg_dma_map_page_attrs: shared memory not yet ready\n");
	    return DMA_MAPPING_ERROR;
    }

    spin_lock(&disagg_dma_allocator.lock);

    // just a simple one page allocator
    if (find_free_region(size, &proxyDMA) != 0) {
	pr_err("disagg_dma_map_page_attrs: request not fulfillable");
	goto error;
    }

    new_entry = kmalloc(sizeof(struct disagg_dma_entry), GFP_KERNEL);

    new_entry->vmDMA = vmDMA;
    new_entry->proxyDMA = proxyDMA;
    new_entry->size = size;

    disagg_insert_entry(new_entry);
    // end of allocator

    vmShmem = proxyDMA_to_vmShmem(proxyDMA);

    // Encrypt the data to shmem
    disagg_dma_encrypt(vmDMA, vmShmem, size);

    // Provide proxy with information where the encrypted data is placed into shmem
    hdr.address = proxyDMA;
    hdr.operation = DISAGG_DEV_OP_DMA_MAP;
    hdr.length = size;
    ivshmem_write(&hdr, sizeof(hdr), 0);

    // confirmation for completion of decryption
    ivshmem_read(&resp, 1, 0);

    spin_unlock(&disagg_dma_allocator.lock);

#ifdef CONFIG_DISAGG_DEBUG_DMA_SEC
    pr_info("disagg_dma_map_page: dma_handle: 0x%llx\n", (uint64_t) proxyDMA);
#endif

    return proxyDMA;

error:
    spin_unlock(&disagg_dma_allocator.lock);
    pr_info("disagg_dma_map_page failed\n");
    return DMA_MAPPING_ERROR;
}

void disagg_dma_unmap_page_attrs(struct device *dev, dma_addr_t proxyDMA, size_t size, enum dma_data_direction dir, unsigned long attrs)
{
    struct disagg_dma_entry *entry;

    spin_lock(&disagg_dma_allocator.lock);

    entry = disagg_find_entry(proxyDMA, size);

    if (entry == NULL) {
	pr_err("disagg_dma_free: cannot free non-existent dma buffer\n");
	goto error;
    }

    rb_erase(&entry->node, &disagg_dma_allocator.entry_root);
    kfree(entry);

    add_region_to_free_list(proxyDMA, size); 

    spin_unlock(&disagg_dma_allocator.lock);

    return;

error:
    spin_unlock(&disagg_dma_allocator.lock);
}

void disagg___dma_sync_single_for_cpu(struct device *dev, dma_addr_t proxyDMA, size_t size, enum dma_data_direction dir)
{
    struct guest_message_header hdr;
    u8 res;
    u64 offset;
    struct disagg_dma_entry *entry;

    spin_lock(&disagg_dma_allocator.lock);

    entry = disagg_find_entry(proxyDMA, size);
    if (entry == NULL) {
	pr_info("disagg___dma_sync_single_for_cpu: no entry corresponding to the arguments\n");
	goto error;
    }

    offset = proxyDMA - entry->proxyDMA;

#ifdef CONFIG_DISAGG_DEBUG_DMA_SEC
    pr_info("disagg___dma_sync_single_for_cpu\n");
#endif

    // Provide proxy with information where to encrypt the data inside shmem to
    hdr.address = (u64) proxyDMA;
    hdr.operation = DISAGG_DEV_OP_DMA_ENC;
    hdr.length = size;
    ivshmem_write(&hdr, sizeof(hdr), 0);

    // Confirm completion of encryption
    ivshmem_read(&res, sizeof(res), 0);

    // Decrypt data into virtual address space
    disagg_dma_decrypt(proxyDMA_to_vmShmem(proxyDMA), entry->vmDMA + offset, size);

    spin_unlock(&disagg_dma_allocator.lock);

    return;
error:
    spin_unlock(&disagg_dma_allocator.lock);
}

void disagg___dma_sync_single_for_device(struct device *dev, dma_addr_t proxyDMA, size_t size, enum dma_data_direction dir)
{
    struct guest_message_header hdr;
    u8 res;
    u64 offset;
    struct disagg_dma_entry *entry;

#ifdef CONFIG_DISAGG_DEBUG_DMA_SEC
    pr_info("disagg___dma_sync_single_for_device\n");
#endif

    spin_lock(&disagg_dma_allocator.lock);

    entry = disagg_find_entry(proxyDMA, size);
    if (entry == NULL) {
	pr_info("disagg___dma_sync_single_for_device: no entry corresponding to the arguments\n");
	goto error;
    }

    offset = proxyDMA - entry->proxyDMA;
    
    // Encrypt data into virtual address space
    disagg_dma_encrypt(entry->vmDMA + offset, proxyDMA_to_vmShmem(proxyDMA), size);

    // Give proxy source address of decrypted data in shmem
    hdr.address = (u64) proxyDMA;
    hdr.operation = DISAGG_DEV_OP_DMA_DEC;
    hdr.length = size;
    ivshmem_write(&hdr, sizeof(hdr), 0);

    // Confirm completion of encryption
    ivshmem_read(&res, sizeof(res), 0);

    spin_unlock(&disagg_dma_allocator.lock);

    return;

error:
    spin_unlock(&disagg_dma_allocator.lock);
    pr_info("disagg___dma_sync_single_for_device failed\n");
}

bool disagg_test_check_dma_values(size_t nodes, size_t idx, size_t size_at_idx) {
    if (list_count_nodes(&disagg_dma_allocator.free_list) != nodes) {
	pr_err("disagg_test_check_dma_values: failed for nodes; expected: %lu, actual: %lu", nodes, list_count_nodes(&disagg_dma_allocator.free_list));
       return false;	
    }

    struct list_head *pos;
    for (pos = disagg_dma_allocator.free_list.next; idx > 0; --idx, pos = pos->next) { }

    struct memory_region *region = list_entry(pos, struct memory_region, list);
    if (region->size != size_at_idx) {
	pr_err("disagg_test_check_dma_values: failed for size_at_idx; expected: %lu, actual: %lu", size_at_idx, region->size);
       return false;	
    }
    
    return true;
}
EXPORT_SYMBOL(disagg_test_check_dma_values);
