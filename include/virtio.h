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
 * The modern PCI interface is used, not the legacy one.  A modern device
 * publishes a structure in memory rather than a set of registers, which is
 * both less work and the one that does not have a byte-ordering story.
 *
 * The rings are physically contiguous, so they come from the heap and their
 * addresses are used directly.  That is only true because boot code identity
 * maps the low memory, which is noted in the source rather than assumed
 * quietly: a driver that needs arbitrary buffers later will have to scatter
 * them, and the queue layout here is what that has to fit into.
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
    bool ready;
};

struct virtio_device {
    const struct pci_device *pci;
    volatile uint8_t *common;    /* the common configuration block */
    volatile uint8_t *notify;    /* the notification area */
    uint32_t notify_offset_multiplier;
    uint64_t device_features;   /* 64 features, in two 32 bit words */
    uint64_t driver_features;
    uint16_t queue_count;
    struct virtio_queue queue[2];
    bool ready;
    bool modern;
    uint16_t device_id;
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

/* Progress reported by the device, for the shell. */
int virtio_queue_free(const struct virtio_device *device, int index);
int virtio_queue_used(const struct virtio_device *device, int index);

/* Tell the device a queue has work in it. */
void virtio_notify(struct virtio_device *device, int queue);

/* Reason for the most recent failure, or "" if none. */
const char *virtio_error(void);

#endif
