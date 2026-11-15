/* Win32 implementation of runtime/win/posix_compat.h. */
#define POSIX_COMPAT_IMPL
#include "posix_compat.h"
#include <windows.h>
#include <stdarg.h>

/* ================================================================ time */
static int64_t qpc_freq(void) {
    static int64_t f;
    if (!f) { LARGE_INTEGER li; QueryPerformanceFrequency(&li); f = li.QuadPart; }
    return f;
}

int clock_gettime(clockid_t clk, struct timespec *ts) {
    if (clk == CLOCK_MONOTONIC) {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        int64_t f = qpc_freq();
        ts->tv_sec = (time_t)(c.QuadPart / f);
        ts->tv_nsec = (long)((c.QuadPart % f) * 1000000000ll / f);
    } else {
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);
        uint64_t t = ((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - 116444736000000000ull;
        ts->tv_sec = (time_t)(t / 10000000ull);
        ts->tv_nsec = (long)(t % 10000000ull) * 100;
    }
    return 0;
}

int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    tv->tv_sec = (long)ts.tv_sec;
    tv->tv_usec = ts.tv_nsec / 1000;
    return 0;
}

/* Civil-date conversions (H. Hinnant's algorithms): valid for any year, so a
 * zero FILETIME (1601-01-01) converts correctly. */
static int64_t days_from_civil(int64_t y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

struct tm *gmtime_r(const time_t *tp, struct tm *out) {
    int64_t t = (int64_t)*tp;
    int64_t days = t >= 0 ? t / 86400 : -((-t + 86399) / 86400);
    int64_t sec = t - days * 86400;
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int d = (int)(doy - (153 * mp + 2) / 5 + 1);
    int m = (int)(mp < 10 ? mp + 3 : mp - 9);
    int64_t y = yoe + era * 400 + (m <= 2);
    memset(out, 0, sizeof *out);
    out->tm_year = (int)(y - 1900);
    out->tm_mon = m - 1;
    out->tm_mday = d;
    out->tm_hour = (int)(sec / 3600);
    out->tm_min = (int)(sec / 60 % 60);
    out->tm_sec = (int)(sec % 60);
    out->tm_wday = (int)(((days % 7) + 11) % 7); /* 1970-01-01 was a Thursday */
    out->tm_yday = (int)(days - days_from_civil(y, 1, 1));
    return out;
}

time_t timegm(struct tm *tm) {
    int64_t mon = tm->tm_mon, y = (int64_t)tm->tm_year + 1900;
    y += mon >= 0 ? mon / 12 : -((11 - mon) / 12);
    mon -= (mon >= 0 ? mon / 12 : -((11 - mon) / 12)) * 12;
    int64_t days = days_from_civil(y, (int)mon + 1, 1) + tm->tm_mday - 1;
    return (time_t)(days * 86400 + (int64_t)tm->tm_hour * 3600 + (int64_t)tm->tm_min * 60 + tm->tm_sec);
}

/* Sleep() has ~15.6 ms granularity; the XAudio2 mixer paces itself with
 * usleep(1000), so use a per-thread high-resolution waitable timer. It is
 * closed by thread_cleanup() when a pthread_create'd thread ends. */
static __thread HANDLE t_timer;

static void thread_cleanup(void) {
    if (t_timer) { CloseHandle(t_timer); t_timer = NULL; }
}

int usleep(useconds_t us) {
    if (us == 0) { SwitchToThread(); return 0; }
    if (!t_timer) {
        t_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!t_timer) t_timer = CreateWaitableTimerW(NULL, TRUE, NULL);
    }
    if (t_timer) {
        LARGE_INTEGER due;
        due.QuadPart = -(int64_t)us * 10; /* relative, 100 ns units */
        if (SetWaitableTimer(t_timer, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(t_timer, INFINITE);
            return 0;
        }
    }
    Sleep((us + 999) / 1000);
    return 0;
}

int sched_yield(void) { SwitchToThread(); return 0; }

/* ================================================================ files */
static void lazy_precommit(const void *p, size_t n); /* mman section below */

static int64_t filetime_to_unix100(const FILETIME *ft) {
    return (int64_t)(((uint64_t)ft->dwHighDateTime << 32 | ft->dwLowDateTime) - 116444736000000000ull);
}

static void fill_stat(struct compat_stat *st, DWORD attrs, uint64_t size, const FILETIME *a, const FILETIME *m,
                      const FILETIME *c) {
    memset(st, 0, sizeof *st);
    st->st_mode = (attrs & FILE_ATTRIBUTE_DIRECTORY) ? (_S_IFDIR | 0755) : (_S_IFREG | 0644);
    if (attrs & FILE_ATTRIBUTE_READONLY) st->st_mode &= ~0222;
    st->st_nlink = 1;
    st->st_size = (int64_t)size;
    int64_t mt = filetime_to_unix100(m);
    st->st_atime = filetime_to_unix100(a) / 10000000;
    st->st_mtime = mt / 10000000;
    st->st_ctime = filetime_to_unix100(c) / 10000000;
    st->st_mtim.tv_sec = (time_t)(mt / 10000000);
    st->st_mtim.tv_nsec = (long)(mt % 10000000) * 100;
}

static int set_errno_from_win(void) {
    switch (GetLastError()) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_NAME: errno = ENOENT; break;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: errno = EACCES; break;
    case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: errno = EEXIST; break;
    case ERROR_DIR_NOT_EMPTY: errno = ENOTEMPTY; break;
    case ERROR_LOCK_VIOLATION: errno = EACCES; break;
    case ERROR_NOACCESS: errno = EFAULT; break;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: errno = ENOSPC; break;
    default: errno = EIO; break;
    }
    return -1;
}

int compat_stat(const char *path, struct compat_stat *st) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &d)) return set_errno_from_win();
    fill_stat(st, d.dwFileAttributes, (uint64_t)d.nFileSizeHigh << 32 | d.nFileSizeLow, &d.ftLastAccessTime,
              &d.ftLastWriteTime, &d.ftCreationTime);
    return 0;
}

int compat_fstat(int fd, struct compat_stat *st) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    BY_HANDLE_FILE_INFORMATION i;
    if (h == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(h, &i)) { errno = EBADF; return -1; }
    fill_stat(st, i.dwFileAttributes, (uint64_t)i.nFileSizeHigh << 32 | i.nFileSizeLow, &i.ftLastAccessTime,
              &i.ftLastWriteTime, &i.ftCreationTime);
    st->st_dev = i.dwVolumeSerialNumber;
    st->st_ino = (uint64_t)i.nFileIndexHigh << 32 | i.nFileIndexLow;
    return 0;
}

int compat_open(const char *path, int flags, ...) {
    /* CreateFileA rather than _open: share delete too, so the guest can
     * rename/delete files that are still open (as on Linux). Always binary:
     * text mode would translate CR/LF and stop at ^Z. */
    int acc = flags & O_ACCMODE;
    DWORD access = acc == O_RDONLY ? GENERIC_READ : acc == O_WRONLY ? GENERIC_WRITE : GENERIC_READ | GENERIC_WRITE;
    DWORD disp;
    if ((flags & O_CREAT) && (flags & O_EXCL)) disp = CREATE_NEW;
    else if ((flags & O_CREAT) && (flags & O_TRUNC)) disp = CREATE_ALWAYS;
    else if (flags & O_CREAT) disp = OPEN_ALWAYS;
    else if (flags & O_TRUNC) disp = TRUNCATE_EXISTING;
    else disp = OPEN_EXISTING;
    HANDLE h = CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, disp,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return set_errno_from_win();
    int cf = _O_BINARY | (acc == O_RDONLY ? _O_RDONLY : 0) | ((flags & O_APPEND) ? _O_APPEND : 0);
    int fd = _open_osfhandle((intptr_t)h, cf);
    if (fd < 0) CloseHandle(h);
    return fd;
}

int compat_read(int fd, void *buf, unsigned n) {
    lazy_precommit(buf, n);
    return _read(fd, buf, n);
}

int compat_write(int fd, const void *buf, unsigned n) {
    lazy_precommit(buf, n);
    return _write(fd, buf, n);
}

/* ReadFile with an OVERLAPPED offset on a synchronous handle moves the file
 * pointer; pread must not, so save and restore it, under a lock so two
 * concurrent preads can't restore each other's stale position. */
static SRWLOCK g_pread_lock = SRWLOCK_INIT;

ssize_t compat_pread(int fd, void *buf, size_t n, int64_t off) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    lazy_precommit(buf, n);
    AcquireSRWLockExclusive(&g_pread_lock);
    LARGE_INTEGER zero = {0}, pos;
    SetFilePointerEx(h, zero, &pos, FILE_CURRENT);
    OVERLAPPED ov = {0};
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)((uint64_t)off >> 32);
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, (DWORD)(n > 0xFFFFFFFFu ? 0xFFFFFFFFu : n), &got, &ov);
    DWORD err = ok ? 0 : GetLastError();
    SetFilePointerEx(h, pos, NULL, FILE_BEGIN);
    ReleaseSRWLockExclusive(&g_pread_lock);
    if (!ok && err != ERROR_HANDLE_EOF) { SetLastError(err); return set_errno_from_win(); }
    return (ssize_t)got;
}

int compat_ftruncate(int fd, int64_t len) { return _chsize_s(fd, len) == 0 ? 0 : -1; }

int compat_rename(const char *from, const char *to) {
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) ? 0 : set_errno_from_win();
}

int compat_mkdir(const char *path) { return _mkdir(path); }

/* ================================================================ dirent */
struct compat_DIR { HANDLE h; int first; WIN32_FIND_DATAA fd; struct dirent ent; };

DIR *opendir(const char *path) {
    char pat[MAX_PATH + 3];
    size_t n = strlen(path);
    if (n + 3 > sizeof pat) { errno = ENAMETOOLONG; return NULL; }
    memcpy(pat, path, n);
    if (n && path[n - 1] != '/' && path[n - 1] != '\\') pat[n++] = '/';
    pat[n++] = '*';
    pat[n] = 0;
    DIR *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->h = FindFirstFileA(pat, &d->fd);
    if (d->h == INVALID_HANDLE_VALUE) { free(d); set_errno_from_win(); return NULL; }
    d->first = 1;
    return d;
}

struct dirent *readdir(DIR *d) {
    if (!d->first && !FindNextFileA(d->h, &d->fd)) return NULL;
    d->first = 0;
    d->ent.d_type = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? DT_DIR : DT_REG;
    snprintf(d->ent.d_name, sizeof d->ent.d_name, "%s", d->fd.cFileName);
    return &d->ent;
}

int closedir(DIR *d) {
    if (!d) return -1;
    FindClose(d->h);
    free(d);
    return 0;
}

/* ================================================================ fnmatch */
static int fold(int c, int ci) { return ci && c >= 'A' && c <= 'Z' ? c + 32 : c; }

int fnmatch(const char *p, const char *s, int flags) {
    int ci = flags & FNM_CASEFOLD;
    const char *star = NULL, *ss = NULL;
    while (*s) {
        if (*p == '*') { star = p++; ss = s; continue; }
        if (*p == '?' || (*p && fold((unsigned char)*p, ci) == fold((unsigned char)*s, ci))) { p++; s++; continue; }
        if (star) { p = star + 1; s = ++ss; continue; }
        return FNM_NOMATCH;
    }
    while (*p == '*') p++;
    return *p ? FNM_NOMATCH : 0;
}

/* ================================================================ mman */
/* Lazily committed regions (used only if committing the whole guest space
 * up front fails): a vectored handler commits 64 KiB on first touch, except
 * inside PROT_NONE guard ranges. */
#define MAX_LAZY 4
#define MAX_GUARD 16
static struct { uint8_t *base; size_t len; DWORD prot; } g_lazy[MAX_LAZY];
static struct { uint8_t *base; size_t len; } g_guard[MAX_GUARD];
static volatile LONG g_nlazy, g_nguard;
static PVOID g_veh;

static DWORD win_prot(int prot) {
    if (prot & PROT_EXEC) return (prot & PROT_WRITE) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    if (prot & PROT_WRITE) return PAGE_READWRITE;
    if (prot & PROT_READ) return PAGE_READONLY;
    return PAGE_NOACCESS;
}

static int lazy_index(const uint8_t *p) {
    for (LONG i = 0; i < g_nlazy; i++)
        if (p >= g_lazy[i].base && p < g_lazy[i].base + g_lazy[i].len) return (int)i;
    return -1;
}

static LONG CALLBACK lazy_commit_handler(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    uint8_t *a = (uint8_t *)ep->ExceptionRecord->ExceptionInformation[1];
    int i = lazy_index(a);
    if (i < 0) return EXCEPTION_CONTINUE_SEARCH;
    for (LONG g = 0; g < g_nguard; g++)
        if (a >= g_guard[g].base && a < g_guard[g].base + g_guard[g].len) return EXCEPTION_CONTINUE_SEARCH;
    uint8_t *page = (uint8_t *)((uintptr_t)a & ~(uintptr_t)0xFFFF);
    if (page < g_lazy[i].base) page = g_lazy[i].base;
    if (!VirtualAlloc(page, 0x10000, MEM_COMMIT, g_lazy[i].prot)) return EXCEPTION_CONTINUE_SEARCH;
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* The vectored handler only sees user-mode faults: ReadFile/WriteFile into
 * reserved-but-uncommitted pages fail in the kernel (ERROR_NOACCESS) without
 * an exception. So file I/O commits the buffer's chunks up front. */
static void lazy_precommit(const void *p, size_t n) {
    if (!g_nlazy || !n) return;
    int i = lazy_index(p);
    if (i < 0) return;
    uint8_t *s = (uint8_t *)((uintptr_t)p & ~(uintptr_t)0xFFFF);
    uint8_t *e = (uint8_t *)p + n, *end = g_lazy[i].base + g_lazy[i].len;
    if (s < g_lazy[i].base) s = g_lazy[i].base;
    if (e > end) e = end;
    for (; s < e; s += 0x10000) {
        int guarded = 0;
        for (LONG g = 0; g < g_nguard; g++)
            if (s >= g_guard[g].base && s < g_guard[g].base + g_guard[g].len) guarded = 1;
        if (!guarded) VirtualAlloc(s, 0x10000, MEM_COMMIT, g_lazy[i].prot);
    }
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, int64_t off) {
    (void)off;
    if (fd != -1 || !(flags & MAP_ANONYMOUS)) { errno = ENOSYS; return MAP_FAILED; }
    DWORD wp = win_prot(prot);
    /* LB_LAZY_COMMIT=1 forces the on-demand path (to test low-commit behaviour). */
    const char *lazy = getenv("LB_LAZY_COMMIT");
    void *p = (lazy && *lazy == '1' && (flags & MAP_NORESERVE)) ? NULL
              : VirtualAlloc(addr, len, MEM_RESERVE | MEM_COMMIT, wp);
    if (p) return p;
    if (!(flags & MAP_NORESERVE) || g_nlazy >= MAX_LAZY) { errno = ENOMEM; return MAP_FAILED; }
    p = VirtualAlloc(addr, len, MEM_RESERVE, PAGE_NOACCESS);
    if (!p) { errno = ENOMEM; return MAP_FAILED; }
    LONG i = g_nlazy;
    g_lazy[i].base = p;
    g_lazy[i].len = len;
    g_lazy[i].prot = wp;
    InterlockedIncrement(&g_nlazy);
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, lazy_commit_handler);
    fprintf(stderr, "mmap: could not commit %zu MiB up front; committing on demand\n", len >> 20);
    return p;
}

int munmap(void *addr, size_t len) {
    (void)len;
    return VirtualFree(addr, 0, MEM_RELEASE) ? 0 : -1;
}

int mprotect(void *addr, size_t len, int prot) {
    if (lazy_index(addr) >= 0 && prot == PROT_NONE && g_nguard < MAX_GUARD) {
        LONG g = g_nguard;
        g_guard[g].base = addr;
        g_guard[g].len = len;
        InterlockedIncrement(&g_nguard);
        VirtualFree(addr, len, MEM_DECOMMIT);
        return 0;
    }
    DWORD old;
    return VirtualProtect(addr, len, win_prot(prot), &old) ? 0 : -1;
}

int madvise(void *addr, size_t len, int advice) {
    if (advice != MADV_DONTNEED) return 0;
    /* Decommit drops the pages; recommitting gives fresh zero pages
     * (MEM_RESET would not guarantee zeros). Lazy regions recommit on touch. */
    if (!VirtualFree(addr, len, MEM_DECOMMIT)) return -1;
    if (lazy_index(addr) >= 0) return 0;
    if (VirtualAlloc(addr, len, MEM_COMMIT, PAGE_READWRITE)) return 0;
    /* Recommit failed (commit limit filled up in between). Retry chunk by
     * chunk; the caller hands this range out again as zeroed memory, so a
     * hole here would crash later in guest code: fail loudly instead. */
    for (size_t o = 0; o < len; o += 0x10000) {
        size_t c = len - o < 0x10000 ? len - o : 0x10000;
        for (int tries = 0; !VirtualAlloc((uint8_t *)addr + o, c, MEM_COMMIT, PAGE_READWRITE); tries++) {
            if (tries == 50) {
                fprintf(stderr, "FATAL: madvise: cannot recommit %zu bytes at %p (commit limit)\n", len, addr);
                abort();
            }
            Sleep(20);
        }
    }
    return 0;
}

/* ================================================================ pthreads */
#define SRW(p) ((PSRWLOCK)&(p)->srw)
#define CV(p) ((PCONDITION_VARIABLE)&(p)->cv)

int pthread_mutexattr_init(pthread_mutexattr_t *a) { a->type = PTHREAD_MUTEX_NORMAL; return 0; }
int pthread_mutexattr_settype(pthread_mutexattr_t *a, int type) { a->type = type; return 0; }
int pthread_mutexattr_destroy(pthread_mutexattr_t *a) { (void)a; return 0; }

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) {
    memset(m, 0, sizeof *m);
    m->recursive = a && a->type == PTHREAD_MUTEX_RECURSIVE;
    return 0;
}
int pthread_mutex_destroy(pthread_mutex_t *m) { (void)m; return 0; }

int pthread_mutex_lock(pthread_mutex_t *m) {
    DWORD self = GetCurrentThreadId();
    if (m->recursive && m->owner == self) { m->depth++; return 0; }
    AcquireSRWLockExclusive(SRW(m));
    m->owner = self;
    m->depth = 1;
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *m) {
    DWORD self = GetCurrentThreadId();
    if (m->recursive && m->owner == self) { m->depth++; return 0; }
    if (!TryAcquireSRWLockExclusive(SRW(m))) return EBUSY;
    m->owner = self;
    m->depth = 1;
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *m) {
    if (--m->depth > 0) return 0;
    m->owner = 0;
    ReleaseSRWLockExclusive(SRW(m));
    return 0;
}

int pthread_cond_init(pthread_cond_t *c, const void *attr) { (void)attr; c->cv = NULL; return 0; }
int pthread_cond_destroy(pthread_cond_t *c) { (void)c; return 0; }

static int cond_wait_ms(pthread_cond_t *c, pthread_mutex_t *m, DWORD ms) {
    int depth = m->depth;
    m->owner = 0;
    m->depth = 0;
    BOOL ok = SleepConditionVariableSRW(CV(c), SRW(m), ms, 0);
    m->owner = GetCurrentThreadId();
    m->depth = depth;
    if (!ok) return GetLastError() == ERROR_TIMEOUT ? ETIMEDOUT : EINVAL;
    return 0;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) { return cond_wait_ms(c, m, INFINITE); }

int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abstime) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    int64_t ms = ((int64_t)abstime->tv_sec - now.tv_sec) * 1000 + (abstime->tv_nsec - now.tv_nsec) / 1000000;
    if (ms < 0) ms = 0;
    if (ms > 0x7FFFFFFF) ms = 0x7FFFFFFF;
    return cond_wait_ms(c, m, (DWORD)ms);
}

int pthread_cond_signal(pthread_cond_t *c) { WakeConditionVariable(CV(c)); return 0; }
int pthread_cond_broadcast(pthread_cond_t *c) { WakeAllConditionVariable(CV(c)); return 0; }

int pthread_rwlock_rdlock(pthread_rwlock_t *l) { AcquireSRWLockShared(SRW(l)); return 0; }
int pthread_rwlock_wrlock(pthread_rwlock_t *l) {
    AcquireSRWLockExclusive(SRW(l));
    l->writer = GetCurrentThreadId();
    return 0;
}
int pthread_rwlock_unlock(pthread_rwlock_t *l) {
    if (l->writer == GetCurrentThreadId()) { l->writer = 0; ReleaseSRWLockExclusive(SRW(l)); }
    else ReleaseSRWLockShared(SRW(l));
    return 0;
}

int pthread_attr_init(pthread_attr_t *a) { a->detached = 0; a->stacksize = 0; return 0; }
int pthread_attr_destroy(pthread_attr_t *a) { (void)a; return 0; }
int pthread_attr_setdetachstate(pthread_attr_t *a, int s) { a->detached = s == PTHREAD_CREATE_DETACHED; return 0; }
int pthread_attr_setstacksize(pthread_attr_t *a, size_t s) { a->stacksize = s; return 0; }

typedef struct { void *(*fn)(void *); void *arg; } ThreadStart;

static unsigned __stdcall thread_tramp(void *p) {
    ThreadStart ts = *(ThreadStart *)p;
    free(p);
    ts.fn(ts.arg);
    thread_cleanup();
    return 0;
}

int pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg) {
    ThreadStart *ts = malloc(sizeof *ts);
    if (!ts) return EAGAIN;
    ts->fn = fn;
    ts->arg = arg;
    /* glibc gives 8 MiB by default; Windows would give 1 MiB. Reserve only. */
    unsigned stack = (unsigned)(a && a->stacksize ? a->stacksize : 8u << 20);
    uintptr_t h = _beginthreadex(NULL, stack, thread_tramp, ts, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!h) { free(ts); return EAGAIN; }
    if (a && a->detached) { CloseHandle((HANDLE)h); h = 0; }
    if (t) *t = h;
    return 0;
}

int pthread_detach(pthread_t t) {
    if (t) CloseHandle((HANDLE)t);
    return 0;
}

int pthread_join(pthread_t t, void **ret) {
    if (ret) *ret = NULL;
    if (!t) return EINVAL;
    WaitForSingleObject((HANDLE)t, INFINITE);
    CloseHandle((HANDLE)t);
    return 0;
}

pthread_t pthread_self(void) { return (pthread_t)GetCurrentThreadId(); }

void pthread_exit(void *ret) {
    (void)ret;
    thread_cleanup();
    _endthreadex(0);
    __builtin_unreachable();
}
