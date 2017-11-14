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
#include <dlfcn.h>
#include <stdio.h>
#include <fcntl.h>
#include <limits.h>
#include <errno.h>
#include <assert.h>
#include <linux/soundcard.h>
#include <alsa/asoundlib.h>

#include "alsa-local.h"

int alsa_oss_debug = 0;
snd_output_t *alsa_oss_debug_out = NULL;

typedef struct {
	snd_pcm_t *pcm;
	snd_pcm_sw_params_t *sw_params;
	size_t frame_bytes;
	struct {
		snd_pcm_uframes_t period_size;
		snd_pcm_uframes_t buffer_size;
		snd_pcm_uframes_t boundary;
		snd_pcm_uframes_t appl_ptr;
		snd_pcm_uframes_t old_hw_ptr;
		size_t mmap_buffer_bytes;
		size_t mmap_period_bytes;
	} alsa;
	struct {
		snd_pcm_uframes_t period_size;
		unsigned int periods;
		snd_pcm_uframes_t buffer_size;
		size_t bytes;
		size_t hw_bytes;
		size_t boundary;
	} oss;
	unsigned int stopped:1;
	void *mmap_buffer;
	size_t mmap_bytes;
	snd_pcm_channel_area_t *mmap_areas;
	snd_pcm_uframes_t mmap_advance;
} oss_dsp_stream_t;

typedef struct {
	int hwset;
	unsigned int channels;
	unsigned int rate;
	unsigned int oss_format;
	snd_pcm_format_t format;
	unsigned int fragshift;
	unsigned int maxfrags;
	unsigned int subdivision;
	oss_dsp_stream_t streams[2];
} oss_dsp_t;

typedef struct fd {
	int fileno;
	oss_dsp_t *dsp;
	void *mmap_area;
	struct fd *next;
} fd_t;

static fd_t *pcm_fds = NULL;


static fd_t *look_for_fd(int fd)
{
	fd_t *result = pcm_fds;
	while (result) {
		if (result->fileno == fd)
			return result;
		result = result->next;
	}
	return NULL;
}

static inline oss_dsp_t *look_for_dsp(int fd)
{
	fd_t *xfd = look_for_fd(fd);
	return xfd ? xfd->dsp : NULL;
}

static inline oss_dsp_t *look_for_mmap_addr(void * addr)
{
	fd_t *result = pcm_fds;
	while (result) {
		if (result->mmap_area == addr)
			return result->dsp ? result->dsp : NULL;
		result = result->next;
	}
	return NULL;
}

static void insert_fd(fd_t *xfd)
{
	xfd->next = pcm_fds;
	pcm_fds = xfd;
}

static void remove_fd(fd_t *xfd)
{
	fd_t *result = pcm_fds, *prev = NULL;
	while (result) {
		if (result == xfd) {
			if (prev == NULL)
				pcm_fds = xfd->next;
			else
				prev->next = xfd->next;
			return;
		}
		prev = result;
		result = result->next;
	}
	assert(0);
}

static unsigned int ld2(u_int32_t v)
{
	unsigned r = 0;

	if (v >= 0x10000) {
		v >>= 16;
		r += 16;
	}
	if (v >= 0x100) {
		v >>= 8;
		r += 8;
	}
	if (v >= 0x10) {
		v >>= 4;
		r += 4;
	}
	if (v >= 4) {
		v >>= 2;
		r += 2;
	}
	if (v >= 2)
		r++;
	return r;
}

static snd_pcm_format_t oss_format_to_alsa(int format)
{
	switch (format) {
	case AFMT_MU_LAW:	return SND_PCM_FORMAT_MU_LAW;
	case AFMT_A_LAW:	return SND_PCM_FORMAT_A_LAW;
	case AFMT_IMA_ADPCM:	return SND_PCM_FORMAT_IMA_ADPCM;
	case AFMT_U8:		return SND_PCM_FORMAT_U8;
	case AFMT_S16_LE:	return SND_PCM_FORMAT_S16_LE;
	case AFMT_S16_BE:	return SND_PCM_FORMAT_S16_BE;
	case AFMT_S8:		return SND_PCM_FORMAT_S8;
	case AFMT_U16_LE:	return SND_PCM_FORMAT_U16_LE;
	case AFMT_U16_BE:	return SND_PCM_FORMAT_U16_BE;
	case AFMT_MPEG:		return SND_PCM_FORMAT_MPEG;
	default:		return SND_PCM_FORMAT_U8;
	}
}

static int alsa_format_to_oss(snd_pcm_format_t format)
{
	switch (format) {
	case SND_PCM_FORMAT_MU_LAW:	return AFMT_MU_LAW;
	case SND_PCM_FORMAT_A_LAW:	return AFMT_A_LAW;
	case SND_PCM_FORMAT_IMA_ADPCM:	return AFMT_IMA_ADPCM;
	case SND_PCM_FORMAT_U8:		return AFMT_U8;
	case SND_PCM_FORMAT_S16_LE:	return AFMT_S16_LE;
	case SND_PCM_FORMAT_S16_BE:	return AFMT_S16_BE;
	case SND_PCM_FORMAT_S8:		return AFMT_S8;
	case SND_PCM_FORMAT_U16_LE:	return AFMT_U16_LE;
	case SND_PCM_FORMAT_U16_BE:	return AFMT_U16_BE;
	case SND_PCM_FORMAT_MPEG:	return AFMT_MPEG;
	default:			return -EINVAL;
	}
}

static int oss_dsp_hw_params(oss_dsp_t *dsp)
{
	int k;
	for (k = 1; k >= 0; --k) {
		oss_dsp_stream_t *str = &dsp->streams[k];
		snd_pcm_t *pcm = str->pcm;
		snd_pcm_hw_params_t *hw;
		int err;
		unsigned int rate, periods_min;
		if (!pcm)
			continue;
		dsp->format = oss_format_to_alsa(dsp->oss_format);
		str->frame_bytes = snd_pcm_format_physical_width(dsp->format) * dsp->channels / 8;
		snd_pcm_hw_params_alloca(&hw);
		snd_pcm_hw_params_any(pcm, hw);

		err = snd_pcm_hw_params_set_format(pcm, hw, dsp->format);
		if (err < 0)
			return err;
		err = snd_pcm_hw_params_set_channels(pcm, hw, dsp->channels);
		if (err < 0)
			return err;
		rate = dsp->rate;
		err = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, 0);
		if (err < 0)
			return err;
#if 0
		err = snd_pcm_hw_params_set_periods_integer(pcm, hw);
		if (err < 0)
			return err;
#endif

		if (str->mmap_buffer) {
			snd_pcm_uframes_t size;
			snd_pcm_access_mask_t *mask;
			snd_pcm_access_mask_alloca(&mask);
			snd_pcm_access_mask_any(mask);
			snd_pcm_access_mask_set(mask, SND_PCM_ACCESS_MMAP_INTERLEAVED);
			snd_pcm_access_mask_set(mask, SND_PCM_ACCESS_MMAP_NONINTERLEAVED);
			snd_pcm_access_mask_set(mask, SND_PCM_ACCESS_MMAP_COMPLEX);
			err = snd_pcm_hw_params_set_access_mask(pcm, hw, mask);
			if (err < 0)
				return err;
			size = str->alsa.mmap_period_bytes / str->frame_bytes;
			err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &size, NULL);
			if (err < 0)
				return err;
			size = str->alsa.mmap_buffer_bytes / str->frame_bytes;
			err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &size);
			if (err < 0)
				return err;
			err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_MMAP_INTERLEAVED);
			if (err < 0)
				return err;
		} else {
			err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
			if (err < 0)
				return err;
			periods_min = 2;
			if (!dsp->maxfrags) {
				err = snd_pcm_hw_params_set_periods_min(pcm, hw, &periods_min, 0);
				if (err < 0)
					return err;
			} else {
				unsigned int periods_max = periods_min > dsp->maxfrags
					? periods_min : dsp->maxfrags;
				err = snd_pcm_hw_params_set_periods_max(pcm, hw,
									&periods_max, 0);
				if (err < 0)
					return err;
			}
			if (dsp->fragshift > 0) {
				snd_pcm_uframes_t s = (1 << dsp->fragshift) / str->frame_bytes;
				s *= 16;
				while (s >= 1024 && (err = snd_pcm_hw_params_set_buffer_size(pcm, hw, s)) < 0)
					s /= 2;
				s = (1 << dsp->fragshift) / str->frame_bytes;
				while (s >= 256 && (err = snd_pcm_hw_params_set_period_size(pcm, hw, s, 0)) < 0)
					s /= 2;
				if (err < 0) {
					s = (1 << dsp->fragshift) / str->frame_bytes;
					err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &s, 0);
				}
			} else {
				snd_pcm_uframes_t s = 16, old_s;
				while (s * 2 < dsp->rate / 2) 
					s *= 2;
				old_s = s = s / 2;
				while (s >= 1024 && (err = snd_pcm_hw_params_set_buffer_size(pcm, hw, s)) < 0)
					s /= 2;
				s = old_s;
				while (s >= 256 && (err = snd_pcm_hw_params_set_period_size(pcm, hw, s, 0)) < 0)
					s /= 2;
				if (err < 0) {
					s = old_s;
					err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &s, 0);
				}
			}
			if (err < 0)
				return err;
		}
		err = snd_pcm_hw_params(pcm, hw);
		if (err < 0)
			return err;
#if 0
		if (alsa_oss_debug && alsa_oss_debug_out)
			snd_pcm_dump_setup(pcm, alsa_oss_debug_out);
#endif
		if (err < 0)
			return err;
		dsp->oss_format = alsa_format_to_oss(dsp->format);
		err = snd_pcm_hw_params_get_period_size(hw, &str->alsa.period_size, 0);
		if (err < 0)
			return err;
		err = snd_pcm_hw_params_get_buffer_size(hw, &str->alsa.buffer_size);
		if (err < 0)
			return err;
		if (str->mmap_buffer == NULL) {
			str->oss.buffer_size = 1 << ld2(str->alsa.buffer_size);
			if (str->oss.buffer_size < str->alsa.buffer_size)
				str->oss.buffer_size *= 2;
			str->oss.period_size = 1 << ld2(str->alsa.period_size);
			if (str->oss.period_size < str->alsa.period_size)
				str->oss.period_size *= 2;
		} else {
			str->oss.buffer_size = str->alsa.mmap_period_bytes / str->frame_bytes;
			str->oss.period_size = str->alsa.mmap_buffer_bytes / str->frame_bytes;
		}
		str->oss.periods = str->oss.buffer_size / str->oss.period_size;
		if (str->mmap_areas)
			free(str->mmap_areas);
		str->mmap_areas = NULL;
		if (str->mmap_buffer) {
			unsigned int c;
			snd_pcm_channel_area_t *a;
			unsigned int bits_per_sample, bits_per_frame;
			str->mmap_areas = calloc(dsp->channels, sizeof(*str->mmap_areas));
			if (!str->mmap_areas)
				return -ENOMEM;
			bits_per_sample = snd_pcm_format_physical_width(dsp->format);
			bits_per_frame = bits_per_sample * dsp->channels;
			a = str->mmap_areas;
			for (c = 0; c < dsp->channels; c++, a++) {
				a->addr = str->mmap_buffer;
				a->first = bits_per_sample * c;
				a->step = bits_per_frame;
			}
		}
		str->oss.hw_bytes = 0;
		str->oss.boundary = (0x3fffffff / str->oss.buffer_size) * str->oss.buffer_size;
		str->alsa.appl_ptr = 0;
		str->alsa.old_hw_ptr = 0;
		str->mmap_advance = str->oss.period_size;
	}
	return 0;
}

static int oss_dsp_sw_params(oss_dsp_t *dsp)
{
	int k;
	for (k = 1; k >= 0; --k) {
		oss_dsp_stream_t *str = &dsp->streams[k];
		snd_pcm_t *pcm = str->pcm;
		snd_pcm_sw_params_t *sw;
		int err;
		if (!pcm)
			continue;
		sw = str->sw_params;
		snd_pcm_sw_params_current(pcm, sw);
		snd_pcm_sw_params_set_xfer_align(pcm, sw, 1);
		snd_pcm_sw_params_set_start_threshold(pcm, sw, 
						      str->stopped ? str->alsa.buffer_size + 1 :
						      str->alsa.period_size);
#if 1
		snd_pcm_sw_params_set_stop_threshold(pcm, sw,
						     str->mmap_buffer ? LONG_MAX :
						     str->alsa.buffer_size);
#else
		snd_pcm_sw_params_set_stop_threshold(pcm, sw,
						     LONG_MAX);
		snd_pcm_sw_params_set_silence_threshold(pcm, sw,
						       str->alsa.period_size);
		snd_pcm_sw_params_set_silence_size(pcm, sw,
						   str->alsa.period_size);
#endif
		err = snd_pcm_sw_params(pcm, sw);
		if (err < 0)
			return err;
		err = snd_pcm_sw_params_current(pcm, sw);
		if (err < 0)
			return err;
		err = snd_pcm_sw_params_get_boundary(sw, &str->alsa.boundary);
		if (err < 0)
			return err;
	}
	return 0;
}

static int oss_dsp_params(oss_dsp_t *dsp)
{
	int err;
	dsp->hwset = 0;
	err = oss_dsp_hw_params(dsp);
	if (err < 0) 
		return err;
	dsp->hwset = 1;
	err = oss_dsp_sw_params(dsp);
	if (err < 0) 
		return err;
#if 0
	if (alsa_oss_debug && alsa_oss_debug_out) {
		int k;
		for (k = 1; k >= 0; --k) {
			oss_dsp_stream_t *str = &dsp->streams[k];
			if (str->pcm)
				snd_pcm_dump(str->pcm, alsa_oss_debug_out);
		}
	}
#endif
	return 0;
}

int lib_oss_pcm_close(int fd)
{
	int result = 0;
	int k;
	fd_t *xfd = look_for_fd(fd);
	oss_dsp_t *dsp;
	
	if (xfd == NULL) {
		errno = ENOENT;
		return -1;
	}
	dsp = xfd->dsp;
	for (k = 0; k < 2; ++k) {
		oss_dsp_stream_t *str = &dsp->streams[k];
		if (str->sw_params)
			snd_pcm_sw_params_free(str->sw_params);
	}
	for (k = 0; k < 2; ++k) {
		int err;
		oss_dsp_stream_t *str = &dsp->streams[k];
		if (!str->pcm)
			continue;
		if (k == SND_PCM_STREAM_PLAYBACK) {
			if (snd_pcm_state(str->pcm) != SND_PCM_STATE_OPEN)
				snd_pcm_drain(str->pcm);
		}
		err = snd_pcm_close(str->pcm);
		if (err < 0)
			result = err;
	}
	remove_fd(xfd);
	free(dsp);
	free(xfd);
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

static int open_pcm(oss_dsp_t *dsp, const char *name, unsigned int pcm_mode,
		    unsigned int streams)
{
	int k, result;

	result = -ENODEV;
	for (k = 0; k < 2; ++k) {
		if (!(streams & (1 << k)))
			continue;
		result = snd_pcm_open(&dsp->streams[k].pcm, name, k, SND_PCM_NONBLOCK);
		DEBUG("Opened PCM %s for stream %d (result = %d)\n", name, k, result);
		if (result < 0) {
			if (k == 1 && dsp->streams[0].pcm != NULL) {
				dsp->streams[1].pcm = NULL;
				streams &= ~(1 << SND_PCM_STREAM_CAPTURE);
				result = 0;
			}
			break;
		} else if (! pcm_mode)
			/* reset the blocking mode */
			snd_pcm_nonblock(dsp->streams[k].pcm, 0);
	}
	return result;
}

static int oss_dsp_open(int card, int device, int oflag, mode_t mode ATTRIBUTE_UNUSED)
{
	oss_dsp_t *dsp;
	unsigned int pcm_mode = 0;
	unsigned int streams, k;
	int format = AFMT_MU_LAW;
	int fd = -1;
	fd_t *xfd;
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
	case OSS_DEVICE_DSP:
		format = AFMT_U8;
		sprintf(name, "dsp%d", card);
		break;
	case OSS_DEVICE_DSPW:
		format = AFMT_S16_LE;
		sprintf(name, "dspW%d", card);
		break;
	case OSS_DEVICE_AUDIO:
		sprintf(name, "audio%d", card);
		break;
	case OSS_DEVICE_ADSP:
		sprintf(name, "adsp%d", card);
		break;
	default:
		errno = ENOENT;
		return -1;
	}
	if (oflag & O_NONBLOCK)
		pcm_mode = SND_PCM_NONBLOCK;
	switch (oflag & O_ACCMODE) {
	case O_RDONLY:
		streams = 1 << SND_PCM_STREAM_CAPTURE;
		break;
	case O_WRONLY:
		streams = 1 << SND_PCM_STREAM_PLAYBACK;
		break;
	case O_RDWR:
		streams = ((1 << SND_PCM_STREAM_PLAYBACK) | 
			   (1 << SND_PCM_STREAM_CAPTURE));
		break;
	default:
		errno = EINVAL;
		return -1;
	}
	fd = open("/dev/null", oflag & O_ACCMODE);
	if (fd < 0)
		return -1;
	xfd = calloc(1, sizeof(fd_t));
	if (!xfd) {
		close(fd);
		errno = ENOMEM;
		return -1;
	}
	dsp = calloc(1, sizeof(oss_dsp_t));
	if (!dsp) {
		result = -ENOMEM;
		goto _error;
	}
	xfd->dsp = dsp;
	dsp->channels = 1;
	dsp->rate = 8000;
	dsp->oss_format = format;
	result = -EINVAL;
	for (k = 0; k < 2; ++k) {
		if (!(streams & (1 << k)))
			continue;
		result = snd_pcm_sw_params_malloc(&dsp->streams[k].sw_params);
		if (result < 0)
			goto _error;
	}
	s = getenv("ALSA_OSS_PCM_DEVICE");
	result = -ENODEV;
	if (s && *s)
		result = open_pcm(dsp, s, pcm_mode, streams);
	if (result < 0)
		result = open_pcm(dsp, name, pcm_mode, streams);
	if (result < 0) {
		/* try to open the default pcm as fallback */
		if (card == 0 && (device == OSS_DEVICE_DSP || device == OSS_DEVICE_AUDIO))
			strcpy(name, "default");
		else
			sprintf(name, "default:%d", card);
		result = open_pcm(dsp, name, pcm_mode, streams);
		if (result < 0)
			goto _error;
	}
	result = oss_dsp_params(dsp);
	if (result < 0) {
		DEBUG("Error setting params\n");
		goto _error;
	}
	xfd->fileno = fd;
	insert_fd(xfd);
	return fd;

 _error:
	for (k = 0; k < 2; ++k) {
		if (dsp->streams[k].pcm)
			snd_pcm_close(dsp->streams[k].pcm);
		if (dsp->streams[k].sw_params)
			snd_pcm_sw_params_free(dsp->streams[k].sw_params);
	}
	close(fd);
	if (xfd->dsp)
		free(xfd->dsp);
	free(xfd);
	errno = -result;
	return -1;
}

static int xrun(snd_pcm_t *pcm)
{
	switch (snd_pcm_state(pcm)) {
	case SND_PCM_STATE_XRUN:
		return snd_pcm_prepare(pcm);
	case SND_PCM_STATE_DRAINING:
		if (snd_pcm_stream(pcm) == SND_PCM_STREAM_CAPTURE)
			return snd_pcm_prepare(pcm);
		break;
	default:
		break;
	}
	return -EIO;
}

static int resume(snd_pcm_t *pcm)
{
	int res;
	while ((res = snd_pcm_resume(pcm)) == -EAGAIN)
		sleep(1);
	if (! res)
		return 0;
	return snd_pcm_prepare(pcm);
}

ssize_t lib_oss_pcm_write(int fd, const void *buf, size_t n)
{
	ssize_t result;
	oss_dsp_t *dsp = look_for_dsp(fd);
	oss_dsp_stream_t *str;
	snd_pcm_t *pcm;
	snd_pcm_uframes_t frames;

	if (dsp == NULL) {
		errno = EBADFD;
		result = -1;
		goto _end;
	}
	str = &dsp->streams[SND_PCM_STREAM_PLAYBACK];
	pcm = str->pcm;
	if (!pcm) {
		errno = EBADFD;
		result = -1;
		goto _end;
	}
	frames = n / str->frame_bytes;
 _again:
	result = snd_pcm_writei(pcm, buf, frames);
	if (result == -EPIPE) {
		if (! (result = xrun(pcm)))
			goto _again;
	} else if (result == -ESTRPIPE) {
		if (! (result = resume(pcm)))
			goto _again;
	}
	if (result < 0) {
		errno = -result;
		result = -1;
		goto _end;
	}
	str->alsa.appl_ptr += result;
	str->alsa.appl_ptr %= str->alsa.boundary;
	result *= str->frame_bytes;
	str->oss.bytes += result;
 _end:
	DEBUG("write(%d, %p, %ld) -> %ld", fd, buf, (long)n, (long)result);
	if (result < 0)
		DEBUG("(errno=%d)\n", errno);
	else
		DEBUG("\n");
	return result;
}

ssize_t lib_oss_pcm_read(int fd, void *buf, size_t n)
{
	ssize_t result;
	oss_dsp_t *dsp = look_for_dsp(fd);
	oss_dsp_stream_t *str;
	snd_pcm_t *pcm;
	snd_pcm_uframes_t frames;

	if (dsp == NULL) {
		errno = EBADFD;
		result = -1;
		goto _end;
	}
	str = &dsp->streams[SND_PCM_STREAM_CAPTURE];
	pcm = str->pcm;
	if (!pcm) {
		errno = EBADFD;
		result = -1;
		goto _end;
	}
	frames = n / str->frame_bytes;
 _again:
	result = snd_pcm_readi(pcm, buf, frames);
	if (result == -EPIPE) {
		if (! (result = xrun(pcm)))
			goto _again;
	} else if (result == -ESTRPIPE) {
		if (! (result = resume(pcm)))
			goto _again;
	}
	if (result < 0) {
		errno = -result;
		result = -1;
		goto _end;
	}
	str->alsa.appl_ptr += result;
	str->alsa.appl_ptr %= str->alsa.boundary;
	result *= str->frame_bytes;
	str->oss.bytes += result;
 _end:
	DEBUG("read(%d, %p, %ld) -> %ld", fd, buf, (long)n, (long)result);
	if (result < 0)
		DEBUG("(errno=%d)\n", errno);
	else
		DEBUG("\n");
	return result;
}

#define USE_REWIND 1

static void oss_dsp_mmap_update(oss_dsp_t *dsp, snd_pcm_stream_t stream,
				snd_pcm_sframes_t delay)
{
	oss_dsp_stream_t *str = &dsp->streams[stream];
	snd_pcm_t *pcm = str->pcm;
	snd_pcm_sframes_t err;
	snd_pcm_uframes_t size;
	const snd_pcm_channel_area_t *areas;
	switch (stream) {
	case SND_PCM_STREAM_PLAYBACK:
		if (delay < 0) {
			str->mmap_advance -= delay;
			if (str->mmap_advance > dsp->rate / 10)
				str->mmap_advance = dsp->rate / 10;
			//fprintf(stderr, "mmap_advance=%ld\n", str->mmap_advance);
			err = snd_pcm_forward(pcm, -delay);
			if (err >= 0) {
				str->alsa.appl_ptr += err;
				str->alsa.appl_ptr %= str->alsa.boundary;
			}
		}
#if USE_REWIND
		err = snd_pcm_rewind(pcm, str->alsa.buffer_size);
		if (err < 0) {
			/* fallback to not very accurate method */
			size = str->mmap_advance - delay;
		} else {
			str->alsa.appl_ptr -= err;
			str->alsa.appl_ptr %= str->alsa.boundary;
			size = str->mmap_advance;
		}
		//fprintf(stderr, "delay=%ld rewind=%ld forward=%ld\n", delay, err, size);
#else
		size = str->mmap_advance - delay;
#endif
		while (size > 0) {
			snd_pcm_uframes_t ofs;
			snd_pcm_uframes_t frames = size;
			snd_pcm_mmap_begin(pcm, &areas, &ofs, &frames);
			if (frames == 0)
				break;
//			fprintf(stderr, "copy %ld %ld %d\n", ofs, frames, dsp->format);
			snd_pcm_areas_copy(areas, ofs, str->mmap_areas,
					   str->alsa.appl_ptr % str->oss.buffer_size, 
					   dsp->channels, frames,
					   dsp->format);
			err = snd_pcm_mmap_commit(pcm, ofs, frames);
			if (err <= 0)
				break;
			size -= err;
			str->alsa.appl_ptr += err;
			str->alsa.appl_ptr %= str->alsa.boundary;
		}
		break;
	case SND_PCM_STREAM_CAPTURE:
		if (delay > (snd_pcm_sframes_t)str->alsa.buffer_size) {
			err = snd_pcm_forward(pcm, delay - str->alsa.buffer_size);
			if (err >= 0) {
				str->alsa.appl_ptr += err;
				str->alsa.appl_ptr %= str->alsa.boundary;
				size = str->alsa.buffer_size;
			} else {
				size = delay;
			}
		} else {
			size = delay;
		}
		while (size > 0) {
			snd_pcm_uframes_t ofs;
			snd_pcm_uframes_t frames = size;
			snd_pcm_mmap_begin(pcm, &areas, &ofs, &frames);
			if (frames == 0)
				break;
			snd_pcm_areas_copy(str->mmap_areas,
					   str->alsa.appl_ptr % str->oss.buffer_size,
					   areas, ofs,
					   dsp->channels, frames,
					   dsp->format);
			err = snd_pcm_mmap_commit(pcm, ofs, frames);
			if (err < 0)
				break;
			size -= err;
			str->alsa.appl_ptr += err;
			str->alsa.appl_ptr %= str->alsa.boundary;
		}
		break;
	}
}

int lib_oss_pcm_ioctl(int fd, unsigned long cmd, ...)
{
	int result, err = 0;
	va_list args;
	void *arg;
	oss_dsp_t *dsp = look_for_dsp(fd);
	oss_dsp_stream_t *str;
	snd_pcm_t *pcm;

	if (dsp == NULL) {
		errno = EBADFD;
		return -1;
	}
	va_start(args, cmd);
	arg = va_arg(args, void *);
	va_end(args);
	DEBUG("ioctl(%d, ", fd);
	switch (cmd) {
	case OSS_GETVERSION:
		*(int*)arg = SOUND_VERSION;
		DEBUG("OSS_GETVERSION, %p) -> [%d]\n", arg, *(int*)arg);
		break;
	case SNDCTL_DSP_RESET:
	{
		int k;
		DEBUG("SNDCTL_DSP_RESET)\n");
		if (!dsp->hwset) {
			errno = -EIO;
			return -1;
		}
		result = 0;
		for (k = 0; k < 2; ++k) {
			str = &dsp->streams[k];
			pcm = str->pcm;
			if (!pcm)
				continue;
			err = snd_pcm_drop(pcm);
			if (err >= 0)
				err = snd_pcm_prepare(pcm);
			if (err < 0)
				result = err;
			str->oss.bytes = 0;
			str->oss.hw_bytes = 0;
			str->alsa.appl_ptr = 0;
			str->alsa.old_hw_ptr = 0;
		}
		err = result;
		break;
	}
	case SNDCTL_DSP_SYNC:
	{
		int k;
		DEBUG("SNDCTL_DSP_SYNC)\n");
		if (!dsp->hwset) {
			errno = -EIO;
			return -1;
		}
		result = 0;
		for (k = 0; k < 2; ++k) {
			str = &dsp->streams[k];
			pcm = str->pcm;
			if (!pcm)
				continue;
			err = snd_pcm_drain(pcm);
			if (err >= 0)
				err = snd_pcm_prepare(pcm);
			if (err < 0)
				result = err;
			str->oss.hw_bytes = 0;
			str->alsa.appl_ptr = 0;
			str->alsa.old_hw_ptr = 0;
		}
		err = result;
		break;
	}
	case SNDCTL_DSP_SPEED:
		dsp->rate = *(int *)arg;
		err = oss_dsp_params(dsp);
		DEBUG("SNDCTL_DSP_SPEED, %p[%d]) -> [%d]\n", arg, *(int *)arg, dsp->rate);
		*(int *)arg = dsp->rate;
		break;
	case SNDCTL_DSP_STEREO:
		if (*(int *)arg)
			dsp->channels = 2;
		else
			dsp->channels = 1;
		err = oss_dsp_params(dsp);
		DEBUG("SNDCTL_DSP_STEREO, %p[%d]) -> [%d]\n", arg, *(int *)arg, dsp->channels - 1);
		*(int *)arg = dsp->channels - 1;
		break;
	case SNDCTL_DSP_CHANNELS:
		dsp->channels = (*(int *)arg);
		err = oss_dsp_params(dsp);
		if (err < 0)
			break;
		DEBUG("SNDCTL_DSP_CHANNELS, %p[%d]) -> [%d]\n", arg, *(int *)arg, dsp->channels);
		*(int *)arg = dsp->channels;
		break;
	case SNDCTL_DSP_SETFMT:
		if (*(int *)arg != AFMT_QUERY) {
			dsp->oss_format = *(int *)arg;
			err = oss_dsp_params(dsp);
			if (err < 0)
				break;
		}
		DEBUG("SNDCTL_DSP_SETFMT, %p[%d]) -> [%d]\n", arg, *(int *)arg, dsp->oss_format);
		*(int *) arg = dsp->oss_format;
		break;
	case SNDCTL_DSP_GETBLKSIZE:
		str = &dsp->streams[SND_PCM_STREAM_PLAYBACK];
		if (!str->pcm)
			str = &dsp->streams[SND_PCM_STREAM_CAPTURE];
		pcm = str->pcm;
		*(int *) arg = str->oss.period_size * str->frame_bytes;
		DEBUG("SNDCTL_DSP_GETBLKSIZE, %p) -> [%d]\n", arg, *(int *)arg);
		break;
	case SNDCTL_DSP_POST:
		DEBUG("SNDCTL_DSP_POST)\n");
		break;
	case SNDCTL_DSP_SUBDIVIDE:
		DEBUG("SNDCTL_DSP_SUBDIVIDE, %p[%d])\n", arg, *(int *)arg);
		dsp->subdivision = *(int *)arg;
		if (dsp->subdivision < 1)
			dsp->subdivision = 1;
		err = oss_dsp_params(dsp);
		break;
	case SNDCTL_DSP_SETFRAGMENT:
	{
		DEBUG("SNDCTL_DSP_SETFRAGMENT, %p[%x])\n", arg, *(int *)arg);
		dsp->fragshift = *(int *)arg & 0xffff;
		if (dsp->fragshift < 4)
			dsp->fragshift = 4;
		dsp->maxfrags = ((*(int *)arg) >> 16) & 0xffff;
		if (dsp->maxfrags < 2)
			dsp->maxfrags = 2;
		err = oss_dsp_params(dsp);
		break;
	}
	case SNDCTL_DSP_GETFMTS:
	{
		*(int *)arg = (AFMT_MU_LAW | AFMT_A_LAW | AFMT_IMA_ADPCM | 
			       AFMT_U8 | AFMT_S16_LE | AFMT_S16_BE | 
			       AFMT_S8 | AFMT_U16_LE | AFMT_U16_BE);
		DEBUG("SNDCTL_DSP_GETFMTS, %p) -> [%d]\n", arg, *(int *)arg);
		break;
	}
	case SNDCTL_DSP_NONBLOCK:
	{	
		DEBUG("SNDCTL_DSP_NONBLOCK)\n");
		return lib_oss_pcm_nonblock(fd, 1);
	}
	case SNDCTL_DSP_GETCAPS:
	{
		result = DSP_CAP_REALTIME | DSP_CAP_TRIGGER | DSP_CAP_MMAP;
		if (dsp->streams[SND_PCM_STREAM_PLAYBACK].pcm && 
		    dsp->streams[SND_PCM_STREAM_CAPTURE].pcm)
			result |= DSP_CAP_DUPLEX;
		*(int*)arg = result;
		DEBUG("SNDCTL_DSP_GETCAPS, %p) -> [%d]\n", arg, *(int*)arg);
		break;
	}
	case SNDCTL_DSP_GETTRIGGER:
	{
		int s = 0;
		pcm = dsp->streams[SND_PCM_STREAM_PLAYBACK].pcm;
		if (pcm) {
			if (snd_pcm_state(pcm) == SND_PCM_STATE_RUNNING)
				s |= PCM_ENABLE_OUTPUT;
		}
		pcm = dsp->streams[SND_PCM_STREAM_CAPTURE].pcm;
		if (pcm) {
			if (snd_pcm_state(pcm) == SND_PCM_STATE_RUNNING)
				s |= PCM_ENABLE_INPUT;
		}
		*(int*)arg = s;
		DEBUG("SNDCTL_DSP_GETTRIGGER, %p) -> [%d]\n", arg, *(int*)arg);
		break;
	}		
	case SNDCTL_DSP_SETTRIGGER:
	{
		DEBUG("SNDCTL_DSP_SETTRIGGER, %p[%d])\n", arg, *(int*)arg);
		result = *(int*) arg;
		str = &dsp->streams[SND_PCM_STREAM_CAPTURE];
		pcm = str->pcm;
		if (pcm) {
			if (result & PCM_ENABLE_INPUT) {
				if (str->stopped) {
					str->stopped = 0;
					err = oss_dsp_sw_params(dsp);
					if (err < 0)
						break;
					err = snd_pcm_start(pcm);
					if (err < 0)
						break;
				}
			} else {
				if (!str->stopped) {
					str->stopped = 1;
					err = snd_pcm_drop(pcm);
					if (err < 0)
						break;
					err = oss_dsp_sw_params(dsp);
					if (err < 0)
						break;
					err = snd_pcm_prepare(pcm);
					if (err < 0)
						break;
				}
			}
		}
		str = &dsp->streams[SND_PCM_STREAM_PLAYBACK];
		pcm = str->pcm;
		if (pcm) {
			if (result & PCM_ENABLE_OUTPUT) {
				if (str->stopped) {
					str->stopped = 0;
					err = oss_dsp_sw_params(dsp);
					if (err < 0)
						break;
					if (str->mmap_buffer) {
						const snd_pcm_channel_area_t *areas;
						snd_pcm_uframes_t offset;
						snd_pcm_uframes_t size = str->alsa.buffer_size;
						ssize_t cres;
						snd_pcm_mmap_begin(pcm, &areas, &offset, &size);
						snd_pcm_areas_copy(areas, 0, str->mmap_areas, 0,
								   dsp->channels, size,
								   dsp->format);
						cres = snd_pcm_mmap_commit(pcm, offset, size);
						if (cres > 0) {
							str->alsa.appl_ptr += cres;
							str->alsa.appl_ptr %= str->alsa.boundary;
						}
					}
					err = snd_pcm_start(pcm);
					if (err < 0)
						break;
				}
			} else {
				if (!str->stopped) {
					str->stopped = 1;
					err = snd_pcm_drop(pcm);
					if (err < 0)
						break;
					err = oss_dsp_sw_params(dsp);
					if (err < 0)
						break;
					err = snd_pcm_prepare(pcm);
					if (err < 0)
						break;
				}
			}
		}
		break;
	}
	case SNDCTL_DSP_GETISPACE:
	{
		snd_pcm_sframes_t avail, delay;
		snd_pcm_state_t state;
		audio_buf_info *info = arg;
		str = &dsp->streams[SND_PCM_STREAM_CAPTURE];
		pcm = str->pcm;
		if (!pcm) {
			err = -EINVAL;
			break;
		}
		state = snd_pcm_state(pcm);
		if (state == SND_PCM_STATE_XRUN) {
			err = xrun(pcm);
			if (err < 0)
				break;
			state = snd_pcm_state(pcm);
		}
		if (state == SND_PCM_STATE_SUSPENDED) {
			err = resume(pcm);
			if (err < 0)
				break;
			state = snd_pcm_state(pcm);
		}
		if (state == SND_PCM_STATE_RUNNING) {
			snd_pcm_delay(pcm, &delay);
			if (str->mmap_buffer)
				oss_dsp_mmap_update(dsp, SND_PCM_STREAM_CAPTURE, delay);
		}
		avail = snd_pcm_avail_update(pcm);
		if (avail < 0)
			avail = 0;
		if ((snd_pcm_uframes_t)avail > str->oss.buffer_size)
			avail = str->oss.buffer_size;
		info->fragsize = str->oss.period_size * str->frame_bytes;
		info->fragstotal = str->oss.periods;
		info->bytes = avail * str->frame_bytes;
		info->fragments = avail / str->oss.period_size;
		DEBUG("SNDCTL_DSP_GETISPACE, %p) -> {%d, %d, %d, %d}\n", arg,
		      info->fragments,
		      info->fragstotal,
		      info->fragsize,
		      info->bytes);
		break;
	}
	case SNDCTL_DSP_GETOSPACE:
	{
		snd_pcm_sframes_t avail, delay;
		snd_pcm_state_t state;
		audio_buf_info *info = arg;
		str = &dsp->streams[SND_PCM_STREAM_PLAYBACK];
		pcm = str->pcm;
		if (!pcm) {
			err = -EINVAL;
			break;
		}
		state = snd_pcm_state(pcm);
		if (state == SND_PCM_STATE_XRUN) {
			err = xrun(pcm);
			if (err < 0)
				break;
			state = snd_pcm_state(pcm);
		}
		if (state == SND_PCM_STATE_SUSPENDED) {
			err = resume(pcm);
			if (err < 0)
				break;
			state = snd_pcm_state(pcm);
		}
		if (state == SND_PCM_STATE_RUNNING || 
		    state == SND_PCM_STATE_DRAINING) {
			snd_pcm_delay(pcm, &delay);
			if (str->mmap_buffer)
				oss_dsp_mmap_update(dsp, SND_PCM_STREAM_PLAYBACK, delay);
		}
		avail = snd_pcm_avail_update(pcm);
		if (avail < 0 || (snd_pcm_uframes_t)avail > str->oss.buffer_size)
			avail = str->oss.buffer_size;
		info->fragsize = str->oss.period_size * str->frame_bytes;
		info->fragstotal = str->oss.periods;
		info->bytes = avail * str->frame_bytes;
		info->fragments = avail / str->oss.period_size;
		DEBUG("SNDCTL_DSP_GETOSPACE, %p) -> {%d %d %d %d}\n", arg,
		      info->fragments,
		      info->fragstotal,
		      info->fragsize,
		      info->bytes);
		break;
	}
	case SNDCTL_DSP_GETIPTR:
	{
		snd_pcm_sframes_t delay = 0, avail, diff;
		snd_pcm_uframes_t hw_ptr;
		snd_pcm_state_t state;
		count_info *info = arg;
		str = &dsp->streams[SND_PCM_STREAM_CAPTURE];
		pcm = str->pcm;
		if (!pcm) {
			err = -EINVAL;
			break;
		}
		state = snd_pcm_state(pcm);
		if (state == SND_PCM_STATE_XRUN) {
			err = xrun(pcm);
			if (err < 0)
				break;
			state = snd_pcm_state(pcm);
		}
		if (state == SND_PCM_STATE_SUSPENDED) {
			err = resume(pcm);
			if (err < 0)
				break;
			state = snd_pcm_state(pcm);
		}
		if (state == SND_PCM_STATE_RUNNING) {
			snd_pcm_delay(pcm, &delay);
			if (str->mmap_buffer)
				oss_dsp_mmap_update(dsp, SND_PCM_STREAM_CAPTURE, delay);
		}
		avail = snd_pcm_avail_update(pcm);
		hw_ptr = (str->alsa.appl_ptr + avail) % str->alsa.boundary;
		diff = hw_ptr - str->alsa.old_hw_ptr;
		if (diff < 0)
			diff += str->alsa.boundary;
		str->oss.hw_bytes += diff;
		str->oss.hw_bytes %= str->oss.boundary;
		info->bytes = (str->oss.hw_bytes * str->frame_bytes) & 0x7fffffff;
		info->ptr = (str->oss.hw_bytes % str->oss.buffer_size) * str->frame_bytes;
		if (str->mmap_buffer) {
			ssize_t n = (hw_ptr / str->oss.period_size) - (str->alsa.old_hw_ptr / str->oss.period_size);
			if (n < 0)
				n += str->alsa.boundary / str->oss.period_size;
			info->blocks = n;
		} else {
			info->blocks = delay / str->oss.period_size;
		}
		str->alsa.old_hw_ptr = hw_ptr;
		DEBUG("SNDCTL_DSP_GETIPTR, %p) -> {%d %d %d}\n", arg,
		      info->bytes,
		      info->blocks,
		      info->ptr);
		break;
	}
	case SNDCTL_DSP_GETOPTR:
	{
		snd_pcm_sframes_t delay = 0, avail, diff;
		snd_pcm_uframes_t hw_ptr;
		snd_pcm_state_t state;
		count_info *info = arg;
		str = &dsp->streams[SND_PCM_STREAM_PLAYBACK];
		pcm = str->pcm;
		if (!pcm) {
			err = -EINVAL;
			break;
		}
		if (state == SND_PCM_STATE_XRUN) {
			err = xrun(pcm);
			if (err < 0)
				break;
			state = snd_pcm_state(pcm);
		}
		state = snd_pcm_state(pcm);
		if (state == SND_PCM_STATE_SUSPENDED) {
			err = resume(pcm);
			if (err < 0)
				break;
			state = snd_pcm_state(pcm);
		}
		if (state == SND_PCM_STATE_RUNNING || 
		    state == SND_PCM_STATE_DRAINING) {
			snd_pcm_delay(pcm, &delay);
			if (str->mmap_buffer)
				oss_dsp_mmap_update(dsp, SND_PCM_STREAM_PLAYBACK, delay);
		}
		avail = snd_pcm_avail_update(pcm);
		hw_ptr = (str->alsa.appl_ptr - (str->alsa.buffer_size - avail)) % str->alsa.boundary;
		diff = hw_ptr - str->alsa.old_hw_ptr;
		if (diff < 0)
			diff += str->alsa.boundary;
		str->oss.hw_bytes += diff;
		str->oss.hw_bytes %= str->oss.boundary;
		info->bytes = (str->oss.hw_bytes * str->frame_bytes) & 0x7fffffff;
		info->ptr = (str->oss.hw_bytes % str->oss.buffer_size) * str->frame_bytes;
		if (str->mmap_buffer) {
			ssize_t n = (hw_ptr / str->oss.period_size) - (str->alsa.old_hw_ptr / str->oss.period_size);
			if (n < 0)
				n += str->alsa.boundary / str->oss.period_size;
			info->blocks = n;
		} else {
			info->blocks = delay / str->oss.period_size;
		}
		str->alsa.old_hw_ptr = hw_ptr;
		DEBUG("SNDCTL_DSP_GETOPTR, %p) -> {%d %d %d}\n", arg,
		      info->bytes,
		      info->blocks,
		      info->ptr);
		break;
	}
	case SNDCTL_DSP_GETODELAY:
	{
		snd_pcm_sframes_t delay = 0;
		snd_pcm_state_t state;
		str = &dsp->streams[SND_PCM_STREAM_PLAYBACK];
		pcm = str->pcm;
		if (!pcm) {
			err = -EINVAL;
			break;
		}
		state = snd_pcm_state(pcm);
		if (state == SND_PCM_STATE_SUSPENDED) {
			err = resume(pcm);
			if (err < 0)
				break;
			state = snd_pcm_state(pcm);
		}
		if (state == SND_PCM_STATE_RUNNING || 
		    state == SND_PCM_STATE_DRAINING) {
			snd_pcm_delay(pcm, &delay);
			if (str->mmap_buffer)
				oss_dsp_mmap_update(dsp, SND_PCM_STREAM_PLAYBACK, delay);
		}
		*(int *)arg = delay * str->frame_bytes;
		DEBUG("SNDCTL_DSP_GETODELAY, %p) -> [%d]\n", arg, *(int*)arg); 
		break;
	}
	case SNDCTL_DSP_SETDUPLEX:
		DEBUG("SNDCTL_DSP_SETDUPLEX)\n"); 
		break;
	case SOUND_PCM_READ_RATE:
	{
		*(int *)arg = dsp->rate;
		DEBUG("SOUND_PCM_READ_RATE, %p) -> [%d]\n", arg, *(int*)arg); 
		break;
	}
	case SOUND_PCM_READ_CHANNELS:
	{
		*(int *)arg = dsp->channels;
		DEBUG("SOUND_PCM_READ_CHANNELS, %p) -> [%d]\n", arg, *(int*)arg); 
		break;
	}
	case SOUND_PCM_READ_BITS:
	{
		*(int *)arg = snd_pcm_format_width(dsp->format);
		DEBUG("SOUND_PCM_READ_BITS, %p) -> [%d]\n", arg, *(int*)arg); 
		break;
	}
	case SNDCTL_DSP_MAPINBUF:
		DEBUG("SNDCTL_DSP_MAPINBUF)\n");
		err = -EINVAL;
		break;
	case SNDCTL_DSP_MAPOUTBUF:
		DEBUG("SNDCTL_DSP_MAPOUTBUF)\n");
		err = -EINVAL;
		break;
	case SNDCTL_DSP_SETSYNCRO:
		DEBUG("SNDCTL_DSP_SETSYNCRO)\n");
		err = -EINVAL;
		break;
	case SOUND_PCM_READ_FILTER:
		DEBUG("SOUND_PCM_READ_FILTER)\n");
		err = -EINVAL;
		break;
	case SOUND_PCM_WRITE_FILTER:
		DEBUG("SOUND_PCM_WRITE_FILTER)\n");
		err = -EINVAL;
		break;
	default:
		DEBUG("%lx, %p)\n", cmd, arg);
		// return oss_mixer_ioctl(...);
		err = -ENXIO;
		break;
	}
	if (err >= 0)
		return 0;
	DEBUG("dsp ioctl error = %d\n", err);
	errno = -err;
	return -1;
}

int lib_oss_pcm_nonblock(int fd, int nonblock)
{
	oss_dsp_t *dsp = look_for_dsp(fd);
	int k;

	if (dsp == NULL) {
		errno = EBADFD;
		return -1;
	}
	for (k = 0; k < 2; ++k) {
		snd_pcm_t *pcm = dsp->streams[k].pcm;
		int err;
		if (!pcm)
			continue;
		err = snd_pcm_nonblock(pcm, nonblock);
		if (err < 0) {
			errno = -err;
			return -1;
		}
	}
	return 0;
}

void * lib_oss_pcm_mmap(void *addr ATTRIBUTE_UNUSED, size_t len ATTRIBUTE_UNUSED, int prot, int flags ATTRIBUTE_UNUSED, int fd, off_t offset ATTRIBUTE_UNUSED)
{
	int err;
	void *result;
	oss_dsp_t *dsp = look_for_dsp(fd);
	oss_dsp_stream_t *str;

	if (dsp == NULL) {
		errno = -EBADFD;
		return MAP_FAILED;
	}
	switch (prot & (PROT_READ | PROT_WRITE)) {
	case PROT_READ:
		str = &dsp->streams[SND_PCM_STREAM_CAPTURE];
		break;
	case PROT_WRITE:
		str = &dsp->streams[SND_PCM_STREAM_PLAYBACK];
		break;
	case PROT_READ | PROT_WRITE:
		str = &dsp->streams[SND_PCM_STREAM_PLAYBACK];
		if (!str->pcm)
			str = &dsp->streams[SND_PCM_STREAM_CAPTURE];
		break;
	default:
		errno = EINVAL;
		result = MAP_FAILED;
		goto _end;
	}
	if (!str->pcm) {
		errno = EBADFD;
		result = MAP_FAILED;
		goto _end;
	}
	assert(!str->mmap_buffer);
	result = malloc(len);
	if (!result) {
		result = MAP_FAILED;
		goto _end;
	}
	str->mmap_buffer = result;
	str->mmap_bytes = len;
	str->alsa.mmap_period_bytes = str->oss.period_size * str->frame_bytes;
	str->alsa.mmap_buffer_bytes = str->oss.buffer_size * str->frame_bytes;
	err = oss_dsp_params(dsp);
	if (err < 0) {
		free(result);
		str->mmap_buffer = NULL;
		str->mmap_bytes = 0;
		errno = -err;
		result = MAP_FAILED;
		goto _end;
	}
 _end:
	DEBUG("mmap(%p, %lu, %d, %d, %d, %ld) -> %p\n", addr, (unsigned long)len, prot, flags, fd, offset, result);
	return result;
}

int lib_oss_pcm_munmap(void *addr, size_t len)
{
	int err;
	oss_dsp_t *dsp = look_for_mmap_addr(addr);
	oss_dsp_stream_t *str;

	if (dsp == NULL) {
		errno = EBADFD;
		return -1;
	}
	DEBUG("munmap(%p, %lu)\n", addr, (unsigned long)len);
	str = &dsp->streams[SND_PCM_STREAM_PLAYBACK];
	if (!str->pcm)
		str = &dsp->streams[SND_PCM_STREAM_CAPTURE];
	assert(str->mmap_buffer);
	free(str->mmap_buffer);
	str->mmap_buffer = 0;
	str->mmap_bytes = 0;
	err = oss_dsp_params(dsp);
	if (err < 0) {
		errno = -err;
		return -1;
	}
	return 0;
}

static void set_oss_mmap_avail_min(oss_dsp_stream_t *str, int stream ATTRIBUTE_UNUSED, snd_pcm_t *pcm)
{
	snd_pcm_uframes_t hw_ptr;
	snd_pcm_sframes_t diff;

	hw_ptr = str->alsa.old_hw_ptr - 
		   (str->alsa.old_hw_ptr % str->oss.period_size) +
		   str->oss.period_size;
	diff = hw_ptr - str->alsa.appl_ptr;
	if (diff < 0)
		diff += str->alsa.buffer_size;
	if (diff < 1)
		diff = 1;
	//fprintf(stderr, "avail_min (%i): hw_ptr = %lu, appl_ptr = %lu, diff = %lu\n", stream, hw_ptr, str->alsa.appl_ptr, diff);
	snd_pcm_sw_params_set_avail_min(pcm, str->sw_params, diff);
	snd_pcm_sw_params(pcm, str->sw_params);
}

int lib_oss_pcm_select_prepare(int fd, int fmode, fd_set *readfds, fd_set *writefds, fd_set *exceptfds)
{
	oss_dsp_t *dsp = look_for_dsp(fd);
	int k, maxfd = -1;

	if (dsp == NULL) {
		errno = EBADFD;
		return -1;
	}
	for (k = 0; k < 2; ++k) {
		oss_dsp_stream_t *str = &dsp->streams[k];
		snd_pcm_t *pcm = str->pcm;
		int err, count;
		if (!pcm)
			continue;
		if ((fmode & O_ACCMODE) == O_RDONLY && snd_pcm_stream(pcm) == SND_PCM_STREAM_PLAYBACK)
			continue;
		if ((fmode & O_ACCMODE) == O_WRONLY && snd_pcm_stream(pcm) == SND_PCM_STREAM_CAPTURE)
			continue;
		if (str->mmap_buffer)
			set_oss_mmap_avail_min(str, k, pcm);
		count = snd_pcm_poll_descriptors_count(pcm);
		if (count < 0) {
			errno = -count;
			return -1;
		}
		{
			struct pollfd ufds[count];
			int j;
			err = snd_pcm_poll_descriptors(pcm, ufds, count);
			if (err < 0) {
				errno = -err;
				return -1;
			}
			for (j = 0; j < count; j++) {
				int fd = ufds[j].fd;
				unsigned short events = ufds[j].events;
				if (maxfd < fd)
					maxfd = fd;
				if (readfds) {
					FD_CLR(fd, readfds);
					if (events & POLLIN)
						FD_SET(fd, readfds);
				}
				if (writefds) {
					FD_CLR(fd, writefds);
					if (events & POLLOUT)
						FD_SET(fd, writefds);
				}
				if (exceptfds) {
					FD_CLR(fd, exceptfds);
					if (events & (POLLERR|POLLNVAL))
						FD_SET(fd, exceptfds);
				}
			}
		}
	}	
	return maxfd;
}

int lib_oss_pcm_select_result(int fd, fd_set *readfds, fd_set *writefds, fd_set *exceptfds)
{
	oss_dsp_t *dsp = look_for_dsp(fd);
	int k, result = 0;

	if (dsp == NULL) {
		errno = EBADFD;
		return -1;
	}
	for (k = 0; k < 2; ++k) {
		snd_pcm_t *pcm = dsp->streams[k].pcm;
		int err, count;
		if (!pcm)
			continue;
		count = snd_pcm_poll_descriptors_count(pcm);
		if (count < 0) {
			errno = -count;
			return -1;
		}
		{
			struct pollfd ufds[count];
			int j;
			unsigned short revents;
			err = snd_pcm_poll_descriptors(pcm, ufds, count);
			if (err < 0) {
				errno = -err;
				return -1;
			}
			for (j = 0; j < count; j++) {
				int fd = ufds[j].fd;
				revents = 0;
				if (readfds && FD_ISSET(fd, readfds))
					revents |= POLLIN;
				if (writefds && FD_ISSET(fd, writefds))
					revents |= POLLOUT;
				if (exceptfds && FD_ISSET(fd, exceptfds))
					revents |= POLLERR;
				ufds[j].revents = revents;
			}
			err = snd_pcm_poll_descriptors_revents(pcm, ufds, count, &revents);
			if (err < 0) {
				errno = -err;
				return -1;
			}
			if (revents & (POLLNVAL|POLLERR))
				result |= OSS_WAIT_EVENT_ERROR;
			if (revents & POLLIN)
				result |= OSS_WAIT_EVENT_READ;
			if (revents & POLLOUT)
				result |= OSS_WAIT_EVENT_WRITE;
		}
	}	
	return result;
}

extern int lib_oss_pcm_poll_fds(int fd)
{
	oss_dsp_t *dsp = look_for_dsp(fd);
	int k, result = 0;

	if (dsp == NULL) {
		errno = EBADFD;
		return -1;
	}
	for (k = 0; k < 2; ++k) {
		snd_pcm_t *pcm = dsp->streams[k].pcm;
		int err;
		if (!pcm)
			continue;
		err = snd_pcm_poll_descriptors_count(pcm);
		if (err < 0) {
			errno = -err;
			return -1;
		}
		result += err;
	}	
	return result;
}

int lib_oss_pcm_poll_prepare(int fd, int fmode, struct pollfd *ufds)
{
	oss_dsp_t *dsp = look_for_dsp(fd);
	int k, result = 0;

	if (dsp == NULL) {
		errno = EBADFD;
		return -1;
	}
	for (k = 0; k < 2; ++k) {
		oss_dsp_stream_t *str = &dsp->streams[k];
		snd_pcm_t *pcm = str->pcm;
		int err, count;
		if (!pcm)
			continue;
		if ((fmode & O_ACCMODE) == O_RDONLY && snd_pcm_stream(pcm) == SND_PCM_STREAM_PLAYBACK)
			continue;
		if ((fmode & O_ACCMODE) == O_WRONLY && snd_pcm_stream(pcm) == SND_PCM_STREAM_CAPTURE)
			continue;
		if (str->mmap_buffer)
			set_oss_mmap_avail_min(str, k, pcm);
		count = snd_pcm_poll_descriptors_count(pcm);
		if (count < 0) {
			errno = -count;
			return -1;
		}
		err = snd_pcm_poll_descriptors(pcm, ufds, count);
		if (err < 0) {
			errno = -err;
			return -1;
		}
		ufds += count;
		result += count;
	}	
	return result;
}

int lib_oss_pcm_poll_result(int fd, struct pollfd *ufds)
{
	oss_dsp_t *dsp = look_for_dsp(fd);
	int k, result = 0;

	if (dsp == NULL) {
		errno = EBADFD;
		return -1;
	}
	for (k = 0; k < 2; ++k) {
		snd_pcm_t *pcm = dsp->streams[k].pcm;
		int err, count;
		unsigned short revents;
		if (!pcm)
			continue;
		count = snd_pcm_poll_descriptors_count(pcm);
		if (count < 0) {
			errno = -count;
			return -1;
		}
		err = snd_pcm_poll_descriptors_revents(pcm, ufds, count, &revents);
		if (err < 0) {
			errno = -err;
			return -1;
		}
		if (revents & (POLLNVAL|POLLERR))
			result |= OSS_WAIT_EVENT_ERROR;
		if (revents & POLLIN)
			result |= OSS_WAIT_EVENT_READ;
		if (revents & POLLOUT)
			result |= OSS_WAIT_EVENT_WRITE;
		ufds += count;
	}	
	return result;
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

int lib_oss_pcm_open(const char *file, int oflag, ...)
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
		if (!strncmp(file, "/dev/dsp", 8))
			minor = (atoi(file + 8) << 4) | OSS_DEVICE_DSP;
		else if (!strncmp(file, "/dev/dspW", 9))
			minor = (atoi(file + 9) << 4) | OSS_DEVICE_DSPW;
		else if (!strncmp(file, "/dev/adsp", 9))
			minor = (atoi(file + 9) << 4) | OSS_DEVICE_ADSP;
		else if (!strncmp(file, "/dev/audio", 10))
			minor = (atoi(file + 10) << 4) | OSS_DEVICE_AUDIO;
		else if (!strncmp(file, "/dev/sound/dsp", 14))
			minor = (atoi(file + 14) << 4) | OSS_DEVICE_DSP;
		else if (!strncmp(file, "/dev/sound/dspW", 15))
			minor = (atoi(file + 15) << 4) | OSS_DEVICE_DSPW;
		else if (!strncmp(file, "/dev/sound/adsp", 15))
			minor = (atoi(file + 15) << 4) | OSS_DEVICE_ADSP;
		else if (!strncmp(file, "/dev/sound/audio", 16))
			minor = (atoi(file + 16) << 4) | OSS_DEVICE_AUDIO;
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
	case OSS_DEVICE_DSP:
	case OSS_DEVICE_DSPW:
	case OSS_DEVICE_AUDIO:
	case OSS_DEVICE_ADSP:
		result = oss_dsp_open(card, device, oflag, mode);
		DEBUG("open(\"%s\", %d, %d) -> %d\n", file, oflag, mode, result);
		return result;
	default:
		errno = ENOENT;
		return -1;
	}
}
