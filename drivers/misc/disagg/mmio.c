#include <crypto/aead.h>
#include <linux/scatterlist.h>

#include <misc/qemu_ivshmem.h>

#include "internal.h"

static struct mmio_message *msg;

static u64 agg_read = 0;
static u64 agg_write = 0;

//static u64 max = 100000;
//static u64 ctr = 0;

int mmio_read(u64 size, u64 addr, unsigned long *val)
{
ktime_t start, end;
	u64 offset = disagg_ioremap_virt_to_offset(addr);

#ifdef CONFIG_DISAGG_DEBUG_MMIO
	pr_info("mmio_read: Address: %llx\n", addr);
#endif

	msg->operation = DISAGG_DEV_OP_READ;
	msg->address = offset;
	msg->length = size;

	// First byte of buf is reserved for unencrypted OP_TYPE
	msg->op = DISAGG_DEV_OP_READ;

start = ktime_get();
	ivshmem_mmio_region_write(msg, (sizeof(*msg) - sizeof(msg->value)));
end = ktime_get();
agg_write += (u64) ktime_to_ns(end) - (u64) ktime_to_ns(start);
//pr_info("time measured pie: Shmem write;%llu;%llu end\n", size, (u64) ktime_to_ns(end) - (u64) ktime_to_ns(start));

start = ktime_get();
	ivshmem_mmio_region_read(val, sizeof(msg->value));
end = ktime_get();
agg_read += (u64) ktime_to_ns(end) - (u64) ktime_to_ns(start);
//pr_info("time measured pie: Shmem read;%llu;%llu end\n", size, (u64) ktime_to_ns(end) - (u64) ktime_to_ns(start));

#if 0
if (++ctr == max) {
pr_info("time measured pie: Shmem write;%llu;%llu end\n", size, agg_write / max);
pr_info("time measured pie: Shmem read;%llu;%llu end\n", size, agg_read / max);
ctr = 0;
agg_read = 0;
agg_write = 0;
}
#endif
	return 0;
}

int mmio_write(u64 size, u64 addr, unsigned long val)
{
	u64 offset = disagg_ioremap_virt_to_offset(addr);

#ifdef CONFIG_DISAGG_DEBUG_MMIO
	pr_info("mmio_write: Address: %llx\n", addr);
#endif

	msg->operation = DISAGG_DEV_OP_WRITE;
	msg->address = offset;
	msg->length = size;
	msg->value = val;

	// First byte of buf is reserved for unencrypted OP_TYPE
	msg->op = DISAGG_DEV_OP_WRITE;

	ivshmem_mmio_region_write(msg, sizeof(*msg));

	return 0;
}


int disagg_init_mmio(u8 *key, int keylen)
{
	// Alloc message structure object
	msg = kmalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;
	return 0;
}

void disagg_exit_mmio(void)
{
	kfree(msg);
}

