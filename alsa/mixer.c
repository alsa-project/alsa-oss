/*
 *  OSS -> ALSA compatibility layer
 *  Copyright (c) by Abramo Bagnara <abramo@alsa-project.org>
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
#include <dlfcn.h>
#include <stdio.h>
#include <fcntl.h>
#include <limits.h>
#include <errno.h>
#include <assert.h>
#include <linux/soundcard.h>
#include <alsa/asoundlib.h>

#include "alsa-local.h"

typedef struct _oss_mixer {
	int fileno;
	snd_mixer_t *mix;
	unsigned int modify_counter;
	snd_mixer_elem_t *elems[SOUND_MIXER_NRDEVICES];
	struct _oss_mixer *next;
} oss_mixer_t;

static oss_mixer_t *mixer_fds = NULL;

static oss_mixer_t *look_for_fd(int fd)
{
	oss_mixer_t *result = mixer_fds;
	while (result) {
		if (result->fileno == fd)
			return result;
		result = result->next;
	}
	return NULL;
}

static void insert_fd(oss_mixer_t *xfd)
{
	xfd->next = mixer_fds;
	mixer_fds = xfd;
}

static void remove_fd(oss_mixer_t *xfd)
{
	oss_mixer_t *result = mixer_fds, *prev = NULL;
	while (result) {
		if (result == xfd) {
			if (prev == NULL)
				mixer_fds = xfd->next;
			else
				prev->next = xfd->next;
			return;
		}
		prev = result;
		result = result->next;
	}
	assert(0);
}

static int oss_mixer_dev(const char *name, unsigned int index)
{
	static struct {
		char *name;
		unsigned int index;
	} id[SOUND_MIXER_NRDEVICES] = {
		[SOUND_MIXER_VOLUME] = { "Master", 0 },
		[SOUND_MIXER_BASS] = { "Tone Control - Bass", 0 },
		[SOUND_MIXER_TREBLE] = { "Tone Control - Treble", 0 },
		[SOUND_MIXER_SYNTH] = { "Synth", 0 },
		[SOUND_MIXER_PCM] = { "PCM", 0 },
		[SOUND_MIXER_SPEAKER] = { "PC Speaker",	0 },
		[SOUND_MIXER_LINE] = { "Line", 0 },
		[SOUND_MIXER_MIC] = { "Mic", 0 },
		[SOUND_MIXER_CD] = { "CD", 0 },
		[SOUND_MIXER_IMIX] = { "Monitor Mix", 0 },
		[SOUND_MIXER_ALTPCM] = { "PCM",	1 },
		[SOUND_MIXER_RECLEV] = { "-- nothing --", 0 },
		[SOUND_MIXER_IGAIN] = { "Capture", 0 },
		[SOUND_MIXER_OGAIN] = { "Playback", 0 },
		[SOUND_MIXER_LINE1] = { "Aux", 0 },
		[SOUND_MIXER_LINE2] = { "Aux", 1 },
		[SOUND_MIXER_LINE3] = { "Aux", 2 },
		[SOUND_MIXER_DIGITAL1] = { "Digital", 0 },
		[SOUND_MIXER_DIGITAL2] = { "Digital", 1 },
		[SOUND_MIXER_DIGITAL3] = { "Digital", 2 },
		[SOUND_MIXER_PHONEIN] = { "Phone", 0 },
		[SOUND_MIXER_PHONEOUT] = { "Phone", 1 },
		[SOUND_MIXER_VIDEO] = { "Video", 0 },
		[SOUND_MIXER_RADIO] = { "Radio", 0 },
		[SOUND_MIXER_MONITOR] = { "Monitor", 0 },
	};
	unsigned int k;
	for (k = 0; k < SOUND_MIXER_NRDEVICES; ++k) {
		if (index == id[k].index &&
		    strcmp(name, id[k].name) == 0)
			return k;
	}
	return -1;
}

int lib_oss_mixer_close(int fd)
{
	int err, result = 0;
	oss_mixer_t *mixer = look_for_fd(fd);
	err = snd_mixer_close(mixer->mix);
	if (err < 0)
		result = err;
	remove_fd(mixer);
	free(mixer);
	if (result < 0) {
		errno = -result;
		result = -1;
	}
	close(fd);
	DEBUG("close(%d) -> %d", fd, result);
	if (result < 0)
		DEBUG("(errno=%d)\n", errno);
	else
		DEBUG("\n");
	return 0;
}

static int oss_mixer_elem_callback(snd_mixer_elem_t *elem, unsigned int mask)
{
	oss_mixer_t *mixer = snd_mixer_elem_get_callback_private(elem);
	if (mask == SND_CTL_EVENT_MASK_REMOVE) {
		int idx = oss_mixer_dev(snd_mixer_selem_get_name(elem),
					snd_mixer_selem_get_index(elem));
		if (idx >= 0)
			mixer->elems[idx] = 0;
		return 0;
	}
	if (mask & SND_CTL_EVENT_MASK_VALUE) {
		mixer->modify_counter++;
	}
	return 0;
}

static int oss_mixer_callback(snd_mixer_t *mixer, unsigned int mask, 
			      snd_mixer_elem_t *elem)
{
	if (mask & SND_CTL_EVENT_MASK_ADD) {
		oss_mixer_t *mix = snd_mixer_get_callback_private(mixer);
		int idx = oss_mixer_dev(snd_mixer_selem_get_name(elem),
					snd_mixer_selem_get_index(elem));
		if (idx >= 0) {
			mix->elems[idx] = elem;
			snd_mixer_selem_set_playback_volume_range(elem, 0, 100);
			snd_mixer_selem_set_capture_volume_range(elem, 0, 100);
			snd_mixer_elem_set_callback(elem, oss_mixer_elem_callback);
			snd_mixer_elem_set_callback_private(elem, mix);
		}
	}
	return 0;
}

static int oss_mixer_open(int card, int device, int oflag, mode_t mode ATTRIBUTE_UNUSED)
{
	oss_mixer_t *mixer;
	int fd = -1;
	int result;
	char name[64];

	char *s = getenv("ALSA_OSS_DEBUG");
	if (s) {
		alsa_oss_debug = 1;
		if (alsa_oss_debug_out == NULL) {
			if (snd_output_stdio_attach(&alsa_oss_debug_out, stderr, 0) < 0)
				alsa_oss_debug_out = NULL;
		}
	}
	switch (device) {
	case OSS_DEVICE_MIXER:
		sprintf(name, "mixer%d", card);
		break;
	case OSS_DEVICE_AMIXER:
		sprintf(name, "amixer%d", card);
		break;
	default:
		errno = ENODEV;
		return -1;
	}
	switch (oflag & O_ACCMODE) {
	case O_RDONLY:
	case O_WRONLY:
	case O_RDWR:
		break;
	default:
		errno = EINVAL;
		return -1;
	}
	fd = open("/dev/null", oflag & O_ACCMODE);
	assert(fd >= 0);
	mixer = calloc(1, sizeof(oss_mixer_t));
	if (!mixer) {
		errno = -ENOMEM;
		return -1;
	}
	result = snd_mixer_open(&mixer->mix, 0);
	if (result < 0)
		goto _error;
	result = snd_mixer_attach(mixer->mix, name);
	if (result < 0) {
		/* try to open the default mixer as fallback */
		if (card == 0)
			strcpy(name, "default");
		else
			sprintf(name, "hw:%d", card);
		result = snd_mixer_attach(mixer->mix, name);
		if (result < 0)
			goto _error1;
	}
	result = snd_mixer_selem_register(mixer->mix, NULL, NULL);
	if (result < 0)
		goto _error1;
	snd_mixer_set_callback(mixer->mix, oss_mixer_callback);
	snd_mixer_set_callback_private(mixer->mix, mixer);
	result = snd_mixer_load(mixer->mix);
	if (result < 0)
		goto _error1;
	mixer->fileno = fd;
	insert_fd(mixer);
	return fd;
 _error1:
	snd_mixer_close(mixer->mix);
 _error:
	close(fd);
	errno = -result;
	return -1;
}

static int oss_mixer_read_recsrc(oss_mixer_t *mixer, unsigned int *ret)
{
	unsigned int mask = 0;
	unsigned int k;
	int err = 0;
	for (k = 0; k < SOUND_MIXER_NRDEVICES; k++) {
		snd_mixer_elem_t *elem = mixer->elems[k];
		if (elem && 
		    snd_mixer_selem_has_capture_switch(elem)) {
			int sw;
			err = snd_mixer_selem_get_capture_switch(elem, SND_MIXER_SCHN_FRONT_LEFT, &sw);
			if (err < 0)
				break;
			if (sw)
				mask |= 1 << k;
		}
	}
	*ret = mask;
	return err;
}


int lib_oss_mixer_ioctl(int fd, unsigned long cmd, ...)
{
	int err = 0;
	va_list args;
	void *arg;
	oss_mixer_t *mixer = look_for_fd(fd);
	snd_mixer_t *mix;
	unsigned int dev;

	if (mixer == NULL) {
		errno = ENODEV;
		return -1;
	}
	mix = mixer->mix;
	va_start(args, cmd);
	arg = va_arg(args, void *);
	va_end(args);
	DEBUG("ioctl(%d, ", fd);
	switch (cmd) {
	case OSS_GETVERSION:
		*(int*)arg = SOUND_VERSION;
		DEBUG("OSS_GETVERSION, %p) -> [%d]\n", arg, *(int*)arg);
		break;
	case SOUND_MIXER_INFO:
	{
		mixer_info *info = arg;
		snd_mixer_handle_events(mix);
		strcpy(info->id, "alsa-oss");
		strcpy(info->name, "alsa-oss");
		info->modify_counter = mixer->modify_counter;
		DEBUG("SOUND_MIXER_INFO, %p) -> {%s, %s, %d}\n", info, info->id, info->name, info->modify_counter);
		break;
	}
	case SOUND_OLD_MIXER_INFO:
	{
		_old_mixer_info *info = arg;
		strcpy(info->id, "alsa-oss");
		strcpy(info->name, "alsa-oss");
		DEBUG("SOUND_OLD_MIXER_INFO, %p) -> {%s, %s}\n", info, info->id, info->name);
		break;
	}
	case SOUND_MIXER_WRITE_RECSRC:
	{
		unsigned int k, mask = *(unsigned int *) arg;
		unsigned int old;
		int excl = 0;
		DEBUG("SOUND_MIXER_WRITE_RECSRC, %p) -> [%x]", arg, mask);
		err = oss_mixer_read_recsrc(mixer, &old);
		if (err < 0)
			break;
		for (k = 0; k < SOUND_MIXER_NRDEVICES; k++) {
			snd_mixer_elem_t *elem = mixer->elems[k];
			if (elem && 
			    snd_mixer_selem_has_capture_switch(elem)) {
				if (!excl &&
				    snd_mixer_selem_has_capture_switch_exclusive(elem) &&
				    mask & ~old) {
					mask &= ~old;
					excl = 1;
				}
				err = snd_mixer_selem_set_capture_switch_all(elem, !!(mask & 1 << k));
				if (err < 0)
					break;
			}
		}
		if (err < 0)
			break;
		goto __read_recsrc;
	}
	case SOUND_MIXER_READ_RECSRC:
	{
		unsigned int mask;
		DEBUG("SOUND_MIXER_READ_RECSRC, %p) ->", arg);
	__read_recsrc:
		err = oss_mixer_read_recsrc(mixer, &mask);
		*(int *)arg = mask;
		DEBUG(" [%x]\n", mask);
		break;
	}
	case SOUND_MIXER_READ_DEVMASK:
	{
		int k, mask = 0;
		for (k = 0; k < SOUND_MIXER_NRDEVICES; k++) {
			snd_mixer_elem_t *elem = mixer->elems[k];
			if (elem && 
			    (snd_mixer_selem_has_playback_volume(elem) ||
			     snd_mixer_selem_has_capture_volume(elem)))
				mask |= 1 << k;
		}
		*(int *)arg = mask;
		DEBUG("SOUND_MIXER_READ_DEVMASK, %p) -> [%x]\n", arg, mask);
		break;
	}
	case SOUND_MIXER_READ_RECMASK:
	{
		int k, mask = 0;
		for (k = 0; k < SOUND_MIXER_NRDEVICES; k++) {
			snd_mixer_elem_t *elem = mixer->elems[k];
			if (elem &&
			    snd_mixer_selem_has_capture_switch(elem))
				mask |= 1 << k;
		}
		*(int *)arg = mask;
		DEBUG("SOUND_MIXER_READ_RECMASK, %p) -> [%x]\n", arg, mask);
		break;
	}
	case SOUND_MIXER_READ_STEREODEVS:
	{
		int k, mask = 0;
		for (k = 0; k < SOUND_MIXER_NRDEVICES; k++) {
			snd_mixer_elem_t *elem = mixer->elems[k];
			if (elem && 
			    snd_mixer_selem_has_playback_volume(elem) &&
			    !snd_mixer_selem_is_playback_mono(elem))
				mask |= 1 << k;
		}
		*(int *)arg = mask;
		DEBUG("SOUND_MIXER_READ_STEREODEVS, %p) -> [%x]\n", arg, mask);
		break;
	}
	case SOUND_MIXER_READ_CAPS:
	{
		int k;
		*(int *)arg = 0;
		for (k = 0; k < SOUND_MIXER_NRDEVICES; k++) {
			snd_mixer_elem_t *elem = mixer->elems[k];
			if (elem && 
			    snd_mixer_selem_has_capture_switch_exclusive(elem)) {
				* (int*) arg = SOUND_CAP_EXCL_INPUT;
				break;
			}
		}
		DEBUG("SOUND_MIXER_READ_CAPS, %p) -> [%x]\n", arg, *(int*) arg);
		break;
	}
	default:
		if (cmd >= MIXER_WRITE(0) && cmd < MIXER_WRITE(SOUND_MIXER_NRDEVICES)) {
			snd_mixer_elem_t *elem;
			long lvol, rvol;
			dev = cmd & 0xff;
			lvol = *(int *)arg & 0xff;
			if (lvol > 100)
				lvol = 100;
			rvol = (*(int *)arg >> 8) & 0xff;
			if (rvol > 100)
				rvol = 100;
			DEBUG("SOUND_MIXER_WRITE[%d], %p) -> {%ld, %ld}", dev, arg, lvol, rvol);
			elem = mixer->elems[dev];
			if (!elem) {
				err = -EINVAL;
				break;
			}
			if (snd_mixer_selem_has_playback_volume(elem)) {
				err = snd_mixer_selem_set_playback_volume(elem, SND_MIXER_SCHN_FRONT_LEFT, lvol);
				if (err < 0) 
					break;
				if (snd_mixer_selem_is_playback_mono(elem)) {
					if (snd_mixer_selem_has_playback_switch(elem))
						err = snd_mixer_selem_set_playback_switch(elem, SND_MIXER_SCHN_FRONT_LEFT, lvol != 0);
					if (err < 0) 
						break;
				} else {
					err = snd_mixer_selem_set_playback_volume(elem, SND_MIXER_SCHN_FRONT_RIGHT, rvol);
					if (err < 0) 
						break;
					if (snd_mixer_selem_has_playback_switch(elem)) {
						if (snd_mixer_selem_has_playback_switch_joined(elem))
							err = snd_mixer_selem_set_playback_switch(elem, SND_MIXER_SCHN_FRONT_LEFT, lvol != 0 || rvol != 0);
						else {
							err = snd_mixer_selem_set_playback_switch(elem, SND_MIXER_SCHN_FRONT_LEFT, lvol != 0);
							if (err < 0) 
								break;
							err = snd_mixer_selem_set_playback_switch(elem, SND_MIXER_SCHN_FRONT_RIGHT, rvol != 0);
							if (err < 0) 
								break;
						}
					}
				}
			}
			if (snd_mixer_selem_has_capture_volume(elem)) {
				err = snd_mixer_selem_set_capture_volume(elem, SND_MIXER_SCHN_FRONT_LEFT, lvol);
				if (err < 0) 
					break;
				if (!snd_mixer_selem_is_capture_mono(elem)) {
					err = snd_mixer_selem_set_capture_volume(elem, SND_MIXER_SCHN_FRONT_RIGHT, rvol);
					if (err < 0) 
						break;
				}
			}
			goto __read;
		}
		if (cmd >= MIXER_READ(0) && cmd < MIXER_READ(SOUND_MIXER_NRDEVICES)) {
			snd_mixer_elem_t *elem;
			long lvol, rvol;
			int sw;
			dev = cmd & 0xff;
			DEBUG("SOUND_MIXER_READ[%d], %p) ->", dev, arg);
		__read:
			elem = mixer->elems[dev];
			if (!elem) {
				err = -EINVAL;
				break;
			}
			if (snd_mixer_selem_has_playback_volume(elem)) {
				if (snd_mixer_selem_has_playback_switch(elem)) {
					err = snd_mixer_selem_get_playback_switch(elem, SND_MIXER_SCHN_FRONT_LEFT, &sw);
					if (err < 0)
						break;
				} else {
					sw = 1;
				}
				if (sw) {
					err = snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_FRONT_LEFT, &lvol);
					if (err < 0) 
						break;
				} else
					lvol = 0;
				if (snd_mixer_selem_is_playback_mono(elem)) {
					rvol = lvol;
				} else {
					if (snd_mixer_selem_has_playback_switch(elem)) {
						err = snd_mixer_selem_get_playback_switch(elem, SND_MIXER_SCHN_FRONT_RIGHT, &sw);
						if (err < 0)
							break;
					} else {
						sw = 1;
					}
					if (sw) {
						err = snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_FRONT_RIGHT, &rvol);
						if (err < 0) 
							break;
					} else
						rvol = 0;
				}
				* (int*) arg = lvol | (rvol << 8);
				DEBUG("{%ld, %ld}\n", lvol, rvol);
				break;
			}
			if (snd_mixer_selem_has_capture_volume(elem)) {
				err = snd_mixer_selem_get_capture_volume(elem, SND_MIXER_SCHN_FRONT_LEFT, &lvol);
				if (err < 0) 
					break;
				if (!snd_mixer_selem_is_capture_mono(elem)) {
					err = snd_mixer_selem_get_capture_volume(elem, SND_MIXER_SCHN_FRONT_RIGHT, &rvol);
					if (err < 0) 
						break;
				}
				* (int*) arg = lvol | (rvol << 8);
				DEBUG("{%ld, %ld}\n", lvol, rvol);
				break;
			}
		}
		DEBUG("%lx, %p)\n", cmd, arg);
		err = -ENXIO;
		break;
	}
	if (err >= 0)
		return 0;
	errno = -err;
	return -1;
}

static void error_handler(const char *file ATTRIBUTE_UNUSED,
			  int line ATTRIBUTE_UNUSED,
			  const char *func ATTRIBUTE_UNUSED,
			  int err ATTRIBUTE_UNUSED,
			  const char *fmt ATTRIBUTE_UNUSED,
			  ...)
{
	/* suppress the error message from alsa-lib */
}

int lib_oss_mixer_open(const char *file, int oflag, ...)
{
	int result;
	int minor, card, device;
	struct stat s;
	mode_t mode;
	va_list args;
	va_start(args, oflag);
	mode = va_arg(args, mode_t);
	va_end(args);
	result = stat(file, &s);
	if (result < 0) {
		if (!strncmp(file, "/dev/mixer", 10))
			minor = (atoi(file + 10) << 4) | OSS_DEVICE_MIXER;
		else if (!strncmp(file, "/dev/amixer", 11))
			minor = (atoi(file + 11) << 4) | OSS_DEVICE_AMIXER;
		else if (!strncmp(file, "/dev/sound/mixer", 16))
			minor = (atoi(file + 16) << 4) | OSS_DEVICE_MIXER;
		else if (!strncmp(file, "/dev/sound/amixer", 17))
			minor = (atoi(file + 17) << 4) | OSS_DEVICE_AMIXER;
		else {
			errno = ENOENT;
			return -1;
		}
	} else {
		if (!S_ISCHR(s.st_mode) || ((s.st_rdev >> 8) & 0xff) != OSS_MAJOR) {
			errno = ENOENT;
			return -1;
		}
		minor = s.st_rdev & 0xff;
	}
	if (! alsa_oss_debug)
		snd_lib_error_set_handler(error_handler);
	card = minor >> 4;
	device = minor & 0x0f;
	switch (device) {
	case OSS_DEVICE_MIXER:
	case OSS_DEVICE_AMIXER:
		result = oss_mixer_open(card, device, oflag, mode);
		DEBUG("open(\"%s\", %d, %d) -> %d\n", file, oflag, mode, result);
		return result;
	default:
		errno = ENOENT;
		return -1;
	}
}
