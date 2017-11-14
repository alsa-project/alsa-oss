/*
 *  OSS -> ALSA compatibility layer
 *  Copyright (c) by Abramo Bagnara <abramo@alsa-project.org>,
 *		     Jaroslav Kysela <perex@perex.cz>
 *
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <sys/select.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <assert.h>

#include "alsa-oss-emul.h"

#ifndef ATTRIBUTE_UNUSED
/** do not print warning (gcc) when function parameter is not used */
#define ATTRIBUTE_UNUSED __attribute__ ((__unused__))
#endif

#if 1
#define DEBUG_POLL
#define DEBUG_SELECT
#ifdef NEW_MACRO_VARARGS
#define DEBUG(...) do { if (oss_wrapper_debug) fprintf(stderr, __VA_ARGS__); } while (0)
#else /* !NEW_MACRO_VARARGS */
#define DEBUG(args...) do { if (oss_wrapper_debug) fprintf(stderr, ##args); } while (0)
#endif
#else
#ifdef NEW_MACRO_VARARGS
#define DEBUG(...)
#else /* !NEW_MACRO_VARARGS */
#define DEBUG(args...)
#endif
#endif

#ifndef O_LARGEFILE
#define O_LARGEFILE 0100000
#endif

static int (*_select)(int n, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout);
static int (*_poll)(struct pollfd *ufds, unsigned int nfds, int timeout);
static int (*_open)(const char *file, int oflag, ...);
static int (*_open64)(const char *file, int oflag, ...);
static int (*_close)(int fd);
static ssize_t (*_write)(int fd, const void *buf, size_t n);
static ssize_t (*_read)(int fd, void *buf, size_t n);
static int (*_ioctl)(int fd, unsigned long request, ...);
static int (*_fcntl)(int fd, int cmd, ...);
static void *(*_mmap)(void *addr, size_t len, int prot, int flags, int fd, off_t offset);
static int (*_munmap)(void* addr, size_t len);

static FILE *(*_fopen)(const char *path, const char *mode);
static FILE *(*_fopen64)(const char *path, const char *mode);

typedef struct ops {
	int (*close)(int fd);
	ssize_t (*write)(int fd, const void *buf, size_t n);
	ssize_t (*read)(int fd, void *buf, size_t n);
	int (*ioctl)(int fd, unsigned long request, ...);
	int (*fcntl)(int fd, int cmd, ...);
	void *(*mmap)(void *addr, size_t len, int prot, int flags, int fd, off_t offset);
	int (*munmap)(void* addr, size_t len);
} ops_t;

typedef enum {
	FD_OSS_DSP,
	FD_OSS_MIXER,
	FD_CLASSES,
} fd_class_t;                                                            

static ops_t ops[FD_CLASSES];

typedef struct {
	fd_class_t class;
	int oflags;
	void *mmap_area;
	int poll_fds;
} fd_t;

static void initialize(void);
static int initialized = 0;

static int oss_wrapper_debug = 0;
static int open_max;
static int poll_fds_add = 0;
static fd_t **fds;

static inline int is_oss_device(int fd)
{
	return fd >= 0 && fd < open_max && fds[fd];
}

static int is_dsp_device(const char *pathname)
{
	if(!pathname) return 0;
	if(strncmp(pathname,"/dev/dsp",8) == 0) return 1;
	if(strncmp(pathname,"/dev/adsp",9) == 0) return 1;
	if(strncmp(pathname,"/dev/audio",10) == 0) return 1;
	if(strncmp(pathname,"/dev/sound/dsp",14) == 0) return 1;
	if(strncmp(pathname,"/dev/sound/adsp",15) == 0) return 1;
	if(strncmp(pathname,"/dev/sound/audio",16) == 0) return 1;
	return 0;
}

static int is_mixer_device(const char *pathname)
{
	if(!pathname) return 0;
	if(strncmp(pathname,"/dev/mixer",10) == 0) return 1;
	if(strncmp(pathname,"/dev/sound/mixer",16) == 0) return 1;
	return 0;
}

static int oss_pcm_fcntl(int fd, int cmd, ...)
{
	int result;
	va_list args;
	long arg;

	if (!initialized)
		initialize();

	va_start(args, cmd);
	arg = va_arg(args, long);
	va_end(args);

	switch (cmd) {
	case F_GETFL:
		return fds[fd]->oflags;
        case F_SETFL:
		result = lib_oss_pcm_nonblock(fd, (arg & O_NONBLOCK) ? 1 : 0);
                if (result < 0) {
                        errno = -result;
                        return -1;
                }
                return 0;
	default:
		DEBUG("pcm_fcntl(%d, ", fd);
		result = _fcntl(fd, cmd, arg);
		if (result < 0)
			return result;
		DEBUG("%x, %ld)\n", cmd, arg);
		return result;
	}
	return -1;
}

static int oss_mixer_fcntl(int fd, int cmd, ...)
{
	int result;
	va_list args;
	long arg;

	va_start(args, cmd);
	arg = va_arg(args, long);
	va_end(args);

	switch (cmd) {
	case F_GETFL:
		return fds[fd]->oflags;
	default:
		DEBUG("mixer_fcntl(%d, ", fd);
		result = _fcntl(fd, cmd, arg);
		if (result < 0)
			return result;
		DEBUG("%x, %ld)\n", cmd, arg);
		return result;
	}
	return -1;
}

static ssize_t bad_write(int fd ATTRIBUTE_UNUSED, const void *buf ATTRIBUTE_UNUSED, size_t n ATTRIBUTE_UNUSED)
{
	errno = EBADFD;
	return -1;
}

static ssize_t bad_read(int fd ATTRIBUTE_UNUSED, void *buf ATTRIBUTE_UNUSED, size_t n ATTRIBUTE_UNUSED)
{
	errno = EBADFD;
	return -1;
}

static void *bad_mmap(void *addr ATTRIBUTE_UNUSED, size_t len ATTRIBUTE_UNUSED,
		      int prot ATTRIBUTE_UNUSED, int flags ATTRIBUTE_UNUSED,
		      int fd ATTRIBUTE_UNUSED, off_t offset ATTRIBUTE_UNUSED)
{
	errno = EBADFD;
	return MAP_FAILED;
}

static int bad_munmap(void* addr ATTRIBUTE_UNUSED, size_t len ATTRIBUTE_UNUSED)
{
	errno = EBADFD;
	return -1;
}

static ops_t ops[FD_CLASSES] = {
        [FD_OSS_DSP] = {
		.close = lib_oss_pcm_close,
		.write = lib_oss_pcm_write,
		.read = lib_oss_pcm_read,
		.ioctl = lib_oss_pcm_ioctl,
		.fcntl = oss_pcm_fcntl,
		.mmap = lib_oss_pcm_mmap,
		.munmap = lib_oss_pcm_munmap,
        },
        [FD_OSS_MIXER] = {
		.close = lib_oss_mixer_close,
		.write = bad_write,
		.read = bad_read,
		.ioctl = lib_oss_mixer_ioctl,
		.fcntl = oss_mixer_fcntl,
		.mmap = bad_mmap,
		.munmap = bad_munmap,
	},
};

static int dsp_open_helper(const char *file, int oflag)
{
	int fd;
	fd = lib_oss_pcm_open(file, oflag);
	if (fd >= 0) {
		int nfds;
		fds[fd] = calloc(sizeof(fd_t), 1);
		if (fds[fd] == NULL) {
			ops[FD_OSS_DSP].close(fd);
			errno = ENOMEM;
			return -1;
		}
		fds[fd]->class = FD_OSS_DSP;
		fds[fd]->oflags = oflag;
		nfds = lib_oss_pcm_poll_fds(fd);
		if (nfds > 0) {
			fds[fd]->poll_fds = nfds;
			poll_fds_add += nfds;
		}
	}
	return fd;
}

static int mixer_open_helper(const char *file, int oflag)
{
	int fd;
	fd = lib_oss_mixer_open(file, oflag);
	if (fd >= 0) {
		fds[fd] = calloc(sizeof(fd_t), 1);
		if (fds[fd] == NULL) {
			ops[FD_OSS_MIXER].close(fd);
			errno = ENOMEM;
			return -1;
		}
		fds[fd]->class = FD_OSS_MIXER;
		fds[fd]->oflags = oflag;
	}
	return fd;
} 

#define DECL_OPEN(name, callback) \
int name(const char *file, int oflag, ...) \
{ \
	va_list args; \
	mode_t mode = 0; \
	int fd; \
	if (!initialized) \
		initialize(); \
	if (oflag & O_CREAT) { \
		va_start(args, oflag); \
		mode = va_arg(args, mode_t); \
		va_end(args); \
	} \
	if (is_dsp_device(file)) \
		fd = dsp_open_helper(file, oflag); \
	else if (is_mixer_device(file)) \
		fd = mixer_open_helper(file, oflag); \
	else { \
		fd = callback(file, oflag, mode); \
		if (fd >= 0) \
			assert(fds[fd] == NULL); \
	} \
	return fd; \
}

DECL_OPEN(open, _open)
DECL_OPEN(open64, _open64)

int close(int fd)
{
	if (!initialized)
		initialize();

	if (! is_oss_device(fd)) {
		return _close(fd);
	} else {
		fd_t *xfd = fds[fd];
		int err;

		fds[fd] = NULL;
		poll_fds_add -= xfd->poll_fds;
		if (poll_fds_add < 0) {
			fprintf(stderr, "alsa-oss: poll_fds_add screwed up!\n");
			poll_fds_add = 0;
		}
		err = ops[xfd->class].close(fd);
		// assert(err >= 0);
		return err;
	}
}

ssize_t write(int fd, const void *buf, size_t n)
{
	if (!initialized)
		initialize();

	if (! is_oss_device(fd))
		return _write(fd, buf, n);
	else
		return ops[fds[fd]->class].write(fd, buf, n);
}

ssize_t read(int fd, void *buf, size_t n)
{
	if (!initialized)
		initialize();

	if (! is_oss_device(fd))
		return _read(fd, buf, n);
	else
		return ops[fds[fd]->class].read(fd, buf, n);
}

int ioctl(int fd, unsigned long request, ...)
{
	va_list args;
	void *arg;

	if (!initialized)
		initialize();

	va_start(args, request);
	arg = va_arg(args, void *);
	va_end(args);
	if (! is_oss_device(fd))
		return _ioctl(fd, request, arg);
	else
		return ops[fds[fd]->class].ioctl(fd, request, arg);
}

int fcntl(int fd, int cmd, ...)
{
	va_list args;
	void *arg;

	if (!initialized)
		initialize();

	va_start(args, cmd);
	arg = va_arg(args, void *);
	va_end(args);
	if (! is_oss_device(fd))
		return _fcntl(fd, cmd, arg);
	else
		return ops[fds[fd]->class].fcntl(fd, cmd, arg);
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset)
{
	void *result;

	if (!initialized)
		initialize();

	if (! is_oss_device(fd))
		return _mmap(addr, len, prot, flags, fd, offset);
	result = ops[fds[fd]->class].mmap(addr, len, prot, flags, fd, offset);
	if (result != NULL && result != MAP_FAILED)
		fds[fd]->mmap_area = result;
	return result;
}

int munmap(void *addr, size_t len)
{
	int fd;

	if (!initialized)
		initialize();

	for (fd = 0; fd < open_max; ++fd) {
		if (fds[fd] && fds[fd]->mmap_area == addr)
			break;
	}
	if (fd >= open_max)
		return _munmap(addr, len);
	fds[fd]->mmap_area = 0;
	return ops[fds[fd]->class].munmap(addr, len);
}

#ifdef DEBUG_POLL
void dump_poll(struct pollfd *pfds, unsigned long nfds, int timeout)
{
	unsigned int k;
	fprintf(stderr, "POLL nfds: %ld, timeout: %d\n", nfds, timeout);
	for (k = 0; k < nfds; ++k) {
		fprintf(stderr, "fd=%d, events=%x, revents=%x\n", 
			pfds[k].fd, pfds[k].events, pfds[k].revents);
	}
}
#endif

#ifdef DEBUG_SELECT
void dump_select(int nfds, fd_set *rfds, fd_set *wfds, fd_set *efds,
		 struct timeval *timeout)
{
	int k;
	fprintf(stderr, "SELECT nfds: %d, ", nfds);
	if (timeout)
		fprintf(stderr, "timeout: %ld.%06ld\n", (long)timeout->tv_sec, (long)timeout->tv_usec);
	else
		fprintf(stderr, "no timeout\n");
	if (rfds) {
		fprintf(stderr, "rfds: ");
		for (k = 0; k < nfds; ++k) {
			if (FD_ISSET(k, rfds))
				putc('1', stderr);
			else
				putc('0', stderr);
		}
		putc('\n', stderr);
	}
	if (wfds) {
		fprintf(stderr, "wfds: ");
		for (k = 0; k < nfds; ++k) {
			if (FD_ISSET(k, wfds))
				putc('1', stderr);
			else
				putc('0', stderr);
		}
		putc('\n', stderr);
	}
	if (efds) {
		fprintf(stderr, "efds: ");
		for (k = 0; k < nfds; ++k) {
			if (FD_ISSET(k, efds))
				putc('1', stderr);
			else
				putc('0', stderr);
		}
		putc('\n', stderr);
	}
}
#endif

static int poll_with_pcm(struct pollfd *pfds, unsigned long nfds, int timeout);

int poll(struct pollfd *pfds, unsigned long nfds, int timeout)
{
	unsigned int k;

	if (!initialized)
		initialize();

	for (k = 0; k < nfds; ++k) {
		int fd = pfds[k].fd;
		if (! is_oss_device(fd))
			continue;
		if (fds[fd]->class == FD_OSS_DSP)
			return poll_with_pcm(pfds, nfds, timeout);
	}
	return _poll(pfds, nfds, timeout);
}


static int poll_with_pcm(struct pollfd *pfds, unsigned long nfds, int timeout)
{
	unsigned int k;
	unsigned int nfds1;
	int count;
	struct pollfd pfds1[nfds + poll_fds_add + 16];

	nfds1 = 0;
	for (k = 0; k < nfds; ++k) {
		int fd = pfds[k].fd;
		if (is_oss_device(fd) && fds[fd]->class == FD_OSS_DSP) {
			unsigned short events = pfds[k].events;
			int fmode = 0;
			if ((events & (POLLIN|POLLOUT)) == (POLLIN|POLLOUT))
				fmode = O_RDWR;
			else if (events & POLLIN)
				fmode = O_RDONLY;
			else
				fmode = O_WRONLY;
			count = lib_oss_pcm_poll_prepare(fd, fmode, &pfds1[nfds1]);
			if (count < 0)
				return -1;
			nfds1 += count;
		} else {
			pfds1[nfds1] = pfds[k];
			nfds1++;
		}
		if (nfds1 > nfds + poll_fds_add) {
			fprintf(stderr, "alsa-oss: Pollfd overflow!\n");
			errno = EINVAL;
			return -1;
		}
	}
#ifdef DEBUG_POLL
	if (oss_wrapper_debug) {
		fprintf(stderr, "Orig enter ");
		dump_poll(pfds, nfds, timeout);
		fprintf(stderr, "Changed enter ");
		dump_poll(pfds1, nfds1, timeout);
	}
#endif
	count = _poll(pfds1, nfds1, timeout);
	if (count <= 0)
		return count;
	nfds1 = 0;
	count = 0;
	for (k = 0; k < nfds; ++k) {
		int fd = pfds[k].fd;
		unsigned int revents;
		if (is_oss_device(fd) && fds[fd]->class == FD_OSS_DSP) {
			int result = lib_oss_pcm_poll_result(fd, &pfds1[nfds1]);
			revents = 0;
			if (result < 0) {
				revents |= POLLNVAL;
			} else {
				revents |= ((result & OSS_WAIT_EVENT_ERROR) ? POLLERR : 0) |
					   ((result & OSS_WAIT_EVENT_READ) ? POLLIN : 0) |
					   ((result & OSS_WAIT_EVENT_WRITE) ? POLLOUT : 0);
			}
			nfds1 += lib_oss_pcm_poll_fds(fd);
		} else {
			revents = pfds1[nfds1].revents;
			nfds1++;
		}
		pfds[k].revents = revents;
		if (revents)
			count++;
	}
#ifdef DEBUG_POLL
	if (oss_wrapper_debug) {
		fprintf(stderr, "Changed exit ");
		dump_poll(pfds1, nfds1, timeout);
		fprintf(stderr, "Orig exit ");
		dump_poll(pfds, nfds, timeout);
	}
#endif
	return count;
}

static int select_with_pcm(int nfds, fd_set *rfds, fd_set *wfds, fd_set *efds,
			   struct timeval *timeout);

int select(int nfds, fd_set *rfds, fd_set *wfds, fd_set *efds,
	   struct timeval *timeout)
{
	int fd;

	if (!initialized)
		initialize();

	for (fd = 0; fd < nfds; ++fd) {
		int r = (rfds && FD_ISSET(fd, rfds));
		int w = (wfds && FD_ISSET(fd, wfds));
		int e = (efds && FD_ISSET(fd, efds));
		if (!(r || w || e))
			continue;
		if (is_oss_device(fd) && fds[fd]->class == FD_OSS_DSP)
			return select_with_pcm(nfds, rfds, wfds, efds, timeout);
	}
	return _select(nfds, rfds, wfds, efds, timeout);
}


static int select_with_pcm(int nfds, fd_set *rfds, fd_set *wfds, fd_set *efds,
			   struct timeval *timeout)
{
	fd_set _rfds1, _wfds1, _efds1;
	fd_set *rfds1, *wfds1, *efds1;
	int fd;
	int nfds1 = nfds;
	int count;

	if (rfds)
		_rfds1 = *rfds;
	else
		FD_ZERO(&_rfds1);
	rfds1 = &_rfds1;
	if (wfds)
		_wfds1 = *wfds;
	else
		FD_ZERO(&_wfds1);
	wfds1 = &_wfds1;
	if (efds) {
		_efds1 = *efds;
		efds1 = &_efds1;
	} else {
		efds1 = NULL;
	}
	for (fd = 0; fd < nfds; ++fd) {
		int r = (rfds && FD_ISSET(fd, rfds));
		int w = (wfds && FD_ISSET(fd, wfds));
		int e = (efds && FD_ISSET(fd, efds));
		if (!(r || w || e))
			continue;
		if (is_oss_device(fd) && fds[fd]->class == FD_OSS_DSP) {
			int res, fmode = 0;
			
			if (r & w)
				fmode = O_RDWR;
			else if (r)
				fmode = O_RDONLY;
			else
				fmode = O_WRONLY;
			res = lib_oss_pcm_select_prepare(fd, fmode, rfds1, wfds1,
							 e ? efds1 : NULL);
			if (res < 0)
				return -1;
			if (nfds1 < res + 1)
				nfds1 = res + 1;
			if (r)
				FD_CLR(fd, rfds1);
			if (w)
				FD_CLR(fd, wfds1);
			if (e)
				FD_CLR(fd, efds1);
		}
	}
#ifdef DEBUG_SELECT
	if (oss_wrapper_debug) {
		fprintf(stderr, "Orig enter ");
		dump_select(nfds, rfds, wfds, efds, timeout);
		fprintf(stderr, "Changed enter ");
		dump_select(nfds1, rfds1, wfds1, efds1, timeout);
	}
#endif
	count = _select(nfds1, rfds1, wfds1, efds1, timeout);
	if (count < 0)
		return count;
	if (count == 0) {
		if (rfds)
			FD_ZERO(rfds);
		if (wfds)
			FD_ZERO(wfds);
		if (efds)
			FD_ZERO(efds);
		return 0;
	}
	count = 0;
	for (fd = 0; fd < nfds; ++fd) {
		int r = (rfds && FD_ISSET(fd, rfds));
		int w = (wfds && FD_ISSET(fd, wfds));
		int e = (efds && FD_ISSET(fd, efds));
		int r1, w1, e1;
		if (!(r || w || e))
			continue;
		if (is_oss_device(fd) && fds[fd]->class == FD_OSS_DSP) {
			int result = lib_oss_pcm_select_result(fd, rfds1, wfds1, efds1);
			r1 = w1 = e1 = 0;
			if (result < 0 && e) {
				if (efds)
					FD_SET(fd, efds);
				e1 = 1;
			} else {
				if (result & OSS_WAIT_EVENT_ERROR) {
					if (efds)
						FD_SET(fd, efds);
					e1 = 1;
				}
				if (result & OSS_WAIT_EVENT_READ) {
					if (rfds)
						FD_SET(fd, rfds);
					r1 = 1;
				}
				if (result & OSS_WAIT_EVENT_WRITE) {
					if (wfds)
						FD_SET(fd, wfds);
					w1 = 1;
				}
			}
		} else {
			r1 = (r && FD_ISSET(fd, rfds1));
			w1 = (w && FD_ISSET(fd, wfds1));
			e1 = (e && FD_ISSET(fd, efds1));
		}
		if (r && !r1 && rfds)
			FD_CLR(fd, rfds);
		if (w && !w1 && wfds)
			FD_CLR(fd, wfds);
		if (e && !e1 && efds)
			FD_CLR(fd, efds);
		if (r1 || w1 || e1)
			count++;
	}
#ifdef DEBUG_SELECT
	if (oss_wrapper_debug) {
		fprintf(stderr, "Changed exit ");
		dump_select(nfds1, rfds1, wfds1, efds1, timeout);
		fprintf(stderr, "Orig exit ");
		dump_select(nfds, rfds, wfds, efds, timeout);
	}
#endif
	return count;
}


#include "stdioemu.c"

FILE *fopen(const char* path, const char *mode)
{
	if (!initialized)
		initialize();

	if (!is_dsp_device(path)) 
		return _fopen(path, mode);
	
	return fake_fopen(path, mode, 0);
}

FILE *fopen64(const char* path, const char *mode)
{
	if (!initialized)   
		initialize(); 

	if (!is_dsp_device(path))
		return _fopen64(path, mode);

	return fake_fopen(path, mode, O_LARGEFILE);
}

#if 0
int dup(int fd)
{
	return fcntl(fd, F_DUPFD, 0);
}
#endif

#if 0
int dup2(int fd, int fd2)
{
	int save;

	if (fd2 < 0 || fd2 >= open_max) {
		errno = EBADF;
		return -1;
	}
	
	if (fcntl(fd, F_GETFL) < 0)
		return -1;
	
	if (fd == fd2)
		return fd2;
	
	save = errno;
	close(fd2);
	errno = save;
	
	return fcntl(fd, F_DUPFD, fd2);
}
#endif

# define strong_alias(name, aliasname) \
  extern __typeof (name) aliasname __attribute__ ((alias (#name)));

strong_alias(open, __open);
strong_alias(open64, __open64);
strong_alias(close, __close);
strong_alias(write, __write);
strong_alias(read, __read);
strong_alias(ioctl, __ioctl);
strong_alias(fcntl, __fcntl);
strong_alias(mmap, __mmap);
strong_alias(munmap, __munmap);
strong_alias(poll, __poll);
strong_alias(select, __select);
strong_alias(fopen, __fopen);
strong_alias(fopen64, __fopen64);

/* called by each override if needed */
static void initialize()
{
	char *s = getenv("ALSA_OSS_DEBUG");
	if (s)
		oss_wrapper_debug = 1;
	open_max = sysconf(_SC_OPEN_MAX);
	if (open_max < 0)
		exit(1);
	fds = calloc(open_max, sizeof(*fds));
	if (!fds)
		exit(1);
	_open = dlsym(RTLD_NEXT, "open");
	_open64 = dlsym(RTLD_NEXT, "open64");
	_close = dlsym(RTLD_NEXT, "close");
	_write = dlsym(RTLD_NEXT, "write");
	_read = dlsym(RTLD_NEXT, "read");
	_ioctl = dlsym(RTLD_NEXT, "ioctl");
	_fcntl = dlsym(RTLD_NEXT, "fcntl");
	_mmap = dlsym(RTLD_NEXT, "mmap");
	_munmap = dlsym(RTLD_NEXT, "munmap");
	_select = dlsym(RTLD_NEXT, "select");
	_poll = dlsym(RTLD_NEXT, "poll");
	_fopen = dlsym(RTLD_NEXT, "fopen");
	_fopen64 = dlsym(RTLD_NEXT, "fopen64");
	initialized = 1;
}
