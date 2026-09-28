/* virtio.c - the virtio transport, over the modern PCI interface.
 *
 * Reads a device's common configuration, negotiates features, and lays out
 * its queues.  Moving data is the driver's job and is not here.
 */

#include "mmu.h"
#include "virtio.h"

#include "heap.h"
#include "pci.h"

/* Offsets in the common configuration block.  It is published in the device's
 * memory BAR, so these are byte offsets into a structure the host wrote. */
#define COMMON_FEATURE_SELECT 0x00
#define COMMON_DEVICE_FEATURE 0x04
#define COMMON_DRIVER_FEATURE_SELECT 0x08
#define COMMON_DRIVER_FEATURE 0x0C
#define COMMON_MSIX_CONFIG 0x10
#define COMMON_NUM_QUEUES 0x12
#define COMMON_STATUS 0x14
#define COMMON_GENERATION 0x15
#define COMMON_QUEUE_SELECT 0x16
#define COMMON_QUEUE_SIZE 0x18
#define COMMON_QUEUE_MSIX 0x1A
#define COMMON_QUEUE_ENABLE 0x1C
#define COMMON_QUEUE_NOTIFY_OFF 0x1E
#define COMMON_QUEUE_DESC 0x20
#define COMMON_QUEUE_DRIVER 0x28
#define COMMON_QUEUE_DEVICE 0x30

/* The device id this driver accepts, and whether it is one it can drive. */
#define VIRTIO_ID_BLOCK 1

static struct virtio_device device;
static char error_text[64] = "";

static void fail(const char *why)
{
    __builtin_strncpy(error_text, why, sizeof(error_text) - 1);
    error_text[sizeof(error_text) - 1] = 0;
}

const char *virtio_error(void)
{
    return error_text;
}

/* The common block and the notify area are device memory, so every access is
 * volatile.  The compiler has no way to know another agent changes them, and
 * without this it will happily hoist a read of the status byte out of a loop
 * and the loop never ends. */
static void common_write8(const struct virtio_device *d, uint16_t offset,
                          uint8_t value)
{
    d->common[offset] = value;
}

static uint8_t common_read8(const struct virtio_device *d, uint16_t offset)
{
    return d->common[offset];
}

static void common_write16(const struct virtio_device *d, uint16_t offset,
                           uint16_t value)
{
    d->common[offset] = (uint8_t)(value & 0xFFU);
    d->common[(uint16_t)(offset + 1)] = (uint8_t)(value >> 8);
}

static uint16_t common_read16(const struct virtio_device *d, uint16_t offset)
{
    return (uint16_t)(d->common[offset] |
                      ((uint16_t)d->common[(uint16_t)(offset + 1)] << 8));
}

static void common_write32(const struct virtio_device *d, uint16_t offset,
                           uint32_t value)
{
    d->common[offset] = (uint8_t)(value & 0xFFU);
    d->common[(uint16_t)(offset + 1)] = (uint8_t)((value >> 8) & 0xFFU);
    d->common[(uint16_t)(offset + 2)] = (uint8_t)((value >> 16) & 0xFFU);
    d->common[(uint16_t)(offset + 3)] = (uint8_t)((value >> 24) & 0xFFU);
}

static uint32_t common_read32(const struct virtio_device *d, uint16_t offset)
{
    return (uint32_t)d->common[offset] |
           ((uint32_t)d->common[(uint16_t)(offset + 1)] << 8) |
           ((uint32_t)d->common[(uint16_t)(offset + 2)] << 16) |
           ((uint32_t)d->common[(uint16_t)(offset + 3)] << 24);
}

static void common_write64(const struct virtio_device *d, uint16_t offset,
                           uint64_t value)
{
    common_write32(d, offset, (uint32_t)(value & 0xFFFFFFFFU));
    common_write32(d, (uint16_t)(offset + 4), (uint32_t)(value >> 32));
}

/* A device's registers are its own memory, read through the mapping made when
 * the base address register was sized. */
static void mmio_write16(uint64_t base, uint16_t offset, uint16_t value)
{
    *(volatile uint16_t *)(uintptr_t)(base + offset) = value;
}

struct virtio_device *virtio_find(uint16_t device_id)
{
    int count = pci_device_count();
    const struct pci_device *found = 0;
    uint16_t want = device_id;

    if (count == 0) {
        pci_enumerate();
    }
    for (int index = 0; index < pci_device_count(); ++index) {
        const struct pci_device *candidate = pci_device_at(index);

        if (candidate->vendor != VIRTIO_VENDOR) {
            continue;
        }
        /* A modern device reports 0x1040 plus the legacy id it is compatible
         * with, so 0x1001 and 0x1041 name the same device. */
        if (candidate->device_id == want ||
            candidate->device_id == (uint16_t)(VIRTIO_PCI_MODERN + want)) {
            found = candidate;
            break;
        }
    }
    if (found == 0) {
        return 0;
    }
    device.pci = found;
    device.device_id = device_id;
    device.modern = found->device_id >= VIRTIO_PCI_MODERN;
    device.notify_offset_multiplier = 1U;
    return &device;
}

/* Read both halves of the feature bitmap.
 *
 * The device publishes 64 features as two 32 bit words, selected one at a
 * time by a write to the select register.  Asking for word 0 and word 1 with
 * separate writes and reads is the whole protocol; there is no way to get
 * both at once, and a driver that tries will latch the select and get the
 * same word twice. */
static void read_features(struct virtio_device *d)
{
    common_write32(d, COMMON_FEATURE_SELECT, 0U);
    d->device_features = common_read32(d, COMMON_DEVICE_FEATURE);
    common_write32(d, COMMON_FEATURE_SELECT, 1U);
    d->device_features |= (uint64_t)common_read32(d, COMMON_DEVICE_FEATURE) << 32;
}

/* The features this driver is willing to agree to.
 *
 * Only the ones that change how the ring is driven.  A feature the driver
 * ignores but accepts would be a promise it cannot keep, and a feature it
 * rejects when the device requires it is a negotiation failure, so the
 * required bits are checked rather than assumed away. */
#define VIRTIO_F_VERSION_1 32
#define VIRTIO_F_RING_INDIRECT_DESC 28
#define VIRTIO_F_RING_EVENT_IDX 29

static uint64_t supported_features(void)
{
    return (1ULL << VIRTIO_F_VERSION_1) | (1ULL << VIRTIO_F_RING_EVENT_IDX) |
           (1ULL << VIRTIO_F_RING_INDIRECT_DESC);
}

static uint64_t required_features(void)
{
    /* version 1 is the modern ring layout, which is the only one here */
    return 1ULL << VIRTIO_F_VERSION_1;
}

/* Lay out one queue's three rings.
 *
 * They have to be physically contiguous, because the device is given physical
 * addresses and does no translation.  That is fine here and will not be in
 * general: boot code identity maps the low memory, so a heap address is also
 * the address the device should use.  A driver that needs a buffer the heap
 * cannot make contiguous will have to build a bounce buffer or an indirect
 * descriptor table, and the queue layout below is what has to accommodate
 * that. */
static bool set_up_queue(struct virtio_device *d, int index)
{
    struct virtio_queue *queue = &d->queue[index];
    size_t avail_bytes;
    size_t used_bytes;

    common_write16(d, COMMON_QUEUE_SELECT, (uint16_t)index);
    queue->select = (uint16_t)index;
    queue->size = common_read16(d, COMMON_QUEUE_SIZE);
    if (queue->size == 0U) {
        fail("the device reported a queue of no entries");
        return false;
    }

    avail_bytes = sizeof(struct virtio_avail) +
                  (size_t)queue->size * sizeof(uint16_t);
    used_bytes = sizeof(struct virtio_used) +
                 (size_t)queue->size * sizeof(struct virtio_used_elem);
    queue->desc = heap_calloc(queue->size, sizeof(*queue->desc));
    queue->avail = heap_calloc(1, avail_bytes);
    queue->used = heap_calloc(1, used_bytes);
    if (queue->desc == 0 || queue->avail == 0 || queue->used == 0) {
        fail("out of memory for the queue rings");
        return false;
    }
    /* both index words start at zero, and the flags words ask the device not
     * to interrupt */
    queue->avail->flags = 1U;
    queue->avail->index = 0U;
    queue->used->flags = 0U;
    queue->used->index = 0U;

    common_write64(d, COMMON_QUEUE_DESC,
                   (uint64_t)(uintptr_t)queue->desc);
    common_write64(d, COMMON_QUEUE_DRIVER,
                   (uint64_t)(uintptr_t)queue->avail);
    common_write64(d, COMMON_QUEUE_DEVICE,
                   (uint64_t)(uintptr_t)queue->used);
    common_write16(d, COMMON_QUEUE_ENABLE, 1U);
    queue->next_free = 0;
    queue->last_used = 0;
    queue->ready = true;
    return true;
}

bool virtio_setup(struct virtio_device *d)
{
    uint64_t wanted;
    uint64_t required;

    error_text[0] = 0;
    if (d == 0 || d->pci == 0) {
        fail("no device");
        return false;
    }
    if (d->pci->mapped == false) {
        fail("the device has no mapping; run pci map first");
        return false;
    }

    /* The modern interface puts the common block in the first 4 KiB BAR and
     * the notification area in the 16 KiB one.  Which register that is depends
     * on the device, so they are found by size rather than by index: a device
     * that puts them elsewhere is a device this does not drive yet. */
    /* The modern interface is one 16 KiB register with four things at fixed
     * offsets inside it: the common configuration at 0, the interrupt status
     * at 0x1000, the device's own configuration at 0x2000, and the
     * notification area at 0x3000.  It is one register, not four, so looking
     * for a 4 KiB and a 16 KiB register separately finds the wrong one and
     * reads a bar the device never uses. */
    /* Which register holds the common configuration is not something this
     * driver gets to assume, so it is found by writing the status byte to
     * each candidate and seeing which one keeps it.  A device ignores writes
     * to a register it does not own, so the one that sticks is the one. */
    {
        volatile uint8_t *found = 0;

        for (int i = 0; i < 6 && found == 0; ++i) {
            const struct pci_bar *bar = &d->pci->bar[i];
            volatile uint8_t *base;

            if (bar->present == false || bar->is_io != false ||
                bar->mapped == false) {
                continue;
            }
            base = (volatile uint8_t *)(uintptr_t)bar->virtual_address;
            base[COMMON_STATUS] = 0U;
            base[COMMON_STATUS] = VIRTIO_STATUS_ACK;
            if (base[COMMON_STATUS] == VIRTIO_STATUS_ACK) {
                found = base;
            }
        }
        if (found == 0) {
            fail("no memory register kept the status byte");
            return false;
        }
        d->common = found;
        d->notify = found + 0x3000;
    }

    /* start from a device that has been reset, so a second setup does not
     * inherit whatever the first one left behind */
    common_write8(d, COMMON_STATUS, 0U);
    common_write8(d, COMMON_STATUS, VIRTIO_STATUS_ACK);
    /* read it back before going further.  A device that has not been
     * acknowledged is entitled to answer reads of the common configuration
     * with zero, so a feature bitmap of all zeros here means one of two very
     * different things: a device with no features, which is not a thing, or a
     * write that did not land.  The status byte tells them apart. */
    if (common_read8(d, COMMON_STATUS) != VIRTIO_STATUS_ACK) {
        fail("the status byte did not stick, so the register is not writable");
        return false;
    }
    common_write8(d, COMMON_STATUS,
                  (uint8_t)(VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER));

    read_features(d);
    required = required_features();
    if ((d->device_features & required) != required) {
        fail("the device does not offer the features this driver requires");
        return false;
    }
    wanted = d->device_features & supported_features();
    common_write32(d, COMMON_DRIVER_FEATURE_SELECT, 0U);
    common_write32(d, COMMON_DRIVER_FEATURE,
                   (uint32_t)(wanted & 0xFFFFFFFFU));
    common_write32(d, COMMON_DRIVER_FEATURE_SELECT, 1U);
    common_write32(d, COMMON_DRIVER_FEATURE, (uint32_t)(wanted >> 32));
    d->driver_features = wanted;
    common_write8(d, COMMON_STATUS,
                  (uint8_t)(VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER |
                            VIRTIO_STATUS_FEATURES_OK));

    d->queue_count = common_read16(d, COMMON_NUM_QUEUES);
    if (d->queue_count == 0U) {
        fail("the device has no queues");
        return false;
    }
    for (int index = 0; index < (int)d->queue_count && index < 2; ++index) {
        if (set_up_queue(d, index) == false) {
            return false;
        }
    }
    common_write8(d, COMMON_STATUS,
                  (uint8_t)(VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER |
                            VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK));
    d->ready = true;
    return true;
}

uint8_t virtio_status(const struct virtio_device *d)
{
    if (d == 0 || d->common == 0) {
        return 0U;
    }
    return d->common[COMMON_STATUS];
}

int virtio_queue_free(const struct virtio_device *d, int index)
{
    const struct virtio_queue *queue;

    if (d == 0 || index < 0 || index >= 2 || d->queue[index].ready == false) {
        return -1;
    }
    queue = &d->queue[index];
    /* the available ring's index of the next entry to be taken */
    return (int)queue->size - (int)queue->avail->index -
           (int)queue->next_free;
}

int virtio_queue_used(const struct virtio_device *d, int index)
{
    const struct virtio_queue *queue;

    if (d == 0 || index < 0 || index >= 2 || d->queue[index].ready == false) {
        return -1;
    }
    queue = &d->queue[index];
    return (int)queue->used->index;
}

void virtio_notify(struct virtio_device *d, int queue)
{
    uint16_t offset;

    if (d == 0 || d->ready == false || queue < 0 || queue >= 2) {
        return;
    }
    common_write16(d, COMMON_QUEUE_SELECT, (uint16_t)queue);
    offset = common_read16(d, COMMON_QUEUE_NOTIFY_OFF);
    /* the offset is in the units the device asked for, which is 4 bytes
     * unless the device said otherwise */
    /* the device asked for a unit in notify_offset_multiplier; it is 4 bytes
     * unless it says otherwise, and a multiplier of zero would be a device
     * making no sense, so treat it as 4 rather than dividing by it */
    if (d->notify_offset_multiplier == 0U) {
        d->notify_offset_multiplier = 4U;
    }
    mmio_write16((uint64_t)(uintptr_t)d->notify,
                 (uint16_t)(offset * d->notify_offset_multiplier), 0);
}
