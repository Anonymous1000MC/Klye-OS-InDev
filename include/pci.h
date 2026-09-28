#ifndef KLYE_PCI_H
#define KLYE_PCI_H

#include <stdbool.h>
#include <stdint.h>

/* PCI bus enumeration.
 *
 * There is no PCI support in this kernel at all yet: the framebuffer comes
 * from Multiboot and the disk is legacy ATA PIO at 0x1F0.  This is the first
 * layer of getting that wrong fixed, and it is deliberately only this layer.
 *
 * Enumeration finds devices and reads their identity.  It does not map a base
 * address register, does not enable memory or IO decoding, and has no driver
 * for anything.  That is on purpose: enumeration is the only part of this that
 * can be finished and checked on its own, because it needs no mapping to be
 * correct, and a driver written before the BARs are mapped would be testing
 * nothing.
 *
 * Config space is reached through the legacy port pair, 0xCF8 to choose an
 * offset and 0xCFC to read or write 32 bits of it.  Every modern machine has
 * it, including the emulated one this runs under, so there is no reason to
 * write a memory mapped path until something actually needs one.
 */

#define PCI_MAX_DEVICES 64
#define PCI_MAX_BUSES 16
#define PCI_CONFIG_ADDRESS 0xCF8U
#define PCI_CONFIG_DATA 0xCFCU

/* A base address register, as read.  Not sized, not mapped: `address` is the
 * value the device offered and `is_io` says which kind of space it is. */
struct pci_bar {
    uint64_t address;
    uint32_t size;   /* 0 when unimplemented, or before pci_map_all() */
    uint64_t virtual_address; /* where it is mapped, 0 until it is */
    bool is_io;
    bool is_64bit;
    bool mapped;
    bool unsized;  /* the device did not report a size for this register */
    bool present;  /* the device implements this register at all */
};

struct pci_device {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t vendor;
    uint16_t device_id;
    uint8_t class_code;   /* byte at 0x0b */
    uint8_t subclass;     /* byte at 0x0a */
    uint8_t prog_if;      /* byte at 0x09 */
    uint8_t header_type;  /* 0 for a plain device, 1 for a bridge */
    uint8_t bar_count;
    bool multifunction;
    bool mapped;
    bool unsized;  /* the device did not report a size for this register */
    bool present;  /* the device implements this register at all */   /* pci_map_all() gave it at least one mapping */
    bool has_memory_bar;
    bool decoding_failed; /* the command register would not keep the enable bits */
    struct pci_bar bar[6];
};

/* Walk the bus and everything behind a bridge, filling in the device list.
 * Safe to call more than once; the list is rebuilt.  Returns how many devices
 * were found, which is 0 on a machine with no PCI bus at all rather than a
 * failure, because that is a legitimate answer. */
int pci_enumerate(void);

/* Devices found by the last pci_enumerate(). */
int pci_device_count(void);
const struct pci_device *pci_device_at(int index);

/* Find the first device with this class and subclass, or NULL.  Passing -1 for
 * either matches anything, which is how a driver asks for "any display" or
 * "anything on this bus". */
const struct pci_device *pci_find(int class_code, int subclass);

/* Size every base address register, map the memory ones, and turn on the
 * device's memory or IO decoding.
 *
 * Sizing a register means writing all ones to it and reading back the mask,
 * then putting the original value straight back, because in between the
 * device believes it answers at the top of the address space.  This is the
 * known hazard of the method and is the reason it is a separate step from
 * enumeration: enumeration only reads, and cannot leave a device in a state it
 * was not already in.
 *
 * Returns how many devices ended up with a mapping. */
int pci_map_all(void);

/* Read 8, 16 or 32 bits of a device's config space.  The narrow widths read
 * the enclosing 32 bits and keep the right part, because 0xCFC to 0xCFF are
 * four byte wide ports, not four one byte ports. */
uint32_t pci_read32(const struct pci_device *device, uint8_t offset);
uint16_t pci_read16(const struct pci_device *device, uint8_t offset);
uint8_t pci_read8(const struct pci_device *device, uint8_t offset);

/* A readable name for a class, subclass pair, for printing. */
const char *pci_class_name(uint8_t class_code, uint8_t subclass);

/* A vendor name, or NULL when it is not one of the handful worth naming.
 * Printing the raw id is fine; a partial table that implies completeness is
 * not. */
const char *pci_vendor_name(uint16_t vendor);

/* Reason the last call failed, or "" if none. */
const char *pci_error(void);

#endif
