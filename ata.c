/* ata.c - ATA PIO driver for the primary master on the legacy IDE ports.
 *
 * The controller sits at 0x1F0 in the legacy I/O range, so this needs no PCI
 * enumeration and no BAR mapping: the ports are fixed by the architecture and
 * QEMU wires its IDE interface there by default.  That makes a disk reachable
 * long before the PCI work lands.
 *
 * Everything here is LBA28 and program-mapped I/O.  PIO is slow, roughly a
 * megabyte a second, which is fine for loading a WAD at boot and useless for
 * anything that has to keep up with a 60 Hz frame loop, so DMA is the next
 * step if throughput ever matters.
 */

#include "ata.h"
#include "io.h"

#define ATA_DATA       0x1F0U
#define ATA_ERR        0x1F1U /* also the features register when written */
#define ATA_SCOUNT     0x1F2U
#define ATA_LBA_LOW    0x1F3U
#define ATA_LBA_MID    0x1F4U
#define ATA_LBA_HIGH   0x1F5U
#define ATA_DRIVE_HEAD 0x1F6U
#define ATA_STATUS     0x1F7U
#define ATA_CONTROL    0x3F6U

#define ST_ERR   0x01U /* error */
#define ST_DRQ   0x08U /* data request ready */
#define ST_DSC   0x10U /* data stream ready, used for LBA48 reads */
#define ST_DF    0x20U /* device fault */
#define ST_DRDY  0x40U /* device ready */
#define ST_BSY   0x80U /* busy */

#define CMD_READ     0x20U
#define CMD_WRITE    0x30U
#define CMD_IDENTIFY 0xECU

/* A real controller answers in well under a millisecond.  This is a spin
 * count, not a clock, so it is only a backstop against a missing drive:
 * 16 million iterations of an inb is roughly a second. */
#define ATA_SPIN_LIMIT 16000000U

static char ata_error_text[64] = "";
static uint32_t ata_sectors;
static char ata_model_text[41];
static bool ata_found;

static void ata_fail(const char *why)
{
    __builtin_strncpy(ata_error_text, why, sizeof(ata_error_text) - 1);
    ata_error_text[sizeof(ata_error_text) - 1] = 0;
}

/* Wait for BSY to clear.  Returns the status byte, or 0xFF on timeout, so
 * callers should treat 0xFF as failure rather than testing bits on it. */
static uint8_t ata_wait_ready(void)
{
    for (uint32_t spin = 0; spin < ATA_SPIN_LIMIT; ++spin) {
        uint8_t status = inb(ATA_STATUS);

        if ((status & ST_BSY) == 0) {
            return status;
        }
        io_wait();
    }
    return 0xFFU;
}

/* Wait for DRQ, which the drive asserts once a sector is ready to transfer. */
static bool ata_wait_drq(void)
{
    for (uint32_t spin = 0; spin < ATA_SPIN_LIMIT; ++spin) {
        uint8_t status = inb(ATA_STATUS);

        if (status == 0xFFU) {
            return false;
        }
        if (status & ST_BSY) {
            io_wait();
            continue;
        }
        if (status & ST_DRQ) {
            return true;
        }
        if (status & ST_ERR) {
            return false;
        }
        io_wait();
    }
    return false;
}

static void ata_pick_master(void)
{
    /* Bit 4 of the drive/head register selects the device; everything else
     * with bit 5 set selects LBA and the upper nibble of the LBA. */
    outb(ATA_CONTROL, 0x02U); /* nIEN, so the drive stays quiet for now */
    outb(ATA_DRIVE_HEAD, 0xA0U | 0x00U);
}

/* Program the LBA28 registers.  Kept separate because both the data commands
 * and IDENTIFY go through it. */
static void ata_set_lba(uint32_t lba)
{
    outb(ATA_SCOUNT, 1U);
    outb(ATA_LBA_LOW, (uint8_t)(lba & 0xFFU));
    outb(ATA_LBA_MID, (uint8_t)((lba >> 8) & 0xFFU));
    outb(ATA_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFFU));
    outb(ATA_DRIVE_HEAD, (uint8_t)(0xE0U | ((lba >> 24) & 0x0FU)));
}

/* Transfer one 512-byte sector as 256 words, which is what the data register
 * is: it is a 16-bit port, and reading it as bytes would double the port
 * access count and swap the halves of every other byte. */
/* The data register is a 16-bit port.  Going through inw/outw rather than
 * casting the port to a pointer keeps the compiler from trying to reason about
 * an object at 0x1F0, and it is what the hardware actually wants: one word per
 * access, not two bytes. */
static void ata_transfer_in(void *out)
{
    uint8_t *bytes = (uint8_t *)out;

    for (int index = 0; index < (int)(ATA_SECTOR_BYTES / 2U); ++index) {
        uint16_t word = inw(ATA_DATA);

        bytes[index * 2] = (uint8_t)(word & 0xFFU);
        bytes[index * 2 + 1] = (uint8_t)(word >> 8);
    }
}

static void ata_transfer_out(const void *in)
{
    const uint8_t *bytes = (const uint8_t *)in;

    for (int index = 0; index < (int)(ATA_SECTOR_BYTES / 2U); ++index) {
        uint16_t word = (uint16_t)((uint16_t)bytes[index * 2] |
                                   ((uint16_t)bytes[index * 2 + 1] << 8));

        outw(ATA_DATA, word);
    }
}

bool ata_read(uint32_t lba, uint32_t count, void *out)
{
    uint8_t *destination = (uint8_t *)out;

    ata_error_text[0] = 0;
    if (!ata_found) {
        ata_fail("no drive");
        return false;
    }
    if (count == 0U) {
        return true;
    }
    ata_pick_master();
    for (uint32_t index = 0; index < count; ++index) {
        uint32_t sector = lba + index;
        uint8_t status;

        if (sector >= ata_sectors) {
            ata_fail("lba past end of disk");
            return false;
        }
        ata_set_lba(sector);
        outb(ATA_ERR, 0x00U);
        outb(ATA_STATUS, CMD_READ);
        status = ata_wait_ready();
        if (status == 0xFFU) {
            ata_fail("timeout waiting for read");
            return false;
        }
        if (status & (ST_ERR | ST_DF)) {
            ata_fail("drive reported an error");
            return false;
        }
        if (!ata_wait_drq()) {
            ata_fail("drive never signalled data ready");
            return false;
        }
        ata_transfer_in(destination + index * ATA_SECTOR_BYTES);
    }
    return true;
}

bool ata_write(uint32_t lba, uint32_t count, const void *in)
{
    const uint8_t *source = (const uint8_t *)in;

    ata_error_text[0] = 0;
    if (!ata_found) {
        ata_fail("no drive");
        return false;
    }
    if (count == 0U) {
        return true;
    }
    ata_pick_master();
    for (uint32_t index = 0; index < count; ++index) {
        uint32_t sector = lba + index;
        uint8_t status;

        if (sector >= ata_sectors) {
            ata_fail("lba past end of disk");
            return false;
        }
        ata_set_lba(sector);
        outb(ATA_ERR, 0x00U);
        outb(ATA_STATUS, CMD_WRITE);
        status = ata_wait_ready();
        if (status == 0xFFU) {
            ata_fail("timeout waiting for write");
            return false;
        }
        if (status & (ST_ERR | ST_DF)) {
            ata_fail("drive reported an error");
            return false;
        }
        if (!ata_wait_drq()) {
            ata_fail("drive never signalled data ready");
            return false;
        }
        ata_transfer_out(source + index * ATA_SECTOR_BYTES);
    }
    return true;
}

/* IDENTIFY returns 256 words of device data, in ATA's usual byte-swapped word
 * order: strings are two bytes per word with the bytes reversed. */
bool ata_identify(void)
{
    uint8_t status;

    ata_pick_master();
    outb(ATA_SCOUNT, 0x00U);
    outb(ATA_LBA_LOW, 0x00U);
    outb(ATA_LBA_MID, 0x00U);
    outb(ATA_LBA_HIGH, 0x00U);
    outb(ATA_DRIVE_HEAD, 0xA0U);
    outb(ATA_ERR, 0x00U);
    outb(ATA_STATUS, CMD_IDENTIFY);

    status = ata_wait_ready();
    if (status == 0xFFU) {
        ata_fail("timeout waiting for identify");
        return false;
    }
    if (status & ST_ERR) {
        ata_fail("no drive answered identify");
        return false;
    }
    if (!ata_wait_drq()) {
        ata_fail("identify produced no data");
        return false;
    }

    {
        uint8_t raw[ATA_SECTOR_BYTES];
        uint32_t words[ATA_SECTOR_BYTES / 2U];

        ata_transfer_in(raw);
        for (uint32_t index = 0; index < ATA_SECTOR_BYTES / 2U; ++index) {
            /* each 16-bit word is stored low byte first in memory but the
             * string inside it is high byte first, so swap once here */
            words[index] = (uint32_t)raw[index * 2] |
                           ((uint32_t)raw[index * 2 + 1] << 8);
        }
        if (words[0] == 0U || words[82] == 0U) {
            ata_fail("drive is a slave or empty");
            return false;
        }
        ata_sectors = words[60] & 0x0FFFFFFFU;
        if (words[60] & 0x10000000U) {
            /* the high word of the sector count, for drives over 256 TiB */
            uint32_t high = words[61] & 0x0FFFFFFFU;

            ata_sectors |= high << 28;
        }
        for (int index = 0; index < 40; ++index) {
            ata_model_text[index] = (char)(words[27 + (uint32_t)index / 2U] >>
                                           ((index % 2 == 0) ? 8 : 0));
        }
        ata_model_text[40] = 0;
        for (int index = 39; index >= 0 && ata_model_text[index] == ' '; --index) {
            ata_model_text[index] = 0;
        }
        for (int index = 0; ata_model_text[index] != 0; ++index) {
            if ((unsigned char)ata_model_text[index] < 0x20U) {
                ata_model_text[index] = '.';
            }
        }
    }
    ata_found = true;
    return true;
}

bool ata_present(void)
{
    return ata_found;
}

uint32_t ata_sector_count(void)
{
    return ata_sectors;
}

const char *ata_model(void)
{
    return ata_model_text[0] != 0 ? ata_model_text : "(none)";
}

const char *ata_error(void)
{
    return ata_error_text;
}
