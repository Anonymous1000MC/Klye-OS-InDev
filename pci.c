/* pci.c - walking the PCI bus and reading device identity.
 *
 * Nothing here maps anything.  See the header for why that is deliberate.
 */

#include "pci.h"

#include "io.h"

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

        bar->address = is_io ? (raw & 0xFFFFFFF0U) : (raw & 0xFFFFFFF0U);
        bar->is_io = is_io;
        bar->is_64bit = (is_io == false) && (low != 0U);
        bar->size = 0;
        if (raw == 0U) {
            continue; /* a unimplemented BAR, and not a reserved one */
        }
        if (out->bar_count < 6) {
            out->bar_count++;
        }
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
