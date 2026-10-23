#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif

#include "internal.h"

#ifdef _WIN32

#include <stdlib.h>

void favo_lock_init(favo_lock *lock)
{
    InitializeSRWLock(lock);
}

void favo_lock_destroy(favo_lock *lock)
{
    (void)lock;
}

void favo_lock_acquire(favo_lock *lock)
{
    AcquireSRWLockExclusive(lock);
}

void favo_lock_release(favo_lock *lock)
{
    ReleaseSRWLockExclusive(lock);
}

size_t favo_os_page(void)
{
    static size_t page;
    if (!page) {
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        page = info.dwPageSize ? (size_t)info.dwPageSize : 4096;
    }
    return page;
}

void *favo_os_map(size_t length)
{
    return VirtualAlloc(NULL, length, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

int favo_os_unmap(void *base, size_t length)
{
    (void)length;
    return VirtualFree(base, 0, MEM_RELEASE) ? 0 : -1;
}

int favo_os_has_sbrk(void)
{
    return 0;
}

void *favo_os_sbrk(intptr_t increment)
{
    (void)increment;
    return (void *)-1;
}

void favo_os_fatal(const char *message)
{
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    DWORD written = 0;
    if (err && err != INVALID_HANDLE_VALUE) {
        WriteFile(err, message, (DWORD)strlen(message), &written, NULL);
        WriteFile(err, "\n", 1, &written, NULL);
    }
    abort();
}

#else

#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

void favo_lock_init(favo_lock *lock)
{
    pthread_mutex_init(lock, NULL);
}

void favo_lock_destroy(favo_lock *lock)
{
    pthread_mutex_destroy(lock);
}

void favo_lock_acquire(favo_lock *lock)
{
    pthread_mutex_lock(lock);
}

void favo_lock_release(favo_lock *lock)
{
    pthread_mutex_unlock(lock);
}

size_t favo_os_page(void)
{
    static size_t page;
    if (!page) {
        long value = sysconf(_SC_PAGESIZE);
        page = value > 0 ? (size_t)value : 4096;
    }
    return page;
}

void *favo_os_map(size_t length)
{
    void *p = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

int favo_os_unmap(void *base, size_t length)
{
    return munmap(base, length);
}

#if defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
int favo_os_has_sbrk(void)
{
    return 1;
}

void *favo_os_sbrk(intptr_t increment)
{
    return sbrk(increment);
}
#else
int favo_os_has_sbrk(void)
{
    return 0;
}

void *favo_os_sbrk(intptr_t increment)
{
    (void)increment;
    return (void *)-1;
}
#endif

void favo_os_fatal(const char *message)
{
    ssize_t ignored = write(2, message, strlen(message));
    ignored = write(2, "\n", 1);
    (void)ignored;
    abort();
}

#endif

size_t favo_page_size(void)
{
    return favo_os_page();
}
