/* POSIX compatibility layer for the native Windows (aarch64-pc-windows-msvc) build.
 *
 * The runtime is written against POSIX/glibc. On Windows the shim headers in
 * runtime/win/include (unistd.h, pthread.h, dirent.h, ...) all pull in this
 * file, so the existing #include lines compile unchanged. It deliberately
 * does NOT include <windows.h>: the guest-facing shims (kernel32.c etc.)
 * define their own Win32 names and constants. Only the C files in runtime/win
 * include <windows.h>; they define POSIX_COMPAT_IMPL to skip the renaming macros. */
#pragma once
#ifdef _WIN32

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <io.h>
#include <direct.h>
#include <fcntl.h>
#include <process.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef intptr_t ssize_t;
typedef uint32_t useconds_t;
typedef int clockid_t;

/* ---------------------------------------------------------------- strings */
static inline int strcasecmp(const char *a, const char *b) { return _stricmp(a, b); }
static inline int strncasecmp(const char *a, const char *b, size_t n) { return _strnicmp(a, b, n); }

/* ---------------------------------------------------------------- env */
static inline int setenv(const char *k, const char *v, int overwrite) {
    if (!overwrite && getenv(k)) return 0;
    return _putenv_s(k, v) == 0 ? 0 : -1;
}

/* ---------------------------------------------------------------- time */
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
struct timeval { long tv_sec; long tv_usec; };  /* WIN32_LEAN_AND_MEAN keeps winsock's out */
int clock_gettime(clockid_t clk, struct timespec *ts);
int gettimeofday(struct timeval *tv, void *tz);
int usleep(useconds_t us);
int sched_yield(void);
/* Proleptic-Gregorian, so FILETIMEs before 1970 (down to 1601) work; UCRT's gmtime_s/_mkgmtime don't. */
struct tm *gmtime_r(const time_t *t, struct tm *out);
time_t timegm(struct tm *tm);
static inline struct tm *localtime_r(const time_t *t, struct tm *out) { return localtime_s(out, t) ? NULL : out; }

/* ---------------------------------------------------------------- files */
#ifndef O_ACCMODE
#define O_ACCMODE (O_RDONLY | O_WRONLY | O_RDWR)
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0
#endif
#define F_OK 0
#define W_OK 2
#define R_OK 4
#define X_OK 0
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif

/* POSIX-shaped stat: 64-bit size/times plus st_mtim (used by the dir cache). */
struct compat_stat {
    uint32_t st_dev;          /* volume serial (fstat only) */
    uint64_t st_ino;          /* NTFS file index (fstat only; 0 from stat) */
    unsigned short st_mode;
    short st_nlink, st_uid, st_gid;
    uint32_t st_rdev;
    int64_t st_size;
    int64_t st_atime, st_mtime, st_ctime;
    struct timespec st_mtim;
};
int compat_stat(const char *path, struct compat_stat *st);
int compat_fstat(int fd, struct compat_stat *st);
int compat_open(const char *path, int flags, ...);   /* always _O_BINARY; shares read/write/delete */
int compat_read(int fd, void *buf, unsigned n);      /* these three pre-commit lazily mapped buffers: */
int compat_write(int fd, const void *buf, unsigned n); /* kernel I/O into uncommitted pages fails */
ssize_t compat_pread(int fd, void *buf, size_t n, int64_t off);
int compat_ftruncate(int fd, int64_t len);
int compat_rename(const char *from, const char *to); /* replaces an existing target, like POSIX */
int compat_mkdir(const char *path);

/* ---------------------------------------------------------------- dirent */
#define DT_UNKNOWN 0
#define DT_DIR 4
#define DT_REG 8
struct dirent { unsigned char d_type; char d_name[260]; };
typedef struct compat_DIR DIR;
DIR *opendir(const char *path);
struct dirent *readdir(DIR *d);
int closedir(DIR *d);

/* ---------------------------------------------------------------- fnmatch */
#define FNM_NOMATCH 1
#define FNM_CASEFOLD 0x10
int fnmatch(const char *pat, const char *name, int flags);

/* ---------------------------------------------------------------- mman */
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_NORESERVE 0x4000
#define MAP_FAILED ((void *)-1)
#define MADV_DONTNEED 4
/* Anonymous mappings only. If committing the whole range fails (commit limit),
 * the range is reserved and committed lazily on first touch. */
void *mmap(void *addr, size_t len, int prot, int flags, int fd, int64_t off);
int munmap(void *addr, size_t len);
int mprotect(void *addr, size_t len, int prot);
int madvise(void *addr, size_t len, int advice); /* DONTNEED: pages read back as zero */

/* ---------------------------------------------------------------- pthreads */
/* All statically initializable (SRWLOCK / CONDITION_VARIABLE are one pointer). */
typedef struct { void *srw; unsigned long owner; int depth; int recursive; } pthread_mutex_t;
typedef struct { int type; } pthread_mutexattr_t;
typedef struct { void *cv; } pthread_cond_t;
typedef struct { void *srw; unsigned long writer; } pthread_rwlock_t;
typedef struct { int detached; size_t stacksize; } pthread_attr_t;
typedef uintptr_t pthread_t;
#define PTHREAD_MUTEX_INITIALIZER {0, 0, 0, 0}
#define PTHREAD_COND_INITIALIZER {0}
#define PTHREAD_RWLOCK_INITIALIZER {0, 0}
#define PTHREAD_MUTEX_NORMAL 0
#define PTHREAD_MUTEX_RECURSIVE 1
#define PTHREAD_MUTEX_DEFAULT PTHREAD_MUTEX_NORMAL
#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1

int pthread_mutexattr_init(pthread_mutexattr_t *a);
int pthread_mutexattr_settype(pthread_mutexattr_t *a, int type);
int pthread_mutexattr_destroy(pthread_mutexattr_t *a);
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);
int pthread_cond_init(pthread_cond_t *c, const void *attr);
int pthread_cond_destroy(pthread_cond_t *c);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abstime);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);
int pthread_rwlock_rdlock(pthread_rwlock_t *l);
int pthread_rwlock_wrlock(pthread_rwlock_t *l);
int pthread_rwlock_unlock(pthread_rwlock_t *l);
int pthread_attr_init(pthread_attr_t *a);
int pthread_attr_destroy(pthread_attr_t *a);
int pthread_attr_setdetachstate(pthread_attr_t *a, int state);
int pthread_attr_setstacksize(pthread_attr_t *a, size_t size);
int pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg);
int pthread_detach(pthread_t t);
int pthread_join(pthread_t t, void **ret);
pthread_t pthread_self(void);
__declspec(noreturn) void pthread_exit(void *ret);

/* ---------------------------------------------------------------- platform */
void win_install_crash_handler(void);   /* also: 1 ms timer resolution, main-thread stack guarantee */
void win_thread_stack_guarantee(void);  /* per thread: room for the crash report after a stack overflow */
/* Defaults for a double-clicked exe: game/save dirs from lb_paths.ini next to the exe
 * (section [paths], keys game_dir / save_dir, %VARS% expanded). Leaves them unchanged
 * when there is no ini. Command-line arguments are applied afterwards and win. */
void win_default_paths(char *game, size_t game_sz, char *save, size_t save_sz);
/* GUI subsystem: with no usable stderr, log to <save_dir>/log.txt (creates the dir). */
void win_setup_log(const char *save_dir);
void win_fatal_box(const char *title, const char *msg);
/* Reads [section] key from lb_paths.ini next to the exe (%VARS% expanded).
 * Returns 1 and fills out when the key exists and is non-empty, else 0. */
int win_ini_get(const char *section, const char *key, char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif

/* Redirect POSIX names to the compat versions (skipped in the runtime/win C files). */
#ifndef POSIX_COMPAT_IMPL
#define stat compat_stat        /* both `struct stat` and stat() */
#define fstat compat_fstat
#define mkdir(p, mode) compat_mkdir(p)
#ifndef __cplusplus             /* C++ std headers use these names as members */
#define open compat_open
#define read compat_read
#define write compat_write
#define pread compat_pread
#define lseek _lseeki64
#define off_t int64_t
#define ftruncate compat_ftruncate
#define rename compat_rename
#endif
#endif

#endif /* _WIN32 */
