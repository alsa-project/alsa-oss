/*
 *  OSS Redirector
 *  Copyright (c) by Jaroslav Kysela <perex@suse.cz>
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
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
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
ssize_t (*oss_pcm_read)(int fd, void *buf, size_t count);
ssize_t (*oss_pcm_write)(int fd, const void *buf, size_t count);
void * (*oss_pcm_mmap)(void *start, size_t length, int prot, int flags, int fd, off_t offset);
int (*oss_pcm_munmap)(void *start, size_t length);
int (*oss_pcm_ioctl)(int fd, unsigned long int request, ...);
int (*oss_pcm_select_prepare)(int fd, fd_set *readfds, fd_set *writefds);
int (*oss_pcm_select_result)(int fd, fd_set *readfds, fd_set *writefds);
int (*oss_pcm_poll_fds)(int fd);
int (*oss_pcm_poll_prepare)(int fd, struct pollfd *ufds);
int (*oss_pcm_poll_result)(int fd, struct pollfd *ufds);

static int (*x_oss_mixer_open)(const char *pathname, int flags);
static int (*x_oss_mixer_close)(int fd);
int (*oss_mixer_ioctl)(int fd, unsigned long int request, ...);

int native_pcm_select_prepare(int fd, fd_set *readfds, fd_set *writefds)
{
	if (fd < 0)
		return -EINVAL;
	if (readfds)
		FD_SET(fd, readfds);
	if (writefds)
		FD_SET(fd, writefds);
	return 0;
}

int native_pcm_select_result(int fd, fd_set *readfds, fd_set *writefds)
{
	int result = 0;

	if (fd < 0)
		return -EINVAL;
	if (readfds && FD_ISSET(fd, readfds))
		result |= OSS_WAIT_EVENT_READ;
	if (writefds && FD_ISSET(fd, writefds))
		result |= OSS_WAIT_EVENT_WRITE;
	return result;
}

int native_pcm_poll_fds(int fd)
{
	if (fd < 0)
		return -EINVAL;
	return 1;
}

int native_pcm_poll_prepare(int fd, struct pollfd *ufds)
{
	if (fd < 0)
		return -EINVAL;
	ufds->fd = fd;
	ufds->events = POLLIN | POLLOUT | POLLERR;
	return 0;
}

int native_pcm_poll_result(int fd, struct pollfd *ufds)
{
	int result = 0;

	if (fd < 0)
		return -EINVAL;
	if (ufds->events & POLLIN)
		result |= OSS_WAIT_EVENT_READ;
	if (ufds->events & POLLOUT)
		result |= OSS_WAIT_EVENT_WRITE;
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
	if (--open_count)
		dlclose(dl_handle);
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
		if (!strcmp(hal, "oss"))
			native_oss = 1;
	} else {
		native_oss = 1;
	}
	if (native_oss) {
	      __native:
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
