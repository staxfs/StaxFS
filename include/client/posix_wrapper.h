#pragma once

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#ifdef __cplusplus
  #include <cstdint>
  #include <fcntl.h>
  #include <sys/types.h>
using ssize_t = int64_t;
using size_t = uint64_t;
using off_t = int64_t;
using off64_t = int64_t;
using dev_t = uint64_t;
using mode_t = uint32_t;
extern "C" {
#else
  #include <fcntl.h>
  #include <stdint.h>
  #include <sys/types.h>
#endif
#include <linux/stat.h>
#include <stdarg.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>

struct LockEntry {
  pid_t pid_;
  struct flock lock_;
};

bool LocksConflict(const struct flock *a, const struct flock *b);

int dfs_open(const char *pathname, int flags, mode_t mode);
int64_t dfs_lseek(int fd, int64_t offset, int whence);
ssize_t dfs_read(int fd, void *buf, size_t count);
ssize_t dfs_write(int fd, const void *buf, size_t count);
int64_t dfs_pread(int fd, void *buf, uint64_t count, int64_t offset);
int64_t dfs_pwrite(int fd, const void *buf, uint64_t count, int64_t offset);
int dfs_fsync(int fd);
int dfs_fdatasync(int fd);
int dfs_close(int fd);

int dfs_dup(int oldfd);
int dfs_dup3(int oldfd, int newfd, int flags);
int dfs_fcntl(int fd, int cmd, va_list args);

int dfs_stat(const char *pathname, struct stat *statbuf);
int dfs_fstat(int fd, struct stat *statbuf);
int dfs_access(const char *pathname, int mode);
int dfs_rename(const char *oldpath, const char *newpath);
int dfs_unlink(const char *pathname);
int dfs_mkdir(const char *pathname, uint32_t mode);
int dfs_rmdir(const char *pathname);
int dfs_link(const char *oldpath, const char *newpath);

/** libc interface requires that we use DIR *, whose structure is opaque to
 * users by design, so we can't use it in our code and had to use our own
 * Dirstream *. Therefore, to distinguish whether a stream pointer is from dfs
 * or local fs, we have to add a tag in the pointer itself. However, we can't
 * use the upper 16 bits because nowadays CXL-enabled system may need them for
 * addressing. So we can only use the LSB as tag, hoping that DIR * is aligned
 * to 4 bytes or sth. */
static inline int is_dfs_dirstream(DIR *sptr) { return (uint64_t)sptr & 1; }

DIR *dfs_opendir(const char *pathname, int alloc);
struct dirent *dfs_readdir(DIR *sptr);
int dfs_closedir(DIR *sptr);

int dfs_creat(const char *pathname, uint32_t mode);
int dfs_statx(int dirfd, const char *pathname, int flags, unsigned int mask,
              struct statx *statxbuf);

int dfs_chmod(const char *pathname, mode_t mode);
FILE *dfs_fdopen(int fd, const char *mode);
FILE *dfs_fopen(const char *pathname, const char *mode);
int dfs_fileno(FILE *stream);
void *dfs_mmap(void *addr, size_t len, int prot, int flags, int fd,
               __off_t offset);
int dfs_munmap(void *addr, size_t len);
int dfs_mkstemp(char *pathname);
int dfs_symlink(const char *linkname, const char *pathname);

// TODO
int dfs_mknod(const char *pathname, mode_t mode, dev_t dev);
int dfs_statfs(const char *path, struct statfs *buf);
int dfs_statvfs(const char *path, struct statvfs *buf);

#ifdef __cplusplus
}
#endif
