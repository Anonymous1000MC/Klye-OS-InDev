#ifndef KLYE_ATA_H
#define KLYE_ATA_H

#include <stdbool.h>
#include <stdint.h>

#define ATA_SECTOR_BYTES 512U

/* Probe the primary master.  Safe to call when no drive is present. */
bool ata_identify(void);

/* True once a drive answered IDENTIFY and reported a sector count. */
bool ata_present(void);

/* Total sectors on the primary master, or 0 when there is no drive. */
uint32_t ata_sector_count(void);

/* Model string from IDENTIFY, trimmed of trailing spaces.  Never NULL. */
const char *ata_model(void);

/* Read `count` sectors starting at `lba` into `out`, which must have room for
 * count * 512 bytes.  Returns false on any timeout or error status, having
 * left `out` in an unspecified state. */
bool ata_read(uint32_t lba, uint32_t count, void *out);

/* Write `count` sectors.  Same contract as ata_read. */
bool ata_write(uint32_t lba, uint32_t count, const void *in);

/* Human readable reason for the most recent failure, or "" if none. */
const char *ata_error(void);

#endif
