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
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>
#include <linux/soundcard.h>

static int initialized = 0;
static int native_oss = 1;
static char hal[64];

int (*oss_pcm_open)(const char *pathname, int flags, ...);
int (*oss_pcm_close)(int fd);
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

int (*oss_mixer_open)(const char *pathname, int flags, ...);
int (*oss_mixer_close)(int fd);
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

static void initialize()
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
		oss_pcm_open = open;
		oss_pcm_close = close;
		oss_pcm_read  = read;
		oss_pcm_write = write;
		oss_pcm_mmap = mmap;
		oss_pcm_munmap = munmap;
		oss_pcm_ioctl = ioctl;
		oss_pcm_select_prepare = native_pcm_select_prepare;
		oss_pcm_select_result = native_pcm_select_result;
		oss_pcm_poll_fds = native_pcm_poll_fds;
		oss_pcm_poll_prepare = native_pcm_poll_prepare;
		oss_pcm_poll_result = native_pcm_poll_result;
		oss_mixer_open = open;
		oss_mixer_close = close;
		oss_mixer_ioctl = ioctl;
	}
}

static inline void check_initialized(void)
{
	if (!initialized)
		initialize();
}
