/* pci.c - walking the PCI bus and reading device identity.
 *
 * Nothing here maps anything.  See the header for why that is deliberate.
 */

#include "pci.h"

#include "io.h"
#include "kernel.h"
#include "mmu.h"

static struct pci_device devices[PCI_MAX_DEVICES];
static int device_count;
static char error_text[48] = "";

static void fail(const char *why)
{
    __builtin_strncpy(error_text, why, sizeof(error_text) - 1);
    error_text[sizeof(error_text) - 1] = 0;
}

const char *pci_error(void)
{
    return error_text;
}

/* Config space is selected by writing a composed address to 0xCF8 and read or
 * written 32 bits at a time through 0xCFC.  Bit 31 enables the cycle, and the
 * register is read and write in 4 byte steps, so the low two bits of the
 * offset are dropped. */
static uint32_t config_read(uint8_t bus, uint8_t device, uint8_t function,
                            uint8_t offset)
{
    uint32_t address = 0x80000000U | ((uint32_t)bus << 16) |
                       ((uint32_t)device << 11) | ((uint32_t)function << 8) |
                       ((uint32_t)offset & 0xFCU);

    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

/* Writing config space, which the sizing pass needs.  Enumeration alone never
 * writes; sizing a BAR does, and only by accident, twice. */
static void config_write(uint8_t bus, uint8_t device, uint8_t function,
                         uint8_t offset, uint32_t value)
{
    uint32_t address = 0x80000000U | ((uint32_t)bus << 16) |
                       ((uint32_t)device << 11) | ((uint32_t)function << 8) |
                       ((uint32_t)offset & 0xFCU);

    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
}

/* Write 16 bits of config space.  A base address register is 32 bits wide but
 * the command register beside it is 16, and a plain 32 bit write at the same
 * offset puts zeroes over the status register that follows.  Reading, masking
 * and writing back is the only way to touch half a register with a port this
 * wide. */
static void config_write16(uint8_t bus, uint8_t device, uint8_t function,
                            uint8_t offset, uint16_t value)
{
    uint32_t shift = (uint32_t)(offset & 2U) * 8U;
    uint32_t old = config_read(bus, device, function, offset);

    config_write(bus, device, function, offset,
                 (old & ~(0xFFFFU << shift)) | ((uint32_t)value << shift));
}

uint32_t pci_read32(const struct pci_device *device, uint8_t offset)
{
    if (device == 0) {
        return 0xFFFFFFFFU;
    }
    return config_read(device->bus, device->device, device->function, offset);
}

uint16_t pci_read16(const struct pci_device *device, uint8_t offset)
{
    uint32_t value = pci_read32(device, offset);

    return (uint16_t)((value >> ((offset & 2U) * 8U)) & 0xFFFFU);
}

uint8_t pci_read8(const struct pci_device *device, uint8_t offset)
{
    uint32_t value = pci_read32(device, offset);

    return (uint8_t)((value >> ((offset & 3U) * 8U)) & 0xFFU);
}

const char *pci_class_name(uint8_t class_code, uint8_t subclass)
{
    if (class_code == 0x00 && subclass == 0x01) {
        return "VGA compatible";
    }
    if (class_code == 0x01) {
        return "mass storage";
    }
    if (class_code == 0x02) {
        return "network";
    }
    if (class_code == 0x03) {
        return "display";
    }
    if (class_code == 0x04) {
        return "multimedia";
    }
    if (class_code == 0x06) {
        return "bridge";
    }
    if (class_code == 0x07) {
        return "communication";
    }
    if (class_code == 0x08) {
        return "base system";
    }
    if (class_code == 0x09) {
        return "input";
    }
    if (class_code == 0x0C) {
        return "serial bus";
    }
    if (class_code == 0x0D) {
        return "wireless";
    }
    return "unknown";
}

const char *pci_vendor_name(uint16_t vendor)
{
    switch (vendor) {
    case 0x1002: return "AMD";
    case 0x10DE: return "NVIDIA";
    case 0x1179: return "Toshiba";
    case 0x1234: return "QEMU";
    case 0x1AF4: return "Red Hat (virtio)";
    case 0x1B36: return "Red Hat";
    case 0x2668: return "IBM";
    case 0x8086: return "Intel";
    case 0x80EE: return "VirtualBox";
    case 0xABCD: return "Klye";
    default: return 0;
    }
}

/* Read the base address registers into the device record.
 *
 * A BAR's low bit says whether it is port IO rather than memory, and for a
 * 64 bit memory BAR the next one is its upper half.  The address is left
 * exactly as the device set it, because sizing a BAR means writing all ones
 * and reading back the mask, which temporarily makes the device claim an
 * address range it does not have.  That belongs in the mapping layer, where
 * being wrong about it can point a mapping at the wrong thing. */
static void read_bars(struct pci_device *out)
{
    uint8_t index;

    out->bar_count = 0;
    for (index = 0; index < 6; ++index) {
        uint8_t offset = (uint8_t)(0x10U + index * 4U);
        uint32_t raw = config_read(out->bus, out->device, out->function, offset);
        struct pci_bar *bar = &out->bar[index];
        bool is_io = (raw & 1U) != 0U;
        bool low = (raw & 2U) != 0U; /* for memory, 32 or 64 bit addressing */

        bar->address = raw & 0xFFFFFFF0U;
        bar->is_io = is_io;
        bar->is_64bit = (is_io == false) && (low != 0U);
        bar->size = 0;
        bar->present = false;
        bar->unsized = false;
        bar->mapped = false;
        bar->virtual_address = 0U;
        if (raw == 0U) {
            continue; /* an unimplemented BAR, which is not a reserved one */
        }
        /* Presence is tracked per register rather than as a count, because a
         * count is not an index.  A device with bar0 and bar2 implemented and
         * bar1 not has two bars, and using that two as a loop bound would size
         * bar0 and bar1 and never touch bar2. */
        bar->present = true;
        out->bar_count++;
    }
}

static bool same_slot(uint8_t bus, uint8_t device, uint8_t function)
{
    int i;

    for (i = 0; i < device_count; ++i) {
        if (devices[i].bus == bus && devices[i].device == device &&
            devices[i].function == function) {
            return true;
        }
    }
    return false;
}

/* One byte of a device's config space.
 *
 * Config space is a byte array, and 0xCFC to 0xCFF are one 32 bit port, not
 * four 8 bit ports, so every narrow field has to be read as a dword and
 * picked out of it.  Getting the shift wrong is quiet: the values that come
 * back are real bytes from the same device, just the wrong ones, so a class
 * code read from where the revision ID lives still looks like a class code
 * and the table fills with plausible nonsense. */
static uint8_t config_byte(uint8_t bus, uint8_t device, uint8_t function,
                           uint8_t offset)
{
    uint32_t value = config_read(bus, device, function, offset);

    return (uint8_t)((value >> ((offset & 3U) * 8U)) & 0xFFU);
}

/* Record one device, or return false when the list is full. */
static bool record(uint8_t bus, uint8_t device, uint8_t function,
                   uint8_t *buses, int *bus_count)
{
    struct pci_device *out;
    uint32_t identity = config_read(bus, device, function, 0x00);
    uint16_t vendor = (uint16_t)(identity & 0xFFFFU);
    uint8_t header = config_byte(bus, device, function, 0x0D);
    int index;

    if (vendor == 0xFFFFU) {
        return true; /* no device in this slot, which is not an error */
    }
    if (same_slot(bus, device, function)) {
        return true;
    }
    if (device_count >= PCI_MAX_DEVICES) {
        return false;
    }
    index = device_count++;
    out = &devices[index];
    out->bus = bus;
    out->device = device;
    out->function = function;
    out->vendor = vendor;
    out->device_id = (uint16_t)((identity >> 16) & 0xFFFFU);
    /* 0x09 prog if, 0x0a subclass, 0x0b class, 0x08 revision */
    out->prog_if = config_byte(bus, device, function, 0x09);
    out->subclass = config_byte(bus, device, function, 0x0A);
    out->class_code = config_byte(bus, device, function, 0x0B);
    out->header_type = (uint8_t)(header & 0x7FU);
    out->multifunction =
        (config_byte(bus, device, function, 0x0E) & 0x80U) != 0U;
    read_bars(out);

    /* A bridge's secondary bus is where the rest of the tree hangs, so a scan
     * of bus 0 alone would find the first bridge and nothing behind it. */
    if (out->header_type == 0x01U && *bus_count < PCI_MAX_BUSES) {
        uint8_t secondary = config_byte(bus, device, function, 0x19);

        if (secondary != 0U && secondary != bus) {
            for (int i = 0; i < *bus_count; ++i) {
                if (buses[i] == secondary) {
                    return true; /* already queued */
                }
            }
            buses[*bus_count] = secondary;
            (*bus_count)++;
        }
    }
    return true;
}

/* Work out how large each base address register is.
 *
 * The method is to write all ones into the register and read back what sticks.
 * The bits that stay are the size mask, and the size is that mask plus one:
 * a 4 KiB region leaves bits 0 through 11 set, which is 0xFFF, and 0xFFF + 1
 * is 4 KiB.  The original value is written back immediately, because between
 * the two writes the device believes it owns an address range at the top of
 * the address space and will answer to anyone who asks.
 *
 * The two reads cannot be avoided, and the window is left as short as it can
 * be.  A device that decodes its BAR during the write can be talked to by
 * software in that gap, which is the known hazard of this method; on this
 * machine nothing does, and closing the window properly needs a device model
 * cooperation that does not exist. */
static void size_bars(struct pci_device *device)
{
    for (uint8_t index = 0; index < 6; ++index) {
        if (device->bar[index].present == false) {
            continue;
        }
        uint8_t offset = (uint8_t)(0x10U + index * 4U);
        uint32_t original = config_read(device->bus, device->device,
                                        device->function, offset);
        uint32_t mask;
        bool is_io = (original & 1U) != 0U;

        /* An unimplemented BAR reads as zero and must not be written: all
         * ones to a register the device does not implement is not a mask, it
         * is whatever the device decides to latch. */
        if (original == 0U) {
            device->bar[index].size = 0;
            continue;
        }
        config_write(device->bus, device->device, device->function, offset,
                     0xFFFFFFFFU);
        mask = config_read(device->bus, device->device, device->function,
                           offset);
        config_write(device->bus, device->device, device->function, offset,
                     original);
        /* Bits 0 and 1 are read only flags, not address.  They read as set,
         * so leaving them in would put them in the size. */
        mask &= is_io ? 0xFFFFFFFCU : 0xFFFFFFF0U;
        if (mask == (is_io ? 0xFFFFFFFCU : 0xFFFFFFF0U)) {
            /* Everything came back set, so the device told us nothing. */
            device->bar[index].size = 0;
            device->bar[index].unsized = true;
            continue;
        }
        /* The mask is the set of bits that vary with the size, so the size is
         * what is left of the bits, plus one: a 16 MiB region leaves
         * 0xFF000000 set and leaves 0x00FFFFFF clear, and 0x00FFFFFF + 1 is
         * 16 MiB.  Adding one to the mask instead gives 0xFF000001, which is
         * a plausible looking number that is 4 GiB minus 4 KiB. */
        {
            uint32_t size = (mask == 0U) ? 0U : (~mask + 1U);
            /* A base address register covers a power of two, always.  A
             * result that is not one did not come from a size mask: it is
             * either reserved bits read as set or a device that latched the
             * write instead of masking it.  Either way the number is fiction,
             * and mapping a window for it would be worse than not mapping. */
            if (size != 0U && (size & (size - 1U)) != 0U) {
                device->bar[index].size = 0;
                device->bar[index].unsized = true;
                continue;
            }
            device->bar[index].size = size;
        }
    }
}

/* Map one device's memory registers and turn on its decoding.
 *
 * Nothing is reserved first.  The device owns the range its BAR names, and
 * taking frames for it would not stop the device writing there, so the honest
 * thing is to map exactly what the BAR says and let the driver be careful.
 * Memory decoding is enabled only after the mapping succeeded, so a device is
 * never left decoding a region the CPU has no mapping for. */
static bool map_device(struct pci_device *device)
{
    uint32_t command = pci_read16(device, 0x04);

    /* Turn decoding off before sizing and back on after.  A device that is
     * decoding while its register holds all ones is decoding the top of the
     * address space, which is how an IO register ends up sizing to 4 GiB
     * instead of the few hundred bytes it actually occupies.  It also shrinks
     * the window in which the device answers to a bogus address, which is the
     * hazard of sizing by write at all. */
    config_write16(device->bus, device->device, device->function, 0x04,
                   (uint16_t)(command & ~0x0003U));
    size_bars(device);
    for (uint8_t index = 0; index < 6; ++index) {
        if (device->bar[index].present == false) {
            continue;
        }
        struct pci_bar *bar = &device->bar[index];

        if (bar->is_io) {
            /* port IO needs no mapping, the port number is the address */
            continue;
        }
        if (bar->size == 0U || bar->address == 0U) {
            continue;
        }
        device->has_memory_bar = true;
        bar->virtual_address =
            (uint64_t)(uintptr_t)vm_map_physical(bar->address, bar->size);
        bar->mapped = bar->virtual_address != 0U;
        if (bar->mapped) {
            uint64_t back = vm_to_physical(bar->virtual_address);

            if (back != bar->address) {
                bar->mapped = false;
                fail("the mapping did not come back as the address asked for");
            }
        }
    }
    {
        uint16_t new_command = (uint16_t)(command | 0x0001U);

        if (device->has_memory_bar) {
            new_command |= 0x0002U;
        }
        /* Always write the register, even when the value has not changed from
         * what was read before sizing.  The comparison is against the value
         * saved at the start, but that is not what the device holds now,
         * because the sizing step just cleared the enable bits.  Comparing
         * against it means the re-enable is skipped, and every device is left
         * with decoding off and no memory to answer with, which looks exactly
         * like a device that has none. */
        config_write16(device->bus, device->device, device->function, 0x04,
                       new_command);
        {
            uint16_t back = pci_read16(device, 0x04);

            if ((back & 0x0003U) != (new_command & 0x0003U)) {
                device->decoding_failed = true;
                return false;
            }
        }
        return (new_command & 0x0002U) != 0U;
    }
}

int pci_map_all(void)
{
    int mapped = 0;

    if (device_count == 0) {
        pci_enumerate();
    }
    for (int index = 0; index < device_count; ++index) {
        if (map_device(&devices[index])) {
            devices[index].mapped = true;
            mapped++;
        }
    }
    return mapped;
}

int pci_enumerate(void)
{
    uint8_t buses[PCI_MAX_BUSES];
    int bus_count = 1;
    int pass;

    error_text[0] = 0;
    device_count = 0;
    buses[0] = 0;

    /* Each pass walks every bus queued so far, because a bridge found on a
     * late bus can add another behind it.  Bounded by the queue size, so this
     * terminates even on a machine that reports a bridge pointing at itself
     * through a chain. */
    for (pass = 0; pass < PCI_MAX_BUSES; ++pass) {
        int queued = bus_count;

        for (int b = 0; b < queued; ++b) {
            uint8_t bus = buses[b];

            for (int slot = 0; slot < 32; ++slot) {
                uint8_t device = (uint8_t)slot;

                for (int fn = 0; fn < 8; ++fn) {
                    uint8_t function = (uint8_t)fn;
                    uint32_t identity = config_read(bus, device, function, 0x00);

                    if ((identity & 0xFFFFU) == 0xFFFFU) {
                        /* An absent function 0 means the whole device is
                         * absent, and on a non-multifunction device the other
                         * seven functions cannot exist either. */
                        break;
                    }
                    if (fn > 0 && (config_byte(bus, device, 0, 0x0E) & 0x80U) == 0U) {
                        break; /* not multifunction, so there is no function 1 */
                    }
                    if (record(bus, device, function, buses, &bus_count) == false) {
                        fail("more devices than the list holds");
                        return device_count;
                    }
                }
            }
        }
        if (bus_count == queued) {
            break; /* no new buses, so the tree is fully walked */
        }
    }
    if (device_count == 0) {
        /* Not a failure: a machine with no PCI bus is a real machine, and the
         * caller gets an empty list rather than an error. */
        return 0;
    }
    return device_count;
}

int pci_device_count(void)
{
    return device_count;
}

const struct pci_device *pci_device_at(int index)
{
    if (index < 0 || index >= device_count) {
        return 0;
    }
    return &devices[index];
}

const struct pci_device *pci_find(int class_code, int subclass)
{
    for (int i = 0; i < device_count; ++i) {
        if (class_code >= 0 && devices[i].class_code != (uint8_t)class_code) {
            continue;
        }
        if (subclass >= 0 && devices[i].subclass != (uint8_t)subclass) {
            continue;
        }
        return &devices[i];
    }
    return 0;
}
