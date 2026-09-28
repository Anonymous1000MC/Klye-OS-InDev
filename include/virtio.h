#ifndef KLYE_VIRTIO_H
#define KLYE_VIRTIO_H

#include <stdbool.h>
#include <stdint.h>

/* The virtio transport, over PCI.
 *
 * virtio is how a guest talks to a device the host emulates without either
 * side knowing much about the other.  The device model publishes a feature
 * bitmap, the driver picks the bits it understands, and then both sides agree
 * on a pair of rings of descriptors that name buffers.  Neither the kernel
 * nor QEMU has to know what a disk is.
 *
 * This is the transport only, and only the part of it that can be checked
 * without moving any data: find the device, read the common configuration
 * block, negotiate features, and lay out one queue.  Driving a request
 * through the queue is the next step, and needs a driver on top.
 *
 * The legacy transport is used, through the device's port register.  A
 * transitional device, which is what QEMU presents by default as device id
 * 0x1001, has both, and the legacy one is a handful of registers at fixed
 * offsets: four words of features, four words of ring addresses, a queue size
 * and a status byte.  The modern interface puts a structure in memory instead,
 * which is the more modern arrangement and was tried first; on this device its
 * queue size read back byte swapped, and a transport that cannot be trusted to
 * read one field is not worth using for the sake of the others.
 *
 * The rings are physically contiguous and page aligned, because the legacy
 * interface is told where the queue is as a single page frame number and
 * works out the rest itself.  They come from the heap, over-allocated so the
 * address can be rounded up to a page.  That only works because boot code
 * identity maps the low memory, which is noted in the source rather than
 * assumed quietly: a driver that needs an arbitrary buffer later will have to
 * scatter it, and this layout is what has to accommodate that.
 */

#define VIRTIO_VENDOR 0x1AF4U
#define VIRTIO_PCI_MODERN 0x1000U /* 0x1040 + device id for a modern device */

#define VIRTIO_STATUS_ACK 0x01U
#define VIRTIO_STATUS_DRIVER 0x02U
#define VIRTIO_STATUS_DRIVER_OK 0x04U
#define VIRTIO_STATUS_FEATURES_OK 0x08U

#define VIRTIO_QUEUE_DESC 0
#define VIRTIO_QUEUE_AVAIL 1
#define VIRTIO_QUEUE_USED 2

/* One 16 byte descriptor, as the split virtqueue layout defines it. */
struct virtio_desc {
    uint64_t address;
    uint32_t length;
    uint16_t flags;
    uint16_t next;
};

#define VIRTIO_DESC_NEXT 1U
#define VIRTIO_DESC_WRITE 2U

/* The ring the device writes back into. */
struct virtio_used_elem {
    uint32_t id;
    uint32_t length;
};

/* The two halves of a split virtqueue.  Both start with a flags word the
 * device uses to suppress interrupts, then an index, then the entries.  The
 * available ring's index is written by the driver and the used ring's by the
 * device, and each is only valid once the other's has been read: the pair is
 * how a queue stays consistent without a lock, and reading them in the wrong
 * order is how a ring loses an entry. */
struct virtio_avail {
    uint16_t flags;
    uint16_t index;
    uint16_t ring[];
};

struct virtio_used {
    uint16_t flags;
    uint16_t index;
    struct virtio_used_elem ring[];
};

struct virtio_queue {
    uint16_t size;
    uint16_t select;
    struct virtio_desc *desc;
    struct virtio_avail *avail;
    struct virtio_used *used;
    uint32_t next_free;
    uint16_t last_used;
    uint32_t pfn;
    uint64_t physical;   /* where it is in the machine, not in the window */
    bool ready;
};

/* The block request a virtio disk expects in front of every transfer.  The
 * first field is read by the device and read by the driver, so it cannot be
 * reordered around the rest of the structure. */
struct virtio_blk_request {
    volatile uint32_t type;   /* 0 in, 1 out */
    uint32_t reserved;
    uint64_t sector;
};

#define VIRTIO_BLK_IN 0U
#define VIRTIO_BLK_SECTOR_BYTES 512U

struct virtio_device {
    const struct pci_device *pci;
    uint16_t io;                  /* the legacy port register base */
    struct virtio_blk_request request;
    uint64_t ring_virtual;   /* the window allocation, as the CPU sees it */
    uint64_t physical;       /* and where that is in the machine */
    uint32_t request_offset; /* the request header's offset into it */
    uint32_t data_offset;    /* and the data buffer's */  /* the header in front of a transfer */
    uint32_t ring_bytes;          /* the contiguous ring allocation */
    void *ring;
    uint64_t device_features;   /* 64 features, in two 32 bit words */
    uint64_t driver_features;
    uint16_t queue_count;
    struct virtio_queue queue[2];
    bool ready;
    bool modern;
    uint16_t device_id;
    uint16_t num_queues_reported;   /* as the device says, even after a failure */
    uint16_t select_readback;
    uint32_t pfn;            /* the frame number handed to the device */
    uint8_t last_isr;
    uint16_t last_queue_size;
    uint8_t last_status;        /* after a failed request, for working out why */
    uint16_t last_used_index;
    uint16_t last_avail_index;
};

/* Find a virtio device by its PCI device id, or 0.  A modern device is
 * 0x1040 plus the legacy id, so both spellings are tried. */
struct virtio_device *virtio_find(uint16_t device_id);

/* Set up one device: read its configuration, negotiate the features this
 * driver understands, and lay out its queues.  Sets the device status to
 * DRIVER_OK on success. */
bool virtio_setup(struct virtio_device *device);

/* The device status byte, which is the transport's own report of how far it
 * has got: acknowledge, driver, features ok, driver ok. */
uint8_t virtio_status(const struct virtio_device *device);

/* A 32 bit read of the common configuration block, for working out what a
 * device is actually answering. */
uint32_t virtio_common_read(const struct virtio_device *device, uint16_t offset);

/* Read one sector through the queue and leave it in `buffer`.
 *
 * Two descriptors, chained: the request header first, then the data.  The
 * device is told the queue has work by a write to its notify register, and
 * then everything depends on polling the used ring, because there is no
 * interrupt wired up yet.  Returns 0 on success, or a negative number. */
int virtio_blk_read(struct virtio_device *device, uint64_t sector,
                    uint8_t *buffer);

/* Progress reported by the device, for the shell. */
int virtio_queue_free(const struct virtio_device *device, int index);
int virtio_queue_used(const struct virtio_device *device, int index);

/* Tell the device a queue has work in it. */
void virtio_notify(struct virtio_device *device, int queue);

/* Legacy register offsets, in port space. */
#define VIRTIO_LEGACY_FEATURES 0x00
#define VIRTIO_LEGACY_GUEST_FEATURES 0x04
#define VIRTIO_LEGACY_QUEUE_PFN 0x08
#define VIRTIO_LEGACY_QUEUE_SIZE 0x0C
#define VIRTIO_LEGACY_QUEUE_SELECT 0x0E
#define VIRTIO_LEGACY_QUEUE_NOTIFY 0x10
#define VIRTIO_LEGACY_STATUS 0x12
#define VIRTIO_LEGACY_ISR 0x13

/* Reason for the most recent failure, or "" if none. */
const char *virtio_error(void);

#endif
