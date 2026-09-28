#ifndef KLYE_VFS_H
#define KLYE_VFS_H

#include <stdbool.h>
#include <stdint.h>

/* These are ceilings, not storage.  The tables they describe are allocated
 * from the heap when the filesystem mounts, because they used to be static
 * arrays in BSS and together they were most of it: 160 KiB of block store in
 * a 1 MB BSS, which ran out before the disk did and stopped anything from
 * being installed.  A guest too small for the full size gets a smaller
 * filesystem rather than no filesystem, so vfs_node_capacity() and
 * vfs_bytes_total() report what was actually allocated. */
#define VFS_MAX_NODES 512
#define VFS_NAME_MAX 48
#define VFS_PATH_MAX 160
#define VFS_BLOCK_SIZE 512
#define VFS_TOTAL_BLOCKS 8192    /* 4 MiB of file data */
#define VFS_MAX_BLOCKS_PER_FILE 512  /* 256 KiB in any one file */
#define VFS_BODY_MAX 4096
#define VFS_LIST_MAX 64

bool vfs_mount(void);
bool vfs_mounted(void);
void vfs_format(void);
uint32_t vfs_bytes_used(void);
uint32_t vfs_bytes_total(void);
int vfs_node_total(void);
int vfs_node_capacity(void);

int vfs_resolve(const char *path, const char *cwd);
void vfs_absolute_of(const char *path, const char *cwd, char *out, int max);
int vfs_resolve_parent(const char *path, const char *cwd, char *name,
                       int name_max);
bool vfs_exists(const char *path);
bool vfs_is_directory_path(const char *path);

int vfs_create(const char *path, bool directory);
int vfs_touch(const char *path);
int vfs_delete(const char *path);
int vfs_rename(const char *path, const char *new_name);

int vfs_open(const char *path);
int vfs_read(const char *path, char *out, uint32_t max);
int vfs_write(const char *path, const char *data, uint32_t length);
int vfs_append(const char *path, const char *data, uint32_t length);
int vfs_truncate(const char *path);

int vfs_list(const char *directory, int *out_indices, int maximum);
int vfs_list_path(const char *path, int *out_indices, int maximum);
int vfs_child_count(int index);

int vfs_count(void);
uint32_t vfs_bytes_free(void);
const char *vfs_path(int index);
const char *vfs_name(int index);
const char *vfs_kind(int index);
uint32_t vfs_size(int index);
const char *vfs_body(int index);
const char *vfs_find(const char *path);
bool vfs_is_directory(const char *path);

int vfs_parent(int index);
int vfs_node_created(int index);
uint32_t vfs_node_modified(int index);

#endif
