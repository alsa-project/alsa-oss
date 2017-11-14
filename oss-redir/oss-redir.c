/*
 *  OSS Redirector
 *  Copyright (c) by Jaroslav Kysela <perex@perex.cz>
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

#include "oss-redir.h"
#include <sys/poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>
#include <linux/soundcard.h>

static int initialized = 0;
static int native_oss = 1;
static int open_count = 0;
static char hal[64];
static void *dl_handle = NULL;

static void initialize(void);

static int (*x_oss_pcm_open)(const char *pathname, int flags);
static int (*x_oss_pcm_close)(int fd);
int (*oss_pcm_nonblock)(int fd, int nonblock);
ssize_t (*oss_pcm_read)(int fd, void *buf, size_t count);
ssize_t (*oss_pcm_write)(int fd, const void *buf, size_t count);
void * (*oss_pcm_mmap)(void *start, size_t length, int prot, int flags, int fd, off_t offset);
int (*oss_pcm_munmap)(void *start, size_t length);
int (*oss_pcm_ioctl)(int fd, unsigned long int request, ...);
int (*oss_pcm_select_prepare)(int fd, int fmode, fd_set *readfds, fd_set *writefds, fd_set *exceptfds);
int (*oss_pcm_select_result)(int fd, fd_set *readfds, fd_set *writefds, fd_set *exceptfds);
int (*oss_pcm_poll_fds)(int fd);
int (*oss_pcm_poll_prepare)(int fd, int fmode, struct pollfd *ufds);
int (*oss_pcm_poll_result)(int fd, struct pollfd *ufds);

static int (*x_oss_mixer_open)(const char *pathname, int flags);
static int (*x_oss_mixer_close)(int fd);
int (*oss_mixer_ioctl)(int fd, unsigned long int request, ...);

static int native_pcm_nonblock(int fd, int nonblock)
{
	long flags;

	if ((flags = fcntl(fd, F_GETFL)) < 0)
		return -1;
	if (nonblock)
		flags |= O_NONBLOCK;
	else
		flags &= ~O_NONBLOCK;
	if (fcntl(fd, F_SETFL, flags) < 0)
		return -1;
	return 0;
}

static int native_pcm_select_prepare(int fd, int fmode, fd_set *readfds, fd_set *writefds, fd_set *exceptfds)
{
	if (fd < 0)
		return -EINVAL;
	if ((fmode & O_ACCMODE) != O_WRONLY && readfds) {
		FD_SET(fd, readfds);
		if (exceptfds)
			FD_SET(fd, exceptfds);
	}
	if ((fmode & O_ACCMODE) != O_RDONLY && writefds) {
		FD_SET(fd, writefds);
		if (exceptfds)
			FD_SET(fd, exceptfds);
	}
	return fd;
}

static int native_pcm_select_result(int fd, fd_set *readfds, fd_set *writefds, fd_set *exceptfds)
{
	int result = 0;

	if (fd < 0)
		return -EINVAL;
	if (readfds && FD_ISSET(fd, readfds))
		result |= OSS_WAIT_EVENT_READ;
	if (writefds && FD_ISSET(fd, writefds))
		result |= OSS_WAIT_EVENT_WRITE;
	if (exceptfds && FD_ISSET(fd, exceptfds))
		result |= OSS_WAIT_EVENT_ERROR;
	return result;
}

static int native_pcm_poll_fds(int fd)
{
	if (fd < 0)
		return -EINVAL;
	return 1;
}

static int native_pcm_poll_prepare(int fd, int fmode, struct pollfd *ufds)
{
	if (fd < 0)
		return -EINVAL;
	ufds->fd = fd;
	ufds->events = ((fmode & O_ACCMODE) == O_WRONLY ? 0 : POLLIN) |
		       ((fmode & O_ACCMODE) == O_RDONLY ? 0 : POLLOUT) | POLLERR;
	return 1;
}

static int native_pcm_poll_result(int fd, struct pollfd *ufds)
{
	int result = 0;

	if (fd < 0)
		return -EINVAL;
	if (ufds->events & POLLIN)
		result |= OSS_WAIT_EVENT_READ;
	if (ufds->events & POLLOUT)
		result |= OSS_WAIT_EVENT_WRITE;
	if (ufds->events & POLLERR)
		result |= OSS_WAIT_EVENT_ERROR;
	return result;
}

static inline void check_initialized(void)
{
	if (!initialized)
		initialize();
}

int oss_pcm_open(const char *pathname, int flags, ...)
{
	int result;

	check_initialized();
	if (native_oss)
		return open(pathname, flags);
	result = x_oss_pcm_open(pathname, flags);
	if (result >= 0) {
		open_count++;
	} else {
		if (open_count == 0) {
			dlclose(dl_handle);
			dl_handle = NULL;
		}
	}
	return result;
}

int oss_pcm_close(int fd)
{
	int result;

	if (native_oss)
		return close(fd);
	result = x_oss_pcm_close(fd);
	if (--open_count) {
		dlclose(dl_handle);
		dl_handle = NULL;
	}
	return result;
}

int oss_mixer_open(const char *pathname, int flags, ...)
{
	int result;

	check_initialized();
	if (native_oss)
		return open(pathname, flags);
	result = x_oss_mixer_open(pathname, flags);
	if (result >= 0) {
		open_count++;
	} else {
		if (open_count == 0) {
			dlclose(dl_handle);
			dl_handle = NULL;
		}
	}
	return result;
}

int oss_mixer_close(int fd)
{
	int result;

	if (fd < 0)
		return -EINVAL;
	if (native_oss)
		return close(fd);
	result = x_oss_mixer_close(fd);
	if (--open_count) {
		dlclose(dl_handle);
		dl_handle = NULL;
	}
	return result;
}

static void initialize(void)
{
	char *s = getenv("OSS_REDIRECTOR");
	if (s) {
		strncpy(hal, s, sizeof(hal));
		hal[sizeof(hal)-1] = '\0';
		if (!strcasecmp(hal, "oss"))
			native_oss = 1;
		else
			native_oss = 0;
	} else {
		native_oss = 1;
	}
	if (native_oss) {
	      __native:
	      	oss_pcm_nonblock = native_pcm_nonblock;
		oss_pcm_read = read;
		oss_pcm_write = write;
		oss_pcm_mmap = mmap;
		oss_pcm_munmap = munmap;
		oss_pcm_ioctl = ioctl;
		oss_pcm_select_prepare = native_pcm_select_prepare;
		oss_pcm_select_result = native_pcm_select_result;
		oss_pcm_poll_fds = native_pcm_poll_fds;
		oss_pcm_poll_prepare = native_pcm_poll_prepare;
		oss_pcm_poll_result = native_pcm_poll_result;
		oss_mixer_ioctl = ioctl;
	} else {
		dl_handle = dlopen(hal, RTLD_NOW);
		if (dl_handle == NULL) {
			fprintf(stderr, "ERROR: dlopen failed for sound (OSS) redirector: %s\n", dlerror());
			fprintf(stderr, "       reverting to native OSS mode\n");
			native_oss = 1;
			goto __native;
		}
		x_oss_pcm_open = dlsym(dl_handle, "lib_oss_pcm_open");
		x_oss_pcm_close = dlsym(dl_handle, "lib_oss_pcm_close");
		oss_pcm_nonblock = dlsym(dl_handle, "lib_oss_pcm_nonblock");
		oss_pcm_read = dlsym(dl_handle, "lib_oss_pcm_read");
		oss_pcm_write = dlsym(dl_handle, "lib_oss_pcm_write");
		oss_pcm_mmap = dlsym(dl_handle, "lib_oss_pcm_mmap");
		oss_pcm_munmap = dlsym(dl_handle, "lib_oss_pcm_munmap");
		oss_pcm_ioctl = dlsym(dl_handle, "lib_oss_pcm_ioctl");
		oss_pcm_select_prepare = dlsym(dl_handle, "lib_oss_select_prepare");
		oss_pcm_select_result = dlsym(dl_handle, "lib_oss_select_result");
		oss_pcm_poll_fds = dlsym(dl_handle, "lib_oss_poll_fds");
		oss_pcm_poll_prepare = dlsym(dl_handle, "lib_oss_poll_prepare");
		oss_pcm_poll_result = dlsym(dl_handle, "lib_oss_poll_result");
		x_oss_mixer_open = dlsym(dl_handle, "lib_oss_mixer_open");
		x_oss_mixer_close = dlsym(dl_handle, "lib_oss_mixer_close");
		oss_mixer_ioctl = dlsym(dl_handle, "lib_oss_mixer_ioctl");
	}
}
