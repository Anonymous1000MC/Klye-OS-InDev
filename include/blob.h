#ifndef KLYE_BLOB_H
#define KLYE_BLOB_H

#include <stdbool.h>
#include <stdint.h>

/* Whole-file access to the disk image.
 *
 * This is not a filesystem. It is a flat manifest of name/LBA/length triples
 * written by tools/mkdisk.py, which is enough to get a multi-megabyte file
 * such as a WAD in front of code that wants the whole thing in memory. The
 * RAM VFS cannot do that job: it holds around 160 KB in total and caps a
 * single file at 12 KB.
 *
 * Loaded blobs are ordinary heap allocations, so the caller owns the memory
 * and must release it with blob_release. */

/* Mount the disk image and read its manifest.  Requires a working drive.
 * Safe to call more than once; the second call is a no-op that succeeds. */
bool blob_mount(void);

/* True when a usable image was found. */
bool blob_ready(void);

/* Number of files in the image manifest. */
uint32_t blob_count(void);

/* Name of entry `index`, or "" when out of range. */
const char *blob_name(uint32_t index);

/* Size of the named file in bytes, or 0 when there is no such file. */
uint32_t blob_size(const char *path);

/* Load a whole file into a fresh heap buffer.  On success stores a pointer
 * that the caller must free with blob_release, sets *length to the file size,
 * and returns true.  On failure returns false, leaves *out alone, and sets a
 * reason readable with blob_error. */
bool blob_load(const char *path, void **out, uint32_t *length);

/* Read part of a file without loading all of it.  Useful for a WAD directory
 * where only some lumps are wanted. */
bool blob_pread(const char *path, uint64_t offset, void *out, uint32_t count);

/* Free a buffer from blob_load. */
void blob_release(void *pointer);

/* Reason for the most recent failure, or "" if none. */
const char *blob_error(void);

#endif
