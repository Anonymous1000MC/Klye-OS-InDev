#ifndef KLYE_FAT_H
#define KLYE_FAT_H

#include <stdbool.h>
#include <stdint.h>

/* FAT16, so the disk holds a real filesystem instead of a flat manifest.
 *
 * Enough of the format to mount a volume, walk directories, read a file, and
 * create and extend one.  Long filenames are supported, because a path that
 * only works when every name fits in 8.3 is not much of a filesystem.
 *
 * Writes append into the first run of free clusters the FAT already knows
 * about, and never move existing data, so there is no defragmentation and no
 * risk of corrupting a file that was already there.  There is no free-space
 * accounting beyond what the FAT itself says, and no delete, so a volume fills
 * up and stays full.
 */

#define FAT_NAME_MAX 256
#define FAT_PATH_MAX 320

/* Mount the volume on sector 0.  Safe to call again; the second call is a
 * no-op that succeeds. */
bool fat_mount(void);

/* True when a FAT volume was found and understood. */
bool fat_ready(void);

/* Volume label, or "" when there is none. */
const char *fat_label(void);

struct fat_stat {
    char name[FAT_NAME_MAX];
    uint32_t size;
    bool directory;
};

/* List a directory.  `path` is absolute, and "/" is the root.  Returns the
 * number of entries written, or -1 on error. */
int fat_list(const char *path, struct fat_stat *out, int max);

/* Look a file up.  Returns false when it does not exist. */
bool fat_stat_file(const char *path, uint32_t *size);

/* Read a whole file into a fresh heap buffer, or read a slice of one.  On
 * success stores a buffer the caller must release with fat_release and the
 * bytes actually read. */
bool fat_read(const char *path, void **out, uint32_t *length);
bool fat_read_range(const char *path, uint64_t offset, void *out,
                    uint32_t count);

/* Create a file, replacing any existing one, and write `length` bytes.  Used by
 * `kpm install` so an installed app survives a reboot. */
bool fat_write(const char *path, const void *data, uint32_t length);

/* Release a buffer from fat_read. */
void fat_release(void *pointer);

/* Reason for the most recent failure, or "" if none. */
const char *fat_error(void);

#endif
