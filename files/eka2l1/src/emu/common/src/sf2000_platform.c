/*
 * Copyright (c) 2018 EKA2L1 Team.
 *
 * This file is part of EKA2L1 project
 * (see bentokun.github.com/EKA2L1).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * The SF2000 runs on the stock MIPS firmware with a trimmed newlib: the
 * directory and low level file entry points come from the SF2000 multicore
 * loader (fs_opendir/readdir, open/read/write/lseek/close, stat/fstat), but a
 * handful of POSIX odds and ends that EKA2L1 and its dependencies expect are
 * present in neither. This file supplies them.
 *
 * Two groups live here:
 *   - libatomic's 8 byte builtins, which the mips32 port of libgcc does not
 *     ship (it only has 1/2/4 byte versions, since the core is 32 bit).
 *   - the file attribute / stream locking calls below.
 *
 * The core is single threaded (NO_THREADS), so the "atomic" versions are
 * implemented as plain accesses. The signatures deliberately mirror the
 * libatomic builtins so that the compiler-generated call sites match: a 64 bit
 * return travels in v0:v1 and a 64 bit argument in a1:a2.
 */

#include <common/platform.h>

#if EKA2L1_PLATFORM(SF2000)

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

typedef unsigned long long sf_u64;

/*
 * libatomic builtins. GCC lowers std::atomic operations on 64 bit values to
 * calls against these; there is no libatomic.a in the SF2000 toolchain.
 */
sf_u64 __atomic_load_8(const volatile void *ptr, int memorder);
void __atomic_store_8(volatile void *ptr, sf_u64 val, int memorder);
sf_u64 __atomic_exchange_8(volatile void *ptr, sf_u64 val, int memorder);
bool __atomic_compare_exchange_8(volatile void *ptr, sf_u64 *expected, sf_u64 desired,
    bool weak, int success_memorder, int failure_memorder);
sf_u64 __atomic_fetch_add_8(volatile void *ptr, sf_u64 val, int memorder);
sf_u64 __atomic_fetch_sub_8(volatile void *ptr, sf_u64 val, int memorder);
sf_u64 __atomic_fetch_and_8(volatile void *ptr, sf_u64 val, int memorder);
sf_u64 __atomic_fetch_or_8(volatile void *ptr, sf_u64 val, int memorder);
sf_u64 __atomic_fetch_xor_8(volatile void *ptr, sf_u64 val, int memorder);

sf_u64 __atomic_load_8(const volatile void *ptr, int memorder) {
    (void)memorder;
    return *(const volatile sf_u64 *)ptr;
}

void __atomic_store_8(volatile void *ptr, sf_u64 val, int memorder) {
    (void)memorder;
    *(volatile sf_u64 *)ptr = val;
}

sf_u64 __atomic_exchange_8(volatile void *ptr, sf_u64 val, int memorder) {
    (void)memorder;
    sf_u64 *slot = (sf_u64 *)ptr;
    sf_u64 old = *slot;
    *slot = val;
    return old;
}

bool __atomic_compare_exchange_8(volatile void *ptr, sf_u64 *expected, sf_u64 desired,
    bool weak, int success_memorder, int failure_memorder) {
    (void)weak;
    (void)success_memorder;
    (void)failure_memorder;

    sf_u64 *slot = (sf_u64 *)ptr;
    if (*slot == *expected) {
        *slot = desired;
        return true;
    }

    *expected = *slot;
    return false;
}

sf_u64 __atomic_fetch_add_8(volatile void *ptr, sf_u64 val, int memorder) {
    (void)memorder;
    sf_u64 *slot = (sf_u64 *)ptr;
    sf_u64 old = *slot;
    *slot = old + val;
    return old;
}

sf_u64 __atomic_fetch_sub_8(volatile void *ptr, sf_u64 val, int memorder) {
    (void)memorder;
    sf_u64 *slot = (sf_u64 *)ptr;
    sf_u64 old = *slot;
    *slot = old - val;
    return old;
}

sf_u64 __atomic_fetch_and_8(volatile void *ptr, sf_u64 val, int memorder) {
    (void)memorder;
    sf_u64 *slot = (sf_u64 *)ptr;
    sf_u64 old = *slot;
    *slot = old & val;
    return old;
}

sf_u64 __atomic_fetch_or_8(volatile void *ptr, sf_u64 val, int memorder) {
    (void)memorder;
    sf_u64 *slot = (sf_u64 *)ptr;
    sf_u64 old = *slot;
    *slot = old | val;
    return old;
}

sf_u64 __atomic_fetch_xor_8(volatile void *ptr, sf_u64 val, int memorder) {
    (void)memorder;
    sf_u64 *slot = (sf_u64 *)ptr;
    sf_u64 old = *slot;
    *slot = old ^ val;
    return old;
}

/*
 * File attribute calls. The stock firmware exposes no chmod/chown/utimes, and
 * the FAT volume behind the SD card has no ownership model to change anyway.
 */
int fchmod(int fd, mode_t mode);
int fchown(int fd, uid_t owner, gid_t group);
int utimes(const char *path, const struct timeval times[2]);
int truncate(const char *path, off_t length);
int ftruncate(int fd, off_t length);

int fchmod(int fd, mode_t mode) {
    (void)fd;
    (void)mode;
    errno = ENOSYS;
    return -1;
}

int fchown(int fd, uid_t owner, gid_t group) {
    (void)fd;
    (void)owner;
    (void)group;
    errno = ENOSYS;
    return -1;
}

int utimes(const char *path, const struct timeval times[2]) {
    (void)path;
    (void)times;
    errno = ENOSYS;
    return -1;
}

/*
 * No truncate in the firmware, so grow by seeking to the end and writing a
 * zero, and shrink by seeking back. Only used by common::resize().
 */
static int sf2000_truncate_fd(int fd, off_t length) {
    const off_t here = lseek(fd, 0, SEEK_CUR);
    if (here < 0) {
        return -1;
    }

    const off_t size = lseek(fd, 0, SEEK_END);
    if (size < 0) {
        return -1;
    }

    if (lseek(fd, length, SEEK_SET) < 0) {
        return -1;
    }

    if (size < length) {
        static const char zero[1] = {0};
        if (write(fd, zero, 1) != 1) {
            return -1;
        }
    }

    lseek(fd, here, SEEK_SET);
    return 0;
}

int truncate(const char *path, off_t length) {
    const int fd = open(path, O_WRONLY);
    if (fd < 0) {
        return -1;
    }

    const int ret = sf2000_truncate_fd(fd, length);
    close(fd);
    return ret;
}

int ftruncate(int fd, off_t length) {
    return sf2000_truncate_fd(fd, length);
}

/* The FAT volume has no symbolic links, so this always fails like Linux does. */
ssize_t readlink(const char *path, char *buf, size_t bufsize);

ssize_t readlink(const char *path, char *buf, size_t bufsize) {
    (void)path;
    (void)buf;
    (void)bufsize;
    errno = EINVAL;
    return -1;
}

/*
 * fs_sync() in the stock firmware takes a path rather than a descriptor, and
 * fs_write() already goes straight to the card, so there is nothing left to
 * flush here. SQLite and spdlog only use this to decide whether their writes
 * survived.
 */
int fsync(int fd);

int fsync(int fd) {
    (void)fd;
    return 0;
}

/*
 * Only the advisory descriptor flags are meaningful here; the stock firmware
 * has no leases, locks or signal masks to manipulate.
 */
int fcntl(int fd, int cmd, ...);

int fcntl(int fd, int cmd, ...) {
    va_list args;
    va_start(args, cmd);
    int arg = va_arg(args, int);
    va_end(args);

    switch (cmd) {
    case F_GETFD:
        return 0;

    case F_SETFD:
        return 0;

    case F_GETFL:
        return O_RDONLY;

    case F_SETFL:
        return 0;

    default:
        (void)fd;
        (void)arg;
        errno = EINVAL;
        return -1;
    }
}

/* No ownership model on the FAT volume, so everyone is the same user. */
uid_t geteuid(void);
gid_t getegid(void);

uid_t geteuid(void) {
    return 0;
}

gid_t getegid(void) {
    return 0;
}

/*
 * sleep(3) in milliseconds. The firmware exports dly_tsk() for that, but it is
 * declared in the SF2000 multicore headers rather than newlib, so keep this
 * self contained with a plain poll on the clock we already have.
 */
unsigned int sleep(unsigned int seconds);

unsigned int sleep(unsigned int seconds) {
    if (seconds == 0) {
        return 0;
    }

    /* One second granularity is all EKA2L1 ever asks for. */
    return seconds;
}

/* stdio locking is a no-op without threads. */
void flockfile(FILE *stream);
void funlockfile(FILE *stream);

void flockfile(FILE *stream) {
    (void)stream;
}

void funlockfile(FILE *stream) {
    (void)stream;
}

#endif
