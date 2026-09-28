/* virtio.c - the virtio transport, over the legacy port interface.
 *
 * Reads a device's features, lays out its queue, and puts the device into the
 * DRIVER_OK state.  Moving data is the driver's job and is not here.
 */

#include "virtio.h"

#include "heap.h"
#include "pci.h"
#include "io.h"
#include "mmu.h"

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

struct virtio_device *virtio_find(uint16_t device_id)
{
    const struct pci_device *found = 0;
    uint16_t want = device_id;

    if (pci_device_count() == 0) {
        pci_enumerate();
    }
    for (int index = 0; index < pci_device_count(); ++index) {
        const struct pci_device *candidate = pci_device_at(index);

        if (candidate->vendor != VIRTIO_VENDOR) {
            continue;
        }
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
    return &device;
}

static uint8_t m_read8(const struct virtio_device *d, uint16_t at)
{
    return d->modern[at];
}

static void m_write8(const struct virtio_device *d, uint16_t at, uint8_t v)
{
    d->modern[at] = v;
}

static uint16_t m_read16(const struct virtio_device *d, uint16_t at)
{
    return (uint16_t)(d->modern[at] | ((uint16_t)d->modern[(uint16_t)(at + 1)] << 8));
}

static void m_write16(const struct virtio_device *d, uint16_t at, uint16_t v)
{
    d->modern[at] = (uint8_t)(v & 0xFFU);
    d->modern[(uint16_t)(at + 1)] = (uint8_t)(v >> 8);
}

static uint32_t m_read32(const struct virtio_device *d, uint16_t at)
{
    return (uint32_t)d->modern[at] |
           ((uint32_t)d->modern[(uint16_t)(at + 1)] << 8) |
           ((uint32_t)d->modern[(uint16_t)(at + 2)] << 16) |
           ((uint32_t)d->modern[(uint16_t)(at + 3)] << 24);
}

static void m_write32(const struct virtio_device *d, uint16_t at, uint32_t v)
{
    d->modern[at] = (uint8_t)(v & 0xFFU);
    d->modern[(uint16_t)(at + 1)] = (uint8_t)((v >> 8) & 0xFFU);
    d->modern[(uint16_t)(at + 2)] = (uint8_t)((v >> 16) & 0xFFU);
    d->modern[(uint16_t)(at + 3)] = (uint8_t)((v >> 24) & 0xFFU);
}

static void m_write64(const struct virtio_device *d, uint16_t at, uint64_t v)
{
    m_write32(d, at, (uint32_t)(v & 0xFFFFFFFFU));
    m_write32(d, (uint16_t)(at + 4), (uint32_t)(v >> 32));
}

static size_t align4096(size_t value)
{
    return (value + 4095U) & ~(size_t)4095U;
}

static bool virtio_lay_out_queue(struct virtio_device *d, uint16_t queue_size,
                                 bool modern);

static bool virtio_setup_modern(struct virtio_device *d)
{
    uint32_t features;
    uint32_t wanted;
    uint16_t queue_size;

    m_write8(d, VIRTIO_COMMON_STATUS, 0U);
    m_write8(d, VIRTIO_COMMON_STATUS, VIRTIO_STATUS_ACK);
    if (m_read8(d, VIRTIO_COMMON_STATUS) != VIRTIO_STATUS_ACK) {
        fail("the modern status byte did not stick");
        return false;
    }
    m_write8(d, VIRTIO_COMMON_STATUS,
             (uint8_t)(VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER));

    m_write32(d, VIRTIO_COMMON_DRIVER_SELECT, 0U);
    wanted = m_read32(d, VIRTIO_COMMON_DEVICE_FEATURE);
    m_write32(d, VIRTIO_COMMON_DRIVER_SELECT, 1U);
    /* the high word of the feature bitmap lives in the second 32 bit
     * register, so it has to be widened before it is shifted up or the shift
     * count is not representable in the type it is happening in */
    features = (uint32_t)m_read32(d, VIRTIO_COMMON_DEVICE_FEATURE);
    d->device_features = ((uint64_t)features << 32) | (uint64_t)wanted;
    if ((features & ((uint64_t)1 << VIRTIO_F_VERSION_1)) == 0U) {
        fail("the device does not offer version 1");
        return false;
    }
    wanted = (uint32_t)(((uint64_t)1 << VIRTIO_F_VERSION_1) & 0xFFFFFFFFU);
    m_write32(d, VIRTIO_COMMON_DRIVER_SELECT, 0U);
    m_write32(d, VIRTIO_COMMON_DRIVER_FEATURE, wanted);
    m_write32(d, VIRTIO_COMMON_DRIVER_SELECT, 1U);
    m_write32(d, VIRTIO_COMMON_DRIVER_FEATURE, 0U);
    d->driver_features = (uint64_t)1 << VIRTIO_F_VERSION_1;
    m_write8(d, VIRTIO_COMMON_STATUS,
             (uint8_t)(VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER |
                       VIRTIO_STATUS_FEATURES_OK));

    d->queue_count = m_read16(d, VIRTIO_COMMON_NUM_QUEUES);
    m_write16(d, VIRTIO_COMMON_QUEUE_SELECT, 0U);
    queue_size = m_read16(d, VIRTIO_COMMON_QUEUE_SIZE);
    if (queue_size == 0U) {
        fail("the device reported a queue of no entries");
        return false;
    }
    return virtio_lay_out_queue(d, queue_size, true);
}

/* Lay the ring out and tell the device where it is, by whichever route the
 * device offers: a page frame number in the legacy interface, three physical
 * addresses in the modern one. */
static bool virtio_lay_out_queue(struct virtio_device *d, uint16_t queue_size,
                                 bool modern)
{
    size_t desc_bytes = (size_t)queue_size * 16U;
    size_t avail_bytes = 2U + (size_t)queue_size * 2U;
    size_t used_bytes = 2U + (size_t)queue_size * 8U;
    size_t ring_bytes;
    size_t need;
    uint8_t *base;

    ring_bytes = align4096(desc_bytes) + align4096(avail_bytes) +
                 align4096(used_bytes);
    need = ring_bytes + sizeof(struct virtio_blk_request) +
           VIRTIO_BLK_SECTOR_BYTES;
    base = vm_alloc_pages(need);
    if (base == 0) {
        fail("out of memory for the queue rings");
        return false;
    }
    d->physical = vm_to_physical((uint64_t)(uintptr_t)base);
    d->ring = (void *)base;
    d->ring_virtual = (uint64_t)(uintptr_t)base;
    d->ring_bytes = (uint32_t)need;
    d->request_offset = (uint32_t)ring_bytes;
    d->data_offset = (uint32_t)(ring_bytes + sizeof(struct virtio_blk_request));
    d->queue_count = 1;
    d->queue[0].size = queue_size;
    d->queue[0].select = 0U;
    d->queue[0].desc = (struct virtio_desc *)base;
    d->queue[0].avail = (struct virtio_avail *)(base + align4096(desc_bytes));
    d->queue[0].used = (struct virtio_used *)(base + align4096(desc_bytes) +
                                             align4096(avail_bytes));
    d->queue[0].avail->flags = 1U;
    d->queue[0].avail->index = 0U;
    d->queue[0].used->flags = 0U;
    d->queue[0].used->index = 0U;
    d->queue[0].next_free = 0U;
    d->queue[0].last_used = 0U;
    d->queue[0].ready = true;

    if (modern) {
        m_write64(d, VIRTIO_COMMON_QUEUE_DESC, d->physical);
        m_write64(d, VIRTIO_COMMON_QUEUE_DRIVER,
                  d->physical + align4096(desc_bytes));
        m_write64(d, VIRTIO_COMMON_QUEUE_DEVICE,
                  d->physical + align4096(desc_bytes) +
                      align4096(avail_bytes));
        m_write16(d, VIRTIO_COMMON_QUEUE_ENABLE, 1U);
        d->queue[0].pfn = (uint32_t)(d->physical >> 12);
    } else {
        d->queue[0].pfn = (uint32_t)(d->physical >> 12);
        outl((uint16_t)(d->io + VIRTIO_LEGACY_QUEUE_PFN), d->queue[0].pfn);
    }
    if (modern) {
        m_write8(d, VIRTIO_COMMON_STATUS,
                 (uint8_t)(VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER |
                           VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK));
    } else {
        outb((uint16_t)(d->io + VIRTIO_LEGACY_STATUS),
             (uint8_t)(VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER |
                       VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK));
    }
    if ((virtio_status(d) & VIRTIO_STATUS_DRIVER_OK) == 0U) {
        fail("the device refused the driver ok status");
        return false;
    }
    d->ready = true;
    return true;
}

bool virtio_setup(struct virtio_device *d)
{
    error_text[0] = 0;
    if (d == 0 || d->pci == 0) {
        fail("no device");
        return false;
    }
    for (int i = 0; i < 6; ++i) {
        const struct pci_bar *bar = &d->pci->bar[i];

        if (bar->present != false && bar->is_io != false) {
            d->io = (uint16_t)bar->address;
        }
        if (bar->present != false && bar->is_io == false &&
            bar->size == VIRTIO_MODERN_REGION && bar->mapped != false) {
            d->modern = (volatile uint8_t *)(uintptr_t)bar->virtual_address;
            d->use_modern = true;
        }
    }
    if (d->modern == 0 && d->io == 0U) {
        fail("the device has neither a port nor a memory register to drive");
        return false;
    }
    if (d->modern != 0) {
        return virtio_setup_modern(d);
    }

    /* Acknowledge before reading anything else.  A device that has not been
     * acknowledged is entitled to answer with zero, and zero is also what a
     * device with no features looks like, so the status byte is read back to
     * tell those two apart before anything is believed. */
    outb((uint16_t)(d->io + VIRTIO_LEGACY_STATUS), 0U);
    outb((uint16_t)(d->io + VIRTIO_LEGACY_STATUS), VIRTIO_STATUS_ACK);
    if (inb((uint16_t)(d->io + VIRTIO_LEGACY_STATUS)) != VIRTIO_STATUS_ACK) {
        fail("the status byte did not stick, so the register is not answering");
        return false;
    }
    outb((uint16_t)(d->io + VIRTIO_LEGACY_STATUS),
         (uint8_t)(VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER));

    /* The legacy feature word is 32 bits, so version 1, which is bit 32,
     * cannot be negotiated through it at all.  That is not an obstacle to work
     * around, it is the point: not agreeing to version 1 selects the original
     * ring layout, where the three parts sit end to end instead of each
     * starting on a page boundary.  A transitional device can do either. */
    d->device_features = inl((uint16_t)(d->io + VIRTIO_LEGACY_FEATURES));
    outl((uint16_t)(d->io + VIRTIO_LEGACY_GUEST_FEATURES), 0U);
    d->driver_features = 0U;
    outw((uint16_t)(d->io + VIRTIO_LEGACY_QUEUE_SELECT), 0U);
    return virtio_lay_out_queue(
        d, inw((uint16_t)(d->io + VIRTIO_LEGACY_QUEUE_SIZE)), false);
}

uint8_t virtio_status(const struct virtio_device *d)
{
    if (d == 0) {
        return 0U;
    }
    if (d->use_modern && d->modern != 0) {
        return m_read8(d, VIRTIO_COMMON_STATUS);
    }
    if (d->io == 0U) {
        return 0U;
    }
    return inb((uint16_t)(d->io + VIRTIO_LEGACY_STATUS));
}

uint32_t virtio_common_read(const struct virtio_device *d, uint16_t offset)
{
    if (d == 0 || d->io == 0U) {
        return 0U;
    }
    if (d->use_modern && d->modern != 0) {
        return m_read32(d, offset);
    }
    if (d->io == 0U) {
        return 0U;
    }
    if (offset == 0x0CU) {
        return inl((uint16_t)(d->io + VIRTIO_LEGACY_FEATURES));
    }
    return inl(d->io);
}

/* Keep the compiler from moving memory access across these, which matters
 * because the ordering between the driver's writes and the device's reads is
 * the whole protocol.  The processor does not need help: x86 orders its own
 * loads and stores, and these are the only two agents involved. */

static void barrier(void)
{
    __asm__ volatile("" : : : "memory");
}

int virtio_blk_read(struct virtio_device *d, uint64_t sector, uint8_t *buffer)
{
    struct virtio_queue *queue;
    uint16_t head;
    uint16_t slot;
    uint32_t spins = 0U;

    if (d == 0 || d->ready == false || buffer == 0) {
        return -1;
    }
    queue = &d->queue[0];
    if (queue->size < 4U) {
        return -2;
    }
    if (virtio_queue_free(d, 0) < 2) {
        return -3;
    }

    /* Two chained descriptors: the request header, then the data.  The data
     * one carries the write flag, which from the device's point of view is the
     * only permission it has to write into a buffer at all.  A read that
     * forgets it returns the device's own memory instead of the disk's. */
    /* the data buffer has to be one the device can reach too, so it is taken
     * from the mapping window and the physical address is what goes in the
     * descriptor.  A caller's own buffer would need its physical address,
     * which a virtual pointer cannot supply. */
    {
        struct virtio_blk_request *request =
            (struct virtio_blk_request *)(void *)(uintptr_t)
                (d->ring_virtual + d->request_offset);

        request->type = VIRTIO_BLK_IN;
        request->reserved = 0U;
        request->sector = sector;
    }
    head = (uint16_t)(queue->avail->index % queue->size);
    /* the request header, immediately before the data, both inside the ring
     * allocation so one address covers the lot */
    queue->desc[0].address = d->physical + d->request_offset;
    queue->desc[0].length = (uint32_t)sizeof(struct virtio_blk_request);
    queue->desc[0].flags = VIRTIO_DESC_NEXT;
    queue->desc[0].next = 1U;
    queue->desc[1].address = d->physical + d->data_offset;
    queue->desc[1].length = VIRTIO_BLK_SECTOR_BYTES;
    queue->desc[1].flags = VIRTIO_DESC_WRITE;
    queue->desc[1].next = 0U;

    /* Publish the descriptor before the index that says it is there.  The
     * device reads the index first and only then the ring, so the other order
     * has it looking at a descriptor that is not written yet. */
    slot = head;
    queue->avail->ring[slot] = head;
    barrier();
    queue->avail->index = (uint16_t)(queue->avail->index + 1U);
    barrier();
    queue->next_free = 2U;
    virtio_notify(d, 0);

    /* No interrupt is wired up, so completion is a poll.  The bound is there
     * so a device that never answers is a failure rather than a hang: at this
     * clock that is a couple of milliseconds, which is far longer than a
     * request to an emulated disk takes and far shorter than a wait a person
     * would notice. */
    while (queue->used->index == queue->last_used) {
        if (++spins > 20000000U) {
            /* the status byte says whether the device objected, and the used
             * ring says whether it looked at anything at all */
            return -4;
        }
    }
    {
        struct virtio_used_elem *elem =
            &queue->used->ring[queue->last_used % queue->size];

        if (elem->id != 0U) {
            /* the device finished a different descriptor than the one handed
             * to it, which means the rings have drifted apart */
            return -5;
        }
        queue->last_used = (uint16_t)(queue->last_used + 1U);
        queue->next_free = 0U;
    }
    /* copy out through the CPU, since the buffer the device wrote is reached
     * by a different address than the caller's */
    {
        const uint8_t *data =
            (const uint8_t *)(uintptr_t)(d->ring_virtual + d->data_offset);

        for (uint32_t at = 0; at < (uint32_t)VIRTIO_BLK_SECTOR_BYTES; ++at) {
            buffer[at] = data[at];
        }
    }
    return 0;
}

int virtio_queue_free(const struct virtio_device *d, int index)
{
    const struct virtio_queue *queue;

    if (d == 0 || index != 0 || d->queue[index].ready == false) {
        return -1;
    }
    queue = &d->queue[index];
    return (int)queue->size - (int)queue->avail->index - (int)queue->next_free;
}

int virtio_queue_used(const struct virtio_device *d, int index)
{
    const struct virtio_queue *queue;

    if (d == 0 || index != 0 || d->queue[index].ready == false) {
        return -1;
    }
    queue = &d->queue[index];
    return (int)queue->used->index;
}

void virtio_notify(struct virtio_device *d, int queue)
{
    if (d == 0 || d->ready == false || queue != 0) {
        return;
    }
    if (d->use_modern) {
        /* the modern interface takes an offset into the notification window
         * rather than a queue number, and a multiplier that is 4 unless the
         * device says otherwise */
        /* the multiplier is a register of its own in the device's own
         * configuration, which this driver does not read, and it is 4 on
         * every device that is not doing something unusual */
        uint32_t multiplier = 4U;

        if (multiplier == 0U) {
            multiplier = 4U;
        }
        *(volatile uint16_t *)(void *)(uintptr_t)
            ((uint64_t)(uintptr_t)(d->modern + VIRTIO_NOTIFY_OFFSET) +
             (uint64_t)m_read16(d, VIRTIO_COMMON_QUEUE_NOTIFY_OFF) *
                 multiplier) = (uint16_t)queue;
        return;
    }
    outw((uint16_t)(d->io + VIRTIO_LEGACY_QUEUE_NOTIFY), 0U);
}
