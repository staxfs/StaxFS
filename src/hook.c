#define _GNU_SOURCE

#include "client/g_constants.h"
#include "client/posix_wrapper.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <utime.h>

extern void InitPreload();
extern void DestroyPreload();
static mode_t gUmask;

__attribute__((constructor)) static void setup(void) {
  InitPreload();
  mode_t old = umask(0);
  umask(old);
  gUmask = old;
}

__attribute__((destructor)) static void teardown(void) { DestroyPreload(); }

// Operations:

// const char* path;
// mode_t mode;
// const void buffer[];
// off_t offset;
// Default type = int

// - [x] open(path, flags, [mode]) -> fd | -1
// - [x] lseek(fd, offset, whence) -> (off_t) offset_set | -1
// - [x] read(fd, buffer, count) -> (ssize_t) bytes_read | -1
// - [x] write(fd, buffer, count) -> (ssize_t) bytes_written | -1
// - [x] fsync(fd) -> 0 | -1
// - [x] fdatasync(fd) -> 0 | -1
// - [x] close(fd) -> 0 | -1
// - [ ] dup(oldfd) -> fd | -1
// - [ ] dup3(oldfd, newfd, flags) -> fd | -1
// - [x] fcntl(fd, cmd, ...) -> (int) ...

// - [x] @PLB: stat(path, buffer) -> 0 | -1
// - [x] @PLB: access(path, mode) -> 0 | -1
// - [x] @PLB: rename(oldpath, newpath) -> 0 | -1
// - [x] @PLB: unlink(path) -> 0 | -1
// - [x] @PLB: mkdir(path, mode) -> 0 | -1
// - [x] @PLB: rmdir(path) -> 0 | -1
// - [x] @PLB: opendir(path) -> (DIR *) dirptr | NULL
// - [x] @PLB: readdir(DIR *dirptr) -> (struct dirent *) direntptr | NULL
// - [x] @PLB: closedir(DIR *dirptr) -> 0 | -1

static inline int is_dfs_fd(int fd) { return fd >= kDfsMagicFdPrefix; }

static inline int is_mount_path(const char *path) {
  // do not support relative path
  if (strncmp(path, DFS_MOUNT_POINT, DFS_MNTPT_LEN) == 0) {
    return 1;
  }
  return 0;
}

static inline const char *get_dfs_path(const char *path) {
  // if the mount path is /dfs
  // input  : /dfs/text.txt
  // output : /text.txt
  // TODO: improve the robustness
  return path[DFS_MNTPT_LEN] ? path + DFS_MNTPT_LEN : "/";
}

static inline void assign_fnptr(void **fnptr, void *new_fnptr) {
  *fnptr = new_fnptr;
}

#define ASSIGN_FN(fn)                                                          \
  do {                                                                         \
    if (libc_##fn == NULL) {                                                   \
      assign_fnptr((void **)&libc_##fn, dlsym(RTLD_NEXT, #fn));                \
    }                                                                          \
  } while (0)

typedef int (*open_t)(const char *pathname, int flags, ...);
open_t libc_open;

int open(const char *file, int oflag, ...) {
  ASSIGN_FN(open);
  // printf("open called: file=%s, oflag=%o\n", file, oflag);
  va_list args;
  va_start(args, oflag);
  if (oflag & O_CREAT || oflag & O_TMPFILE) {
    mode_t mode = va_arg(args, mode_t);
    va_end(args);
    if (is_mount_path(file)) {
      mode = mode & ~gUmask;
      int ret = dfs_open(get_dfs_path(file), oflag, mode);
      // printf("open create return: ret=%d, mode=%o\n", ret, mode);
      return ret;
    }
    int ret = libc_open(file, oflag, mode);
    // printf("open create return: ret=%d\n", ret);
    return ret;
  }
  va_end(args);
  if (is_mount_path(file)) {
    int ret = dfs_open(get_dfs_path(file), oflag, 0644 & ~gUmask);
    // printf("open return: ret=%d\n", ret);
    return ret;
  }
  int ret = libc_open(file, oflag);
  // printf("open return: ret=%d\n", ret);
  return ret;
}

int __open_2(const char *file, int oflag) { return open(file, oflag); }

typedef int (*open64_t)(const char *file, int flags, ...);
open64_t libc_open64;

int open64(const char *file, int oflag, ...) {
  ASSIGN_FN(open64);
  // printf("open64 called: file=%s, oflag=%o\n", file, oflag);
  va_list args;
  va_start(args, oflag);
  if (oflag & O_CREAT || oflag & O_TMPFILE) {
    mode_t mode = va_arg(args, mode_t);
    va_end(args);
    if (is_mount_path(file)) {
      mode = mode & ~gUmask;
      int ret = dfs_open(get_dfs_path(file), oflag, mode);
      // printf("open64 create return: ret=%d, mode=%o\n", ret, mode);
      return ret;
    }
    int ret = libc_open64(file, oflag, mode);
    // printf("open64 create return: ret=%d\n", ret);
    return ret;
  }
  va_end(args);
  if (is_mount_path(file)) {
    int ret = dfs_open(get_dfs_path(file), oflag, 0644 & ~gUmask);
    // printf("open64 return: ret=%d\n", ret);
    return ret;
  }
  int ret = libc_open64(file, oflag);
  // printf("open64 return: ret=%d\n", ret);
  return ret;
}

typedef int (*openat_t)(int fd, const char *pathname, int flags, ...);
openat_t libc_openat;

int openat(int fd, const char *file, int oflag, ...) {
  ASSIGN_FN(openat);
  // printf("openat called: fd = %d, file=%s, oflag=%o\n", fd, file, oflag);
  va_list args;
  va_start(args, oflag);
  if (fd == AT_FDCWD) {
    if (oflag & O_CREAT || oflag & O_TMPFILE) {
      mode_t mode = va_arg(args, mode_t);
      va_end(args);
      if (is_mount_path(file)) {
        mode = mode & ~gUmask;
        int ret = dfs_open(get_dfs_path(file), oflag, mode);
        // printf("openat create return: ret=%d, mode=%o\n", ret, mode);
        return ret;
      }
      int ret = libc_openat(fd, file, oflag, mode);
      // printf("openat create return: ret=%d\n", ret);
      return ret;
    }
    va_end(args);
    if (is_mount_path(file)) {
      int ret = dfs_open(get_dfs_path(file), oflag, 0644 & ~gUmask);
      // printf("openat return: ret=%d\n", ret);
      return ret;
    }
    int ret = libc_openat(fd, file, oflag);
    // printf("openat return: ret=%d\n", ret);
    return ret;
  }

  // printf("openat unimplemented: fd = %d, file=%s, oflag=%o\n", fd, file,
  // oflag);
  return -1;
}

typedef int (*openat64_t)(int fd, const char *pathname, int flags, ...);
openat64_t libc_openat64;

int openat64(int fd, const char *file, int oflag, ...) {
  ASSIGN_FN(openat64);
  // printf("openat64 called: fd = %d, file=%s, oflag=%o\n", fd, file, oflag);
  va_list args;
  va_start(args, oflag);
  if (fd == AT_FDCWD) {
    if (oflag & O_CREAT || oflag & O_TMPFILE) {
      mode_t mode = va_arg(args, mode_t);
      va_end(args);
      if (is_mount_path(file)) {
        mode = mode & ~gUmask;
        int ret = dfs_open(get_dfs_path(file), oflag, mode);
        // printf("openat64 create return: ret=%d, mode=%o\n", ret, mode);
        return ret;
      }
      int ret = libc_openat64(fd, file, oflag, mode);
      // printf("openat64 create return: ret=%d\n", ret);
      return ret;
    }
    va_end(args);
    if (is_mount_path(file)) {
      int ret = dfs_open(get_dfs_path(file), oflag, 0644 & ~gUmask);
      // printf("openat64 return: ret=%d\n", ret);
      return ret;
    }
    int ret = libc_openat64(fd, file, oflag);
    // printf("openat64 return: ret=%d\n", ret);
    return ret;
  }

  // printf("openat64 unimplemented: fd = %d, file=%s, oflag=%o\n", fd, file,
  // oflag);
  return -1;
}

typedef FILE *(*fdopen_t)(int fd, const char *mode);
fdopen_t libc_fdopen;

FILE *fdopen(int fd, const char *modes) {
  ASSIGN_FN(fdopen);
  // printf("fdopen called: fd=%d\n", fd);
  if (is_dfs_fd(fd)) {
    return dfs_fdopen(fd, modes);
  }
  return libc_fdopen(fd, modes);
}

typedef FILE *(*fopen_t)(const char *file, const char *mode);
fopen_t libc_fopen;

FILE *fopen(const char *filename, const char *modes) {
  ASSIGN_FN(fopen);
  // printf("fopen called: file=%s\n", filename);
  if (is_mount_path(filename)) {
    return dfs_fopen(get_dfs_path(filename), modes);
  }
  return libc_fopen(filename, modes);
}

typedef int (*fileno_t)(FILE *__stream);
fileno_t libc_fileno;

int fileno(FILE *stream) {
  ASSIGN_FN(fileno);
  // printf("fileno called\n");
  int ret = dfs_fileno(stream);
  if (ret != -1) {
    // printf("fileno return: %d\n", ret);
    return ret;
  }
  ret = libc_fileno(stream);
  // printf("fileno return: %d\n", ret);
  return ret;
}

typedef FILE *(*fopen64_t)(const char *file, const char *mode);
fopen64_t libc_fopen64;

FILE *fopen64(const char *filename, const char *modes) {
  ASSIGN_FN(fopen64);
  // printf("fopen64 called: file=%s\n", filename);
  if (is_mount_path(filename)) {
    return dfs_fopen(get_dfs_path(filename), modes);
  }
  return libc_fopen64(filename, modes);
}

typedef int (*mkstemp_t)(char *template);
mkstemp_t libc_mkstemp;

int mkstemp(char *template) {
  ASSIGN_FN(mkstemp);
  // printf("mkstemp called: template=%s\n", template);
  if (is_mount_path(template)) {
    const char *orig = get_dfs_path(template);
    char *path = strdup(orig);
    return dfs_mkstemp(path);
  }
  return libc_mkstemp(template);
}

typedef int (*mkstemp64_t)(char *template);
mkstemp64_t libc_mkstemp64;

int mkstemp64(char *template) {
  ASSIGN_FN(mkstemp64);
  // printf("mkstemp64 called: template=%s\n", template);
  if (is_mount_path(template)) {
    const char *orig = get_dfs_path(template);
    char *path = strdup(orig);
    return dfs_mkstemp(path);
  }
  return libc_mkstemp64(template);
}

typedef __off_t (*lseek_t)(int fd, __off_t offset, int whence);
lseek_t libc_lseek;

__off_t lseek(int fd, __off_t offset, int whence) {
  ASSIGN_FN(lseek);
  // printf("lseek called: fd=%d, offset=%ld, whence=%d\n", fd, offset, whence);
  if (is_dfs_fd(fd)) {
    return dfs_lseek(fd, offset, whence);
  }
  return libc_lseek(fd, offset, whence);
}

typedef __off64_t (*lseek64_t)(int fd, __off64_t offset, int whence);
lseek64_t libc_lseek64;

__off64_t lseek64(int fd, __off64_t offset, int whence) {
  ASSIGN_FN(lseek64);
  // printf("lseek64 called: fd=%d\n", fd);
  if (is_dfs_fd(fd)) {
    return dfs_lseek(fd, offset, whence);
  }
  return libc_lseek64(fd, offset, whence);
}

typedef int (*stat_t)(const char *file, struct stat *buf);
stat_t libc_stat;

int stat(const char *file, struct stat *buf) {
  ASSIGN_FN(stat);
  // printf("stat called: path=%s\n", file);
  if (is_mount_path(file)) {
    return dfs_stat(get_dfs_path(file), buf);
  }
  return libc_stat(file, buf);
}

typedef int (*stat64_t)(const char *file, struct stat64 *buf);
stat64_t libc_stat64;

int stat64(const char *file, struct stat64 *buf) {
  ASSIGN_FN(stat64);
  // printf("stat64 called: path=%s\n", file);
  if (is_mount_path(file)) {
    // in 64-bit system stat and stat64 has the same shape.
    return dfs_stat(get_dfs_path(file), (struct stat *)buf);
  }
  return libc_stat64(file, buf);
}

typedef int (*utime_t)(const char *file, const struct utimbuf *file_times);
utime_t libc_utime;

int utime(const char *file, const struct utimbuf *file_times) {
  ASSIGN_FN(utime);
  // printf("utime called: file=%s\n", file);
  if (is_mount_path(file)) {
    // TODO(hyx): Used for git clone, currently not implemented, use Access
    // instead
    return dfs_access(get_dfs_path(file), 0);
  }
  return libc_utime(file, file_times);
}

typedef int (*fstat_t)(int fd, struct stat *buf);
fstat_t libc_fstat;

int fstat(int fd, struct stat *buf) {
  ASSIGN_FN(fstat);
  // printf("fstat called: fd=%d\n", fd);
  if (is_dfs_fd(fd)) {
    return dfs_fstat(fd, buf);
  }
  return libc_fstat(fd, buf);
}

typedef int (*fstat64_t)(int fd, struct stat64 *buf);
fstat64_t libc_fstat64;

int fstat64(int fd, struct stat64 *buf) {
  ASSIGN_FN(fstat64);
  // printf("fstat64 called: fd=%d\n", fd);
  if (is_dfs_fd(fd)) {
    return dfs_fstat(fd, (struct stat *)buf);
  }
  return libc_fstat64(fd, buf);
}

typedef int (*fstatat_t)(int fd, const char *file, struct stat *buf, int flag);
fstatat_t libc_fstatat;

int fstatat(int fd, const char *file, struct stat *buf, int flag) {
  ASSIGN_FN(fstatat);
  // printf("fstatat called: fd=%d, path=%s\n", fd, file);
  if (fd == AT_FDCWD) {
    if (is_mount_path(file)) {
      return dfs_stat(get_dfs_path(file), buf);
    };
    return libc_fstatat(fd, file, buf, flag);
  }
  if (is_mount_path(file)) {
    // printf("fstatat unimplemented: fd=%d, path=%s\n", fd, file);
    return -1;
  };
  return libc_fstatat(fd, file, buf, flag);
}

typedef int (*fstatat64_t)(int fd, const char *file, struct stat64 *buf,
                           int flag);
fstatat64_t libc_fstatat64;

int fstatat64(int fd, const char *file, struct stat64 *buf, int flag) {
  ASSIGN_FN(fstatat64);
  // printf("fstatat64 called: fd=%d, path=%s\n", fd, file);
  if (fd == AT_FDCWD) {
    if (is_mount_path(file)) {
      return dfs_stat(get_dfs_path(file), (struct stat *)buf);
    };
    return libc_fstatat64(fd, file, buf, flag);
  }
  if (is_mount_path(file)) {
    // printf("fstatat64 unimplemented: fd=%d, path=%s\n", fd, file);
    return -1;
  };
  return libc_fstatat64(fd, file, buf, flag);
}

typedef int (*lstat_t)(const char *file, struct stat *buf);
lstat_t libc_lstat;

int lstat(const char *file, struct stat *buf) {
  ASSIGN_FN(lstat);
  // printf("lstat called: path=%s\n", file);
  if (is_mount_path(file)) {
    return dfs_stat(get_dfs_path(file), buf);
  }
  return libc_lstat(file, buf);
}

typedef int (*lstat64_t)(const char *file, struct stat64 *buf);
lstat64_t libc_lstat64;

int lstat64(const char *file, struct stat64 *buf) {
  ASSIGN_FN(lstat64);
  // printf("lstat64 called: path=%s\n", file);
  if (is_mount_path(file)) {
    return dfs_stat(get_dfs_path(file), (struct stat *)buf);
  }
  return libc_lstat64(file, buf);
}

typedef int (*statx_t)(int dirfd, const char *restrict path, int flags,
                       unsigned int mask, struct statx *buf);
statx_t libc_statx;

int statx(int dirfd, const char *restrict path, int flags, unsigned int mask,
          struct statx *buf) {
  ASSIGN_FN(statx);
  // printf("statx called: path=%s\n", path);
  if (is_mount_path(path)) {
    return dfs_statx(dirfd, get_dfs_path(path), flags, mask, buf);
  }
  return libc_statx(dirfd, path, flags, mask, buf);
}

typedef int (*access_t)(const char *name, int type);
access_t libc_access;

int access(const char *name, int type) {
  ASSIGN_FN(access);
  // printf("access called: path=%s\n", name);
  int ret = 0;
  if (is_mount_path(name)) {
    ret = dfs_access(get_dfs_path(name), type);
    return ret;
  }
  ret = libc_access(name, type);
  return ret;
}

typedef int (*mknod_t)(const char *path, mode_t mode, dev_t dev);
mknod_t libc_mknod;

int mknod(const char *path, mode_t mode, dev_t dev) {
  ASSIGN_FN(mknod);
  // printf("mknod called: path=%s\n", path);
  if (is_mount_path(path)) {
    mode = mode & ~gUmask;
    return dfs_mknod(get_dfs_path(path), mode, dev);
  }
  return libc_mknod(path, mode, dev);
}

typedef int (*chmod_t)(const char *file, mode_t mode);
chmod_t libc_chmod;

int chmod(const char *file, mode_t mode) {
  ASSIGN_FN(chmod);
  // printf("chmod called: path=%s, mode=%o\n", file, mode);
  if (is_mount_path(file)) {
    mode = mode & ~gUmask;
    return dfs_chmod(get_dfs_path(file), mode);
  }
  return libc_chmod(file, mode);
}

typedef int (*creat_t)(const char *file, uint32_t mode);
creat_t libc_creat;

int creat(const char *file, uint32_t mode) {
  ASSIGN_FN(creat);
  // printf("creat called: path=%s\n", file);
  if (is_mount_path(file)) {
    return dfs_creat(get_dfs_path(file), mode);
  }
  return libc_creat(file, mode);
}

typedef int (*close_t)(int fd);
close_t libc_close;

int close(int fd) {
  ASSIGN_FN(close);
  // printf("close called: fd=%d\n", fd);
  if (is_dfs_fd(fd)) {
    return dfs_close(fd);
  }
  return libc_close(fd);
}

typedef int (*read_t)(int fd, void *buf, size_t count);
read_t libc_read;

int read(int fd, void *buf, size_t count) {
  ASSIGN_FN(read);
  // // printf("read called: fd=%d\n", fd);
  if (is_dfs_fd(fd)) {
    return dfs_read(fd, buf, count);
  }
  return libc_read(fd, buf, count);
}

typedef int (*write_t)(int fd, const void *buf, size_t count);
write_t libc_write;

int write(int fd, const void *buf, size_t count) {
  ASSIGN_FN(write);
  // // printf("write called: fd=%d, count=%zu\n", fd, count);
  // // printf("write buf = %s\n", (char *)buf);
  if (is_dfs_fd(fd)) {
    return dfs_write(fd, buf, count);
  }
  return libc_write(fd, buf, count);
}

// https://man7.org/linux/man-pages/man2/pread.2.html
// ssize_t pread(int fd, void *buf, size_t count, off_t offset);
typedef int64_t (*pread_t)(int fd, void *buf, uint64_t count, int64_t offset);
pread_t libc_pread;

int pread(int fd, void *buf, uint64_t count, int64_t offset) {
  ASSIGN_FN(pread);
  // // printf("pread called: fd=%d\n", fd);
  if (is_dfs_fd(fd)) {
    return dfs_pread(fd, buf, count, offset);
  }
  return libc_pread(fd, buf, count, offset);
}

// https://man7.org/linux/man-pages/man2/pread.2.html
// ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset);
typedef int64_t (*pwrite_t)(int fd, const void *buf, uint64_t count,
                            int64_t offset);
pwrite_t libc_pwrite;

int pwrite(int fd, const void *buf, uint64_t count, int64_t offset) {
  ASSIGN_FN(pwrite);
  // // printf("pwrite called: fd=%d\n", fd);
  if (is_dfs_fd(fd)) {
    return dfs_pwrite(fd, buf, count, offset);
  }
  return libc_pwrite(fd, buf, count, offset);
}

typedef void *(*mmap_t)(void *addr, size_t len, int prot, int flags, int fd,
                        __off_t offset);
mmap_t libc_mmap;

void *mmap(void *addr, size_t len, int prot, int flags, int fd,
           __off_t offset) {
  ASSIGN_FN(mmap);
  // printf("mmap called: fd=%d, flags=%o\n", fd, flags);
  if (is_dfs_fd(fd)) {
    return dfs_mmap(addr, len, prot, flags, fd, offset);
  }
  return libc_mmap(addr, len, prot, flags, fd, offset);
}

typedef void *(*mmap64_t)(void *addr, size_t length, int prot, int flags,
                          int fd, __off64_t offset);
static mmap64_t libc_mmap64;

void *mmap64(void *addr, size_t len, int prot, int flags, int fd,
             __off64_t offset) {
  ASSIGN_FN(mmap64);
  // printf("mmap64 called: fd=%d, flags=%o\n", fd, flags);
  if (is_dfs_fd(fd)) {
    return dfs_mmap(addr, len, prot, flags, fd, offset);
  }
  return libc_mmap64(addr, len, prot, flags, fd, offset);
}

typedef int (*munmap_t)(void *__addr, size_t __len);
munmap_t libc_munmap;

int munmap(void *addr, size_t len) {
  ASSIGN_FN(munmap);
  // printf("munmap called: len=%zu\n", len);
  int ret = dfs_munmap(addr, len);
  if (ret != -1) {
    return ret;
  }
  return libc_munmap(addr, len);
}

typedef int (*mkdir_t)(const char *path, mode_t mode);
mkdir_t libc_mkdir;

int mkdir(const char *path, mode_t mode) {
  ASSIGN_FN(mkdir);
  // printf("mkdir called: path=%s\n", path);
  if (is_mount_path(path)) {
    mode = mode & ~gUmask;
    return dfs_mkdir(get_dfs_path(path), mode);
  }
  return libc_mkdir(path, mode);
}

typedef int (*mkdirat_t)(int fd, const char *path, __mode_t mode);
mkdirat_t libc_mkdirat;

int mkdirat(int fd, const char *path, __mode_t mode) {
  ASSIGN_FN(mkdirat);
  // printf("mkdirat called: fd = %d, path=%s\n", fd, path);
  if (fd == AT_FDCWD) {
    if (is_mount_path(path)) {
      mode = mode & ~gUmask;
      return dfs_mkdir(get_dfs_path(path), mode);
    };
    return libc_mkdirat(fd, path, mode);
  }
  if (is_mount_path(path)) {
    // printf("mkdirat unimplemented: fd=%d, path=%s\n", fd, path);
    return -1;
  };
  return libc_mkdirat(fd, path, mode);
}

typedef int (*rmdir_t)(const char *path);
rmdir_t libc_rmdir;

int rmdir(const char *path) {
  ASSIGN_FN(rmdir);
  // printf("rmdir called: path=%s\n", path);
  if (is_mount_path(path)) {
    return dfs_rmdir(get_dfs_path(path));
  }
  return libc_rmdir(path);
}

typedef int (*unlink_t)(const char *name);
unlink_t libc_unlink;

int unlink(const char *name) {
  ASSIGN_FN(unlink);
  // printf("unlink called: path=%s\n", name);
  if (is_mount_path(name)) {
    return dfs_unlink(get_dfs_path(name));
  }
  return libc_unlink(name);
}

typedef int (*link_t)(const char *from, const char *to);
link_t libc_link;

int link(const char *from, const char *to) {
  ASSIGN_FN(link);
  // printf("link called: path=%s, path=%s\n", from, to);
  if (is_mount_path(from) && is_mount_path(to)) {
    return dfs_link(get_dfs_path(from), get_dfs_path(to));
  }
  return libc_link(from, to);
}

typedef int (*symlinkat_t)(const char *from, int tofd, const char *to);
symlinkat_t libc_symlinkat;

int symlinkat(const char *from, int tofd, const char *to) {
  ASSIGN_FN(symlinkat);
  // printf("symlinkat called: from=%s, tofd=%d, to=%s\n", from, tofd, to);
  if (tofd == AT_FDCWD) {
    if (is_mount_path(to)) {
      return dfs_symlink(from, get_dfs_path(to));
    };
    return libc_symlinkat(from, tofd, to);
  }
  if (is_mount_path(to)) {
    // printf("symlinkat unimplemented: fd=%d, path=%s\n", tofd, to);
    return -1;
  };
  return libc_symlinkat(from, tofd, to);
}

typedef int (*rename_t)(const char *old, const char *new);
rename_t libc_rename;

int rename(const char *old, const char *new) {
  ASSIGN_FN(rename);
  // printf("rename called: path=%s, path=%s\n", old, new);
  if (is_mount_path(old) && is_mount_path(new)) {
    return dfs_rename(get_dfs_path(old), get_dfs_path(new));
  }
  if (is_mount_path(old) || is_mount_path(new)) {
    errno = EXDEV;
    return -1;
  }
  return libc_rename(old, new);
}

typedef int (*fsync_t)(int fd);
fsync_t libc_fsync;

int fsync(int fd) {
  ASSIGN_FN(fsync);
  // printf("fsync called: fd=%d\n", fd);
  if (is_dfs_fd(fd)) {
    return dfs_fsync(fd);
  }
  return libc_fsync(fd);
}

typedef int (*fdatasync_t)(int fildes);
fdatasync_t libc_fdatasync;

int fdatasync(int fildes) {
  ASSIGN_FN(fdatasync);
  // printf("fdatasync called: fd=%d\n", fildes);
  if (is_dfs_fd(fildes)) {
    return dfs_fdatasync(fildes);
  }
  return libc_fdatasync(fildes);
}

typedef int (*statfs_t)(const char *file, struct statfs *buf);
statfs_t libc_statfs;

int statfs(const char *file, struct statfs *buf) {
  ASSIGN_FN(statfs);
  // printf("statfs called: path=%s\n", file);
  if (is_mount_path(file)) {
    return dfs_statfs(get_dfs_path(file), buf);
  }
  return libc_statfs(file, buf);
}

typedef int (*statvfs_t)(const char *file, struct statvfs *buf);
statvfs_t libc_statvfs;

int statvfs(const char *file, struct statvfs *buf) {
  ASSIGN_FN(statvfs);
  // printf("statvfs called: path=%s\n", file);
  if (is_mount_path(file)) {
    return dfs_statvfs(get_dfs_path(file), buf);
  }
  return libc_statvfs(file, buf);
}

typedef DIR *(*opendir_t)(const char *name);
opendir_t libc_opendir;

DIR *opendir(const char *name) {
  ASSIGN_FN(opendir);
  // printf("opendir called: path=%s\n", name);
  if (is_mount_path(name)) {
    return dfs_opendir(get_dfs_path(name), DIR_MAX_ALLOC);
  }
  return libc_opendir(name);
}

typedef struct dirent *(*readdir_t)(DIR *dirp);
readdir_t libc_readdir;

struct dirent *readdir(DIR *dirp) {
  ASSIGN_FN(readdir);
  // printf("readdir called\n");
  if (is_dfs_dirstream(dirp)) {
    return dfs_readdir(dirp);
  }
  return libc_readdir(dirp);
}

// #ifdef __USE_LARGEFILE64
typedef struct dirent64 *(*readdir64_t)(DIR *dirp);
readdir64_t libc_readdir64;

struct dirent64 *readdir64(DIR *dirp) {
  ASSIGN_FN(readdir64);
  // printf("readdir64 called\n");
  if (is_dfs_dirstream(dirp)) {
    return (struct dirent64 *)dfs_readdir(dirp);
  }
  return libc_readdir64(dirp);
}

// #endif

typedef int (*closedir_t)(DIR *dirp);
closedir_t libc_closedir;

int closedir(DIR *dirp) {
  ASSIGN_FN(closedir);
  // printf("closedir called\n");
  if (is_dfs_dirstream(dirp)) {
    return dfs_closedir(dirp);
  }
  return libc_closedir(dirp);
}

typedef int (*orig_fcntl_t)(int, int, ...);
orig_fcntl_t libc_fcntl;

int fcntl(int fd, int cmd, ...) {
  ASSIGN_FN(fcntl);
  // printf("fcntl called: fd=%d, cmd=%d\n", fd, cmd);
  va_list args;
  va_start(args, cmd);
  int result = -1;

  if (is_dfs_fd(fd)) {
    result = dfs_fcntl(fd, cmd, args);
  } else {
    switch (cmd) {
    case F_GETFL:
    case F_GETFD:
      result = libc_fcntl(fd, cmd);
      break;
    case F_SETFL:
    case F_SETFD: {
      int flag = va_arg(args, int);
      result = libc_fcntl(fd, cmd, flag);
      break;
    }
    case F_SETLK:
    case F_SETLKW: {
      struct flock *lock = va_arg(args, struct flock *);
      result = libc_fcntl(fd, cmd, lock);
      break;
    }
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
      long arg = va_arg(args, long);
      result = libc_fcntl(fd, cmd, arg);
      break;
    }
    default: {
      long arg = va_arg(args, long);
      result = libc_fcntl(fd, cmd, arg);
      break;
    }
    }
  }

  va_end(args);
  return result;
}
