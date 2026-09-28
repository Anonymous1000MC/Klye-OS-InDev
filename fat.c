/* fat.c - FAT16 on top of the ATA block driver.  See include/fat.h. */

#include "fat.h"
#include "ata.h"
#include "heap.h"

#define SECTOR            512U
#define DIR_ENTRY_SIZE    32U
#define ATTR_READ_ONLY    0x01U
#define ATTR_HIDDEN       0x02U
#define ATTR_SYSTEM       0x04U
#define ATTR_VOLUME       0x08U
#define ATTR_DIRECTORY    0x10U
#define ATTR_ARCHIVE      0x20U
#define ATTR_LONG_NAME    0x0FU
#define CLUSTER_FREE      0x0000U
#define CLUSTER_EOC       0xFFF8U /* 0xFFF8..0xFFFF, the low 3 bits vary */
#define CLUSTER_MASK      0xFFF8U

/* A directory entry is 32 bytes, so sixteen fill a sector exactly. */
#define ENTRIES_PER_SECTOR (SECTOR / DIR_ENTRY_SIZE)

struct fat_volume {
    bool mounted;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t fat_count;
    uint32_t root_entries;
    uint32_t total_sectors;
    uint32_t sectors_per_fat;
    uint32_t root_cluster;   /* FAT16 keeps the root as a cluster number */
    uint32_t first_data_sector;
    uint32_t data_sectors;
    uint32_t cluster_count;
    char label[12];
};

static struct fat_volume fat;
static char fat_error_text[80] = "";

static void fat_fail(const char *why)
{
    __builtin_strncpy(fat_error_text, why, sizeof(fat_error_text) - 1);
    fat_error_text[sizeof(fat_error_text) - 1] = 0;
}

static uint16_t fat_u16(const uint8_t *at)
{
    return (uint16_t)((uint16_t)at[0] | (uint16_t)((uint16_t)at[1] << 8));
}

static uint32_t fat_u32(const uint8_t *at)
{
    return (uint32_t)at[0] | ((uint32_t)at[1] << 8) | ((uint32_t)at[2] << 16) |
           ((uint32_t)at[3] << 24);
}

static bool fat_sector(uint32_t index, void *out)
{
    return ata_read(index, 1U, out);
}

static uint32_t fat_cluster_sector(uint32_t cluster)
{
    return fat.first_data_sector +
           (cluster - 2U) * fat.sectors_per_cluster;
}

/* FAT entries live in a sector-aligned array, two bytes per cluster. */
static bool fat_get_entry(uint32_t cluster, uint16_t *out)
{
    uint32_t per_sector = SECTOR / 2U;
    uint32_t offset = cluster * 2U;
    uint8_t sector[SECTOR];

    if (cluster < 2U || cluster >= fat.cluster_count + 2U) {
        return false;
    }
    if (!fat_sector(fat.reserved_sectors + offset / per_sector, sector)) {
        return false;
    }
    *out = fat_u16(sector + (offset % per_sector) * 2U);
    return true;
}

static bool fat_set_entry(uint32_t cluster, uint16_t value)
{
    uint32_t per_sector = SECTOR / 2U;
    uint32_t offset = cluster * 2U;
    uint8_t sector[SECTOR];

    if (cluster < 2U || cluster >= fat.cluster_count + 2U) {
        return false;
    }
    if (!fat_sector(fat.reserved_sectors + offset / per_sector, sector)) {
        return false;
    }
    sector[(offset % per_sector) * 2U] = (uint8_t)(value & 0xFFU);
    sector[(offset % per_sector) * 2U + 1U] = (uint8_t)(value >> 8);
    return ata_write(fat.reserved_sectors + offset / per_sector, 1U, sector);
}

bool fat_mount(void)
{
    uint8_t boot[SECTOR];
    uint32_t root_dir_sectors;

    fat_error_text[0] = 0;
    if (fat.mounted) {
        return true;
    }
    if (!ata_present()) {
        fat_fail("no drive present");
        return false;
    }
    if (!fat_sector(0U, boot)) {
        fat_fail(ata_error());
        return false;
    }
    /* a FAT volume has a jump instruction, then the OEM name at offset 3 */
    if (boot[0] != 0xEBU && boot[0] != 0xE9U) {
        fat_fail("not a FAT volume: no boot jump");
        return false;
    }
    fat.bytes_per_sector = fat_u16(boot + 11U);
    fat.sectors_per_cluster = boot[13U];
    fat.reserved_sectors = fat_u16(boot + 14U);
    fat.fat_count = boot[16U];
    fat.root_entries = fat_u16(boot + 17U);
    fat.total_sectors = fat_u16(boot + 19U);
    fat.sectors_per_fat = fat_u16(boot + 22U);

    if (fat.bytes_per_sector != SECTOR) {
        fat_fail("only 512-byte sectors are supported");
        return false;
    }
    if (fat.total_sectors == 0U) {
        /* The 32-bit field at offset 32 is four bytes wide, not two.  A 64 MiB
         * volume is 131072 sectors, which does not fit the 16-bit field, so it
         * lands here with the low half reading as zero.  Reading only two bytes
         * left the count at zero, the data-sector subtraction underflowed, and
         * the volume was rejected as FAT32. */
        fat.total_sectors = fat_u32(boot + 32U);
    }
    if (fat.sectors_per_cluster == 0U || fat.fat_count == 0U ||
        fat.sectors_per_fat == 0U) {
        fat_fail("boot sector is not consistent");
        return false;
    }
    if (fat.fat_count < 2U) {
        fat_fail("only FAT12 and FAT16 are supported");
        return false;
    }
    /* FAT12 and FAT32 encode the cluster count differently and would need
     * different entry decoding, so make sure this really is FAT16 */
    root_dir_sectors =
        ((fat.root_entries * DIR_ENTRY_SIZE) + SECTOR - 1U) / SECTOR;
    fat.data_sectors = fat.total_sectors -
                       (fat.reserved_sectors +
                        (fat.fat_count * fat.sectors_per_fat) +
                        root_dir_sectors);
    fat.cluster_count = fat.data_sectors / fat.sectors_per_cluster;
    if (fat.cluster_count < 4085U) {
        fat_fail("volume is too small to be FAT16");
        return false;
    }
    if (fat.cluster_count > 0xFFF5U) {
        fat_fail("volume is FAT32, not FAT16");
        return false;
    }
    fat.first_data_sector = fat.reserved_sectors +
                            (fat.fat_count * fat.sectors_per_fat) +
                            root_dir_sectors;
    fat.root_cluster = 2U;

    /* The volume label is an 11-byte field in the BPB at 0x2B.  Offset 71 is
     * where the extended boot code starts, so reading the label from there
     * produced whatever bytes happened to be in the boot code. */
    for (int index = 0; index < 11; ++index) {
        fat.label[index] = (char)boot[43U + (uint32_t)index];
    }
    fat.label[11] = 0;
    for (int index = 10; index >= 0 && fat.label[index] == ' '; --index) {
        fat.label[index] = 0;
    }
    fat.mounted = true;
    return true;
}

bool fat_ready(void)
{
    return fat.mounted;
}

const char *fat_label(void)
{
    return fat.label;
}

const char *fat_error(void)
{
    return fat_error_text;
}

/* Copy a path element, rejecting "." and ".." and anything empty, so a caller
 * cannot walk above the root. */
static bool fat_next_element(const char **cursor, char *out, int out_max)
{
    const char *start = *cursor;
    int length = 0;

    if (**cursor == '/') {
        ++(*cursor);
    }
    if (**cursor == 0) {
        return false;
    }
    while (**cursor != 0 && **cursor != '/') {
        if (length + 1 >= out_max) {
            return false;
        }
        out[length++] = **cursor;
        ++(*cursor);
    }
    out[length] = 0;
    if (length == 0) {
        return false;
    }
    (void)start;
    return true;
}

static bool fat_same_name(const char *stored, const char *wanted)
{
    int left = 0;
    int right = 0;

    while (stored[left] != 0 && stored[left] != ' ') {
        ++left;
    }
    while (wanted[right] != 0) {
        ++right;
    }
    /* compare case-insensitively, which is what FAT expects */
    for (int index = 0; index < left || index < right; ++index) {
        char a = index < left ? stored[index] : 0;
        char b = index < right ? wanted[index] : 0;

        if (a >= 'A' && a <= 'Z') {
            a = (char)(a + ('a' - 'A'));
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b + ('a' - 'A'));
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

struct fat_entry {
    char name[FAT_NAME_MAX];
    uint32_t cluster;
    uint32_t size;
    bool directory;
    bool valid;
};

/* A long filename entry holds 13 characters spread across three slots, in
 * reverse order: the last logical character comes first. */
static void fat_lfn_piece(const uint8_t *entry, char *out)
{
    static const int offsets[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24,
                                     28, 30 };
    int at = 0;

    for (int index = 0; index < 13; ++index) {
        uint8_t value = entry[offsets[index]];

        if (value == 0x00U || value == 0xFFU) {
            break;
        }
        if (at < FAT_NAME_MAX - 1) {
            out[at++] = (char)value;
        }
    }
    out[at] = 0;
}

/* Scan one directory's sectors, handing every entry to `visit` along with a
 * running long-name accumulator.  Returns false only on a read error. */
struct fat_visitor {
    int (*visit)(void *context, const struct fat_entry *entry);
    void *context;
};

static bool fat_scan_directory(uint32_t cluster, struct fat_visitor *visitor)
{
    uint8_t sector[SECTOR];
    char long_name[FAT_NAME_MAX];
    int long_length = 0;
    /* The root is a fixed run of sectors rather than a cluster chain, so it is
     * walked once.  Everything else follows the FAT, because a directory can
     * span any number of clusters. */
    uint32_t sectors_left = (cluster == 2U)
                                ? ((fat.root_entries * DIR_ENTRY_SIZE +
                                    SECTOR - 1U) / SECTOR)
                                : fat.sectors_per_cluster;
    uint32_t base = (cluster == 2U)
                        ? (fat.reserved_sectors +
                           fat.fat_count * fat.sectors_per_fat)
                        : fat_cluster_sector(cluster);
    uint32_t walked = 0;

    while (sectors_left > 0U) {
        uint32_t index = walked % fat.sectors_per_cluster;
        uint32_t here = base + index;

        if (index == 0U && walked > 0U) {
            /* moved on to the next cluster in the chain */
            uint16_t next;

            if (!fat_get_entry(cluster, &next)) {
                return false;
            }
            if ((next & CLUSTER_MASK) >= CLUSTER_EOC) {
                return true;
            }
            cluster = next & CLUSTER_MASK;
            base = fat_cluster_sector(cluster);
        }
        walked++;
        sectors_left--;
        if (!fat_sector(here, sector)) {
            return false;
        }
        for (uint32_t at = 0; at < ENTRIES_PER_SECTOR; ++at) {
            const uint8_t *entry = sector + at * DIR_ENTRY_SIZE;
            struct fat_entry parsed;

            if (entry[0] == 0x00U) {
                return true; /* no further entries in this directory */
            }
            if (entry[0] == 0xE5U) {
                long_length = 0;
                continue;
            }
            if ((entry[11U] & ATTR_LONG_NAME) == ATTR_LONG_NAME) {
                char piece[FAT_NAME_MAX];

                fat_lfn_piece(entry, piece);
                /* entries arrive last chunk first, so prepend */
                if (long_length + (int)__builtin_strlen(piece) <
                    FAT_NAME_MAX - 1) {
                    int tail = long_length;

                    for (int i = 0; piece[i] != 0; ++i) {
                        long_name[tail++] = piece[i];
                    }
                    long_name[tail] = 0;
                    long_length = tail;
                }
                continue;
            }
            if ((entry[11U] & ATTR_VOLUME) != 0U) {
                long_length = 0;
                continue;
            }
            /* For FAT12 and FAT16 the whole cluster number is the 16-bit field
             * at offset 26.  Offsets 22 to 25 only carry a cluster high half on
             * FAT32; on FAT16 they are unused.  Reading the low half from offset
             * 20, which is reserved, and the high half from 26 gave absurd
             * cluster numbers such as 196608. */
            parsed.cluster = fat_u16(entry + 26U);
            parsed.size = fat_u32(entry + 28U);
            parsed.directory = (entry[11U] & ATTR_DIRECTORY) != 0U;
            parsed.valid = true;
            if (long_length > 0) {
                for (int i = 0; i < long_length; ++i) {
                    parsed.name[i] = long_name[i];
                }
                parsed.name[long_length] = 0;
            } else {
                int at_name = 0;

                for (int i = 0; i < 8 && entry[i] != ' '; ++i) {
                    parsed.name[at_name++] = (char)entry[i];
                }
                if (entry[8U] != ' ') {
                    parsed.name[at_name++] = '.';
                    for (int i = 8; i < 11 && entry[i] != ' '; ++i) {
                        parsed.name[at_name++] = (char)entry[i];
                    }
                }
                parsed.name[at_name] = 0;
            }
            long_length = 0;
            if (parsed.name[0] == '.') {
                continue; /* skip "." and ".." */
            }
            if (visitor->visit(visitor->context, &parsed) != 0) {
                return true;
            }
        }
    }
    return true;
}

struct fat_find_context {
    const char *wanted;
    struct fat_entry found;
};

static int fat_find_visit(void *context, const struct fat_entry *entry)
{
    struct fat_find_context *state = (struct fat_find_context *)context;

    if (fat_same_name(entry->name, state->wanted)) {
        state->found = *entry;
        return 1;
    }
    return 0;
}

/* Walk a path to one entry.  Each element is looked up inside the directory it
 * names, starting at the root, so nothing outside the volume is reachable. */
static bool fat_lookup_path(const char *path, struct fat_entry *out)
{
    const char *cursor = path;
    char element[FAT_NAME_MAX];
    struct fat_entry current;

    if (fat.mounted == false) {
        fat_fail("no volume mounted");
        return false;
    }
    current.cluster = fat.root_cluster;
    current.directory = true;
    current.size = 0;
    current.valid = true;
    for (int index = 0; index < (int)sizeof(current.name); ++index) {
        current.name[index] = 0;
    }

    while (fat_next_element(&cursor, element, FAT_NAME_MAX)) {
        struct fat_find_context state;
        struct fat_visitor visitor;
        bool last = *cursor == 0;

        state.wanted = element;
        state.found.valid = false;
        for (int index = 0; index < (int)sizeof(state.found.name); ++index) {
            state.found.name[index] = 0;
        }
        visitor.visit = fat_find_visit;
        visitor.context = &state;
        if (!fat_scan_directory(current.cluster, &visitor)) {
            fat_fail(ata_error());
            return false;
        }
        if (state.found.valid == false) {
            fat_fail("no such file or directory");
            return false;
        }
        if (last == false && state.found.directory == false) {
            fat_fail("a path element is not a directory");
            return false;
        }
        current = state.found;
    }
    *out = current;
    return true;
}

bool fat_stat_file(const char *path, uint32_t *size)
{
    struct fat_entry entry;

    if (!fat_lookup_path(path, &entry)) {
        return false;
    }
    if (size != 0) {
        *size = entry.size;
    }
    return true;
}

/* Copy `count` bytes starting `offset` into a file, following the cluster
 * chain.  Reads that run off the end of the file fail rather than pad. */
static bool fat_read_chain(uint32_t cluster, uint64_t offset, void *out,
                           uint32_t count)
{
    uint8_t *destination = (uint8_t *)out;
    uint32_t cluster_bytes = fat.sectors_per_cluster * SECTOR;

    while (count > 0U) {
        uint32_t within;
        uint32_t chunk;
        uint8_t sector[SECTOR];

        if (cluster < 2U) {
            fat_fail("file has no data cluster");
            return false;
        }
        if (offset % cluster_bytes == 0U) {
            /* cluster aligned: straight sectors */
            uint32_t first = fat_cluster_sector(cluster) +
                             (uint32_t)(offset / cluster_bytes);
            uint32_t sectors = count / SECTOR;

            if (sectors > 0U) {
                for (uint32_t index = 0; index < sectors; ++index) {
                    if (!fat_sector(first + index, sector)) {
                        fat_fail(ata_error());
                        return false;
                    }
                    for (uint32_t byte = 0; byte < SECTOR; ++byte) {
                        destination[index * SECTOR + byte] = sector[byte];
                    }
                }
                destination += sectors * SECTOR;
                count -= sectors * SECTOR;
                offset += sectors * SECTOR;
                continue;
            }
        }
        /* a partial sector at the end of the file */
        within = (uint32_t)(offset % cluster_bytes);
        chunk = SECTOR - within;
        if (chunk > count) {
            chunk = count;
        }
        if (!fat_sector(fat_cluster_sector(cluster) + within / SECTOR,
                        sector)) {
            fat_fail(ata_error());
            return false;
        }
        for (uint32_t byte = 0; byte < chunk; ++byte) {
            destination[byte] = sector[within + byte];
        }
        destination += chunk;
        count -= chunk;
        offset += chunk;
        if (within + chunk >= cluster_bytes) {
            uint16_t next;

            if (!fat_get_entry(cluster, &next)) {
                fat_fail(ata_error());
                return false;
            }
            if ((next & CLUSTER_MASK) >= CLUSTER_EOC) {
                fat_fail("file ends before the requested length");
                return false;
            }
            cluster = next & CLUSTER_MASK;
        }
    }
    return true;
}

bool fat_read_range(const char *path, uint64_t offset, void *out,
                    uint32_t count)
{
    struct fat_entry entry;

    if (!fat_lookup_path(path, &entry)) {
        return false;
    }
    if (offset + count > entry.size) {
        fat_fail("read past end of file");
        return false;
    }
    if (count == 0U) {
        return true;
    }
    return fat_read_chain(entry.cluster, offset, out, count);
}

bool fat_read(const char *path, void **out, uint32_t *length)
{
    struct fat_entry entry;
    void *buffer;

    if (!fat_lookup_path(path, &entry)) {
        return false;
    }
    buffer = heap_malloc(entry.size != 0U ? entry.size : 1U);
    if (buffer == 0) {
        fat_fail("out of memory reading the file");
        return false;
    }
    if (entry.size != 0U && !fat_read_chain(entry.cluster, 0, buffer,
                                            entry.size)) {
        heap_free(buffer);
        return false;
    }
    *out = buffer;
    if (length != 0) {
        *length = entry.size;
    }
    return true;
}

void fat_release(void *pointer)
{
    heap_free(pointer);
}

/* ------------------------------------------------------------- writing ---- */

struct fat_list_context {
    int count;
    int max;
    struct fat_stat *out;
};

static int fat_list_visit(void *context, const struct fat_entry *entry)
{
    struct fat_list_context *state = (struct fat_list_context *)context;
    struct fat_stat *slot;

    if (state->count >= state->max) {
        return 1;
    }
    slot = &state->out[state->count++];
    for (int index = 0; index < FAT_NAME_MAX; ++index) {
        slot->name[index] = index < (int)__builtin_strlen(entry->name)
                                ? entry->name[index]
                                : 0;
    }
    slot->name[FAT_NAME_MAX - 1] = 0;
    slot->size = entry->size;
    slot->directory = entry->directory;
    return 0;
}

int fat_list(const char *path, struct fat_stat *out, int max)
{
    struct fat_entry entry;
    struct fat_list_context state;
    struct fat_visitor visitor;

    if (!fat_lookup_path(path, &entry)) {
        return -1;
    }
    if (entry.directory == false) {
        fat_fail("not a directory");
        return -1;
    }
    state.count = 0;
    state.max = max;
    state.out = out;
    visitor.visit = fat_list_visit;
    visitor.context = &state;
    if (!fat_scan_directory(entry.cluster, &visitor)) {
        return -1;
    }
    return state.count;
}

static const char *strrchr_local(const char *text, char wanted)
{
    const char *found = 0;

    while (*text != 0) {
        if (*text == wanted) {
            found = text;
        }
        ++text;
    }
    return found;
}

/* Find a free run of `count` clusters, marking them used and linking them into
 * a chain.  Allocation is first fit from cluster 2 upwards, which is not fast
 * but is predictable, and the FAT already knows which clusters are free so
 * nothing has to be tracked twice. */
static bool fat_allocate(uint32_t count, uint32_t *first_out)
{
    uint32_t start = 0;
    uint32_t run = 0;
    uint32_t run_start = 0;

    for (uint32_t cluster = 2U; cluster < fat.cluster_count + 2U; ++cluster) {
        uint16_t entry;

        if (!fat_get_entry(cluster, &entry)) {
            fat_fail(ata_error());
            return false;
        }
        if ((entry & CLUSTER_MASK) == CLUSTER_FREE) {
            if (run == 0U) {
                run_start = cluster;
            }
            ++run;
            if (run == count) {
                start = run_start;
                break;
            }
        } else {
            run = 0;
        }
    }
    if (run < count) {
        fat_fail("not enough free space on the volume");
        return false;
    }
    for (uint32_t index = 0; index < count; ++index) {
        uint16_t next = (index + 1U < count)
                            ? (uint16_t)(start + index + 1U)
                            : (uint16_t)(CLUSTER_EOC | (start + index));

        if (!fat_set_entry(start + index, next)) {
            fat_fail(ata_error());
            return false;
        }
    }
    *first_out = start;
    return true;
}

/* Release a chain, so a rewritten file does not leak its old clusters. */
static void fat_free_chain(uint32_t cluster)
{
    uint32_t guard = 0;

    while (cluster >= 2U && cluster < fat.cluster_count + 2U &&
           guard <= fat.cluster_count) {
        uint16_t next;

        ++guard;
        if (!fat_get_entry(cluster, &next)) {
            return;
        }
        (void)fat_set_entry(cluster, CLUSTER_FREE);
        if ((next & CLUSTER_MASK) >= CLUSTER_EOC) {
            return;
        }
        cluster = next & CLUSTER_MASK;
    }
}

/* Turn a name into an 8.3 entry.  Anything that will not fit is rejected
 * rather than silently mangled, so a caller learns the name is unusable. */
static bool fat_short_name(const char *name, uint8_t *out)
{
    int base = 0;
    int extension = 0;
    int length = (int)__builtin_strlen(name);

    for (int index = 0; index < 11; ++index) {
        out[index] = ' ';
    }
    for (int index = 0; index < length; ++index) {
        char c = name[index];
        int dot = -1;

        for (int at = 0; at < index; ++at) {
            if (name[at] == '.') {
                dot = at;
            }
        }
        if (c == '.') {
            continue;
        }
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - ('a' - 'A'));
        }
        if (c == ' ') {
            continue;
        }
        if (dot >= 0) {
            if (extension >= 3) {
                return false;
            }
            out[8 + extension] = (uint8_t)c;
            ++extension;
        } else {
            if (index - (dot >= 0 ? dot + 1 : 0) >= 8) {
                return false;
            }
            if (base >= 8) {
                return false;
            }
            out[base] = (uint8_t)c;
            ++base;
        }
    }
    if (base == 0 && extension == 0) {
        return false;
    }
    return true;
}

/* Find a directory entry to overwrite, or a free one to claim. */
static bool fat_find_slot(uint32_t directory, const char *name, bool create,
                          uint32_t *sector_out, uint32_t *offset_out)
{
    uint8_t sector[SECTOR];
    uint32_t base;
    uint32_t sectors;
    bool root = (directory == 2U);

    sectors = root ? ((fat.root_entries * DIR_ENTRY_SIZE) + SECTOR - 1U) / SECTOR
                   : fat.sectors_per_cluster;
    base = root ? (fat.reserved_sectors + fat.fat_count * fat.sectors_per_fat)
                : fat_cluster_sector(directory);

    for (uint32_t index = 0; index < sectors; ++index) {
        if (!fat_sector(base + index, sector)) {
            fat_fail(ata_error());
            return false;
        }
        for (uint32_t at = 0; at < ENTRIES_PER_SECTOR; ++at) {
            const uint8_t *entry = sector + at * DIR_ENTRY_SIZE;

            if (entry[0] == 0x00U || entry[0] == 0xE5U) {
                if (create) {
                    *sector_out = base + index;
                    *offset_out = at * DIR_ENTRY_SIZE;
                    return true;
                }
                if (entry[0] == 0x00U) {
                    return false; /* end of directory, nothing to find */
                }
                continue;
            }
            if ((entry[11U] & (ATTR_VOLUME | ATTR_LONG_NAME)) != 0U) {
                continue;
            }
            /* compare the 8.3 form */
            {
                char stored[16];
                int at_name = 0;

                for (int i = 0; i < 8 && entry[i] != ' '; ++i) {
                    stored[at_name++] = (char)entry[i];
                }
                if (entry[8] != ' ') {
                    stored[at_name++] = '.';
                    for (int i = 8; i < 11 && entry[i] != ' '; ++i) {
                        stored[at_name++] = (char)entry[i];
                    }
                }
                stored[at_name] = 0;
                if (fat_same_name(stored, name)) {
                    *sector_out = base + index;
                    *offset_out = at * DIR_ENTRY_SIZE;
                    return true;
                }
            }
        }
    }
    return false;
}

bool fat_write(const char *path, const void *data, uint32_t length)
{
    char directory_path[FAT_PATH_MAX];
    const char *name;
    uint8_t short_name[11];
    struct fat_entry parent;
    uint8_t sector[SECTOR];
    uint32_t entry_sector;
    uint32_t entry_offset;
    uint32_t clusters;
    uint32_t first = 0;
    uint32_t written = 0;
    const uint8_t *source = (const uint8_t *)data;

    fat_error_text[0] = 0;
    if (fat.mounted == false) {
        fat_fail("no volume mounted");
        return false;
    }
    /* split into "directory" and "name" */
    {
        int length_dir = 0;

        for (int index = 0; path[index] != 0; ++index) {
            if (path[index] == '/') {
                length_dir = index;
            }
        }
        if (length_dir == 0) {
            for (int index = 0; index < FAT_PATH_MAX; ++index) {
                directory_path[index] = index == 0 ? '/' : 0;
            }
            if (directory_path[1] != 0) {
                directory_path[1] = 0;
            }
        } else {
            for (int index = 0; index < length_dir; ++index) {
                directory_path[index] = path[index];
            }
            directory_path[length_dir] = 0;
        }
        name = strrchr_local(path, '/');
        name = (name != 0) ? name + 1 : path;
    }
    if (name[0] == 0) {
        fat_fail("a file needs a name");
        return false;
    }
    if (!fat_short_name(name, short_name)) {
        fat_fail("name does not fit the 8.3 form");
        return false;
    }
    if (!fat_lookup_path(directory_path, &parent)) {
        return false;
    }
    if (parent.directory == false) {
        fat_fail("the parent path is not a directory");
        return false;
    }

    /* release the old file's clusters so a rewrite does not leak them */
    if (fat_find_slot(parent.cluster, name, false, &entry_sector,
                      &entry_offset)) {
        if (fat_sector(entry_sector, sector)) {
            const uint8_t *entry = sector + entry_offset;
            uint32_t old = fat_u16(entry + 26U);

            if (old >= 2U) {
                fat_free_chain(old);
            }
            /* mark the old entry deleted */
            sector[entry_offset] = 0xE5U;
            (void)ata_write(entry_sector, 1U, sector);
        }
    }

    clusters = (length + fat.sectors_per_cluster * SECTOR - 1U) /
               (fat.sectors_per_cluster * SECTOR);
    if (clusters == 0U) {
        clusters = 1U;
    }
    if (!fat_allocate(clusters, &first)) {
        return false;
    }

    /* write the data cluster by cluster */
    while (written < length || (length == 0U && written == 0U)) {
        uint8_t block[512];
        uint32_t cluster_bytes = fat.sectors_per_cluster * SECTOR;
        uint32_t chunk = length - written;
        uint32_t base = fat_cluster_sector(first);

        if (chunk > cluster_bytes) {
            chunk = cluster_bytes;
        }
        for (uint32_t index = 0; index < fat.sectors_per_cluster; ++index) {
            uint32_t at = 0;

            while (at < SECTOR && at < chunk) {
                block[at] = source[written + at];
                ++at;
            }
            while (at < SECTOR) {
                block[at] = 0;
                ++at;
            }
            if (!ata_write(base + index, 1U, block)) {
                fat_fail(ata_error());
                return false;
            }
            if (written + at >= length) {
                written += at;
                break;
            }
            written += at;
        }
        if (length == 0U) {
            break;
        }
        /* advance to the next cluster in the chain */
        {
            uint16_t next;

            if (!fat_get_entry(first, &next)) {
                fat_fail(ata_error());
                return false;
            }
            if ((next & CLUSTER_MASK) >= CLUSTER_EOC) {
                break;
            }
            first = next & CLUSTER_MASK;
        }
    }

    /* claim or rewrite the directory entry */
    if (!fat_find_slot(parent.cluster, name, true, &entry_sector,
                       &entry_offset)) {
        fat_fail("no free directory entry for the file");
        return false;
    }
    if (!fat_sector(entry_sector, sector)) {
        fat_fail(ata_error());
        return false;
    }
    {
        uint8_t *entry = sector + entry_offset;

        for (int index = 0; index < 11; ++index) {
            entry[index] = short_name[index];
        }
        entry[11] = ATTR_ARCHIVE;
        /* 12 to 25 are reserved, then the 16-bit cluster at 26, then the size.
         * Writing the cluster across 20 to 23 as well put it in fields that do
         * not exist for FAT16. */
        for (int index = 12; index < 26; ++index) {
            entry[index] = 0;
        }
        entry[26] = (uint8_t)(first & 0xFFU);
        entry[27] = (uint8_t)(first >> 8);
        entry[28] = (uint8_t)(length & 0xFFU);
        entry[29] = (uint8_t)((length >> 8) & 0xFFU);
        entry[30] = (uint8_t)((length >> 16) & 0xFFU);
        entry[31] = (uint8_t)((length >> 24) & 0xFFU);
    }
    if (!ata_write(entry_sector, 1U, sector)) {
        fat_fail(ata_error());
        return false;
    }
    return true;
}
