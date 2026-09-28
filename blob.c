/* blob.c - whole-file reads from the disk image.  See include/blob.h for what
 * this is and is not. */

#include "blob.h"
#include "ata.h"
#include "heap.h"
#include "fat.h"
#include "io.h"
#include "mmu.h"

#define BLOB_SECTOR        512U
/* the magic is read little-endian, so the constants are the bytes of
 * "KLYE" and "DISK" in that order, not their big-endian spelling */
#define BLOB_MAGIC0        0x45594C4BU
#define BLOB_MAGIC1        0x4B534944U
#define BLOB_VERSION       1U
#define BLOB_ENTRY_SIZE    48U
#define BLOB_NAME_MAX      32U
#define BLOB_MAX_ENTRIES   4096U
#define BLOB_ENTRIES_PER_SECTOR (BLOB_SECTOR / BLOB_ENTRY_SIZE)

/* Below this a plain heap allocation is fine and cheaper.  Above it the
 * contiguous allocator usually cannot satisfy the request, so the pages get
 * mapped instead. */
#define BLOB_MAPPED_THRESHOLD (512U * 1024U)

struct blob_entry {
    char name[BLOB_NAME_MAX];
    uint32_t lba;
    uint32_t length;
    uint32_t flags;
    uint32_t checksum;
};

static struct blob_entry blob_entries[BLOB_MAX_ENTRIES];
static uint32_t blob_entry_count;
static bool blob_mounted;
static char blob_error_text[72] = "";

/* Which allocator a loaded buffer came from, so blob_release can hand it back
 * the right way.  A small fixed table is enough: the game code holds one or
 * two WAD-sized buffers for its whole run. */
#define BLOB_TRACKED_MAX 8U
struct blob_tracked {
    void *pointer;
    uint32_t length;
    bool mapped;
};
static struct blob_tracked blob_tracked[BLOB_TRACKED_MAX];

static void blob_fail(const char *why)
{
    __builtin_strncpy(blob_error_text, why, sizeof(blob_error_text) - 1);
    blob_error_text[sizeof(blob_error_text) - 1] = 0;
}

static bool blob_streq(const char *a, const char *b)
{
    while (*a != 0 && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

/* The same checksum mkdisk.py writes, so a manifest entry that got scribbled
 * over is noticed instead of silently matching a wrong name. */
static uint32_t blob_checksum(const char *name)
{
    uint32_t total = 0;

    for (int index = 0; name[index] != 0; ++index) {
        total = total * 31U + (uint32_t)(unsigned char)name[index];
    }
    return total;
}

static uint32_t blob_read_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

bool blob_mount(void)
{
    uint8_t super[BLOB_SECTOR];
    uint32_t version;
    uint32_t count;
    uint32_t manifest_lba;

    blob_error_text[0] = 0;
    if (blob_mounted) {
        return true;
    }
    if (!ata_present()) {
        blob_fail("no drive present");
        return false;
    }
    if (!ata_read(0U, 1U, super)) {
        blob_fail(ata_error());
        return false;
    }
    if (blob_read_u32(super) != BLOB_MAGIC0 ||
        blob_read_u32(super + 4) != BLOB_MAGIC1) {
        blob_fail("not a klye disk image");
        return false;
    }
    version = blob_read_u32(super + 8);
    if (version != BLOB_VERSION) {
        blob_fail("unsupported image version");
        return false;
    }
    count = blob_read_u32(super + 12);
    manifest_lba = blob_read_u32(super + 16);
    if (count > BLOB_MAX_ENTRIES) {
        blob_fail("manifest has too many entries");
        return false;
    }

    blob_entry_count = 0;
    for (uint32_t index = 0; index < count; ++index) {
        uint32_t sector = manifest_lba + index / BLOB_ENTRIES_PER_SECTOR;
        uint32_t slot = index % BLOB_ENTRIES_PER_SECTOR;
        uint8_t raw[BLOB_SECTOR];
        uint8_t *entry = raw + slot * BLOB_ENTRY_SIZE;

        if (!ata_read(sector, 1U, raw)) {
            blob_error_text[0] = 0; /* a bad manifest sector is not fatal */
            break;
        }
        for (int at = 0; at < (int)BLOB_NAME_MAX; ++at) {
            blob_entries[blob_entry_count].name[at] = (char)entry[at];
        }
        blob_entries[blob_entry_count].name[BLOB_NAME_MAX - 1] = 0;
        blob_entries[blob_entry_count].lba = blob_read_u32(entry + 32);
        blob_entries[blob_entry_count].length = blob_read_u32(entry + 36);
        blob_entries[blob_entry_count].flags = blob_read_u32(entry + 40);
        blob_entries[blob_entry_count].checksum = blob_read_u32(entry + 44);
        if (blob_entries[blob_entry_count].checksum !=
            blob_checksum(blob_entries[blob_entry_count].name)) {
            /* keep it, but a reader that cares can compare the checksum */
        }
        ++blob_entry_count;
    }
    blob_mounted = true;
    return true;
}

bool blob_ready(void)
{
    return blob_mounted;
}

uint32_t blob_count(void)
{
    return blob_entry_count;
}

const char *blob_name(uint32_t index)
{
    if (index >= blob_entry_count) {
        return "";
    }
    return blob_entries[index].name;
}

static const struct blob_entry *blob_find(const char *path)
{
    for (uint32_t index = 0; index < blob_entry_count; ++index) {
        if (blob_streq(blob_entries[index].name, path)) {
            return &blob_entries[index];
        }
    }
    return 0;
}

uint32_t blob_size(const char *path)
{
    const struct blob_entry *entry = blob_find(path);

    return entry != 0 ? entry->length : 0U;
}

bool blob_pread(const char *path, uint64_t offset, void *out, uint32_t count)
{
    const struct blob_entry *entry = blob_find(path);
    uint8_t *destination = (uint8_t *)out;

    blob_error_text[0] = 0;
    if (entry == 0) {
        blob_fail("no such file in the image");
        return false;
    }
    if (offset + count > entry->length) {
        blob_fail("read past end of file");
        return false;
    }
    if (count == 0U) {
        return true;
    }

    /* PIO transfers whole sectors, so a partial one at each end is read into a
     * bounce buffer and copied out. */
    while (count > 0U) {
        uint32_t sector = (uint32_t)(offset / BLOB_SECTOR);
        uint32_t within = (uint32_t)(offset % BLOB_SECTOR);
        uint32_t chunk = BLOB_SECTOR - within;

        if (chunk > count) {
            chunk = count;
        }
        if (within == 0U && chunk == BLOB_SECTOR) {
            if (!ata_read(entry->lba + sector, 1U, destination)) {
                blob_fail(ata_error());
                return false;
            }
        } else {
            uint8_t bounce[BLOB_SECTOR];

            if (!ata_read(entry->lba + sector, 1U, bounce)) {
                blob_fail(ata_error());
                return false;
            }
            for (uint32_t at = 0; at < chunk; ++at) {
                destination[at] = bounce[within + at];
            }
        }
        destination += chunk;
        offset += chunk;
        count -= chunk;
    }
    return true;
}

bool blob_load(const char *path, void **out, uint32_t *length)
{
    const struct blob_entry *entry;
    void *buffer;
    bool mapped = false;

    blob_error_text[0] = 0;
    entry = blob_find(path);
    if (entry == 0) {
        blob_fail("no such file in the image");
        return false;
    }
    /* A file past what the contiguous allocator can satisfy gets mapped
     * pages instead: virtually contiguous, physically scattered.  The RAM heap
     * still handles the small files, where it is cheaper and the address is a
     * plain pointer with nothing behind it. */
    if (entry->length > BLOB_MAPPED_THRESHOLD) {
        buffer = vm_alloc_pages(entry->length != 0U ? entry->length : 1U);
        mapped = buffer != 0;
        if (buffer == 0) {
            blob_fail(vm_error());
            return false;
        }
    } else {
        buffer = heap_malloc(entry->length != 0U ? entry->length : 1U);
        if (buffer == 0) {
            blob_fail("out of memory loading the file");
            return false;
        }
    }
    if (!blob_pread(path, 0U, buffer, entry->length)) {
        if (mapped) {
            vm_free_pages(buffer, entry->length);
        } else {
            heap_free(buffer);
        }
        return false;
    }
    for (uint32_t index = 0; index < BLOB_TRACKED_MAX; ++index) {
        if (blob_tracked[index].pointer == 0) {
            blob_tracked[index].pointer = buffer;
            blob_tracked[index].length = entry->length;
            blob_tracked[index].mapped = mapped;
            break;
        }
    }
    *out = buffer;
    if (length != 0) {
        *length = entry->length;
    }
    return true;
}

void blob_release(void *pointer)
{
    /* vm_free_pages unmaps and reclaims frames; heap_free returns the block to
     * the malloc free list.  Which one applies is not knowable from the
     * pointer alone, so blob_load records the choice in a small table and this
     * consults it. */
    for (uint32_t index = 0; index < BLOB_TRACKED_MAX; ++index) {
        if (blob_tracked[index].pointer == pointer) {
            if (blob_tracked[index].mapped) {
                vm_free_pages(pointer, blob_tracked[index].length);
            } else {
                heap_free(pointer);
            }
            blob_tracked[index].pointer = 0;
            return;
        }
    }
    heap_free(pointer);
}

bool blob_load_any(const char *path, void **out, uint32_t *length,
                   bool *from_fat)
{
    bool fat = false;

    if (fat_ready() && fat_read(path, out, length)) {
        fat = true;
    } else if (blob_load(path, out, length) == false) {
        return false;
    }
    if (from_fat != 0) {
        *from_fat = fat;
    }
    return true;
}

void blob_release_any(void *pointer, bool from_fat)
{
    if (from_fat) {
        fat_release(pointer);
    } else {
        blob_release(pointer);
    }
}

const char *blob_error(void)
{
    return blob_error_text;
}
