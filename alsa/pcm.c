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
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
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

#include "alsa-oss-emul.h"

snd_pcm_uframes_t _snd_pcm_boundary(snd_pcm_t *pcm);
snd_pcm_uframes_t _snd_pcm_mmap_hw_ptr(snd_pcm_t *pcm);

int alsa_oss_debug = 0;
snd_output_t *alsa_oss_debug_out = NULL;

typedef struct {
	snd_pcm_t *pcm;
	size_t frame_bytes;
	struct {
		snd_pcm_uframes_t period_size;
		snd_pcm_uframes_t buffer_size;
		snd_pcm_uframes_t boundary;
		snd_pcm_uframes_t old_hw_ptr;
		size_t mmap_buffer_bytes;
		size_t mmap_period_bytes;
	} alsa;
	struct {
		snd_pcm_uframes_t period_size;
		unsigned int periods;
		snd_pcm_uframes_t buffer_size;
		size_t bytes;
	} oss;
	unsigned int stopped:1;
	void *mmap_buffer;
	size_t mmap_bytes;
	snd_pcm_channel_area_t *mmap_areas;
	snd_pcm_uframes_t mmap_advance;
} oss_dsp_stream_t;

typedef struct {
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
		str->frame_bytes = snd_pcm_format_physical_width(dsp->format) * dsp->channels / 8;
		snd_pcm_hw_params_alloca(&hw);
		snd_pcm_hw_params_any(pcm, hw);
		dsp->format = oss_format_to_alsa(dsp->oss_format);

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
			snd_pcm_access_mask_t *mask;
			snd_pcm_access_mask_alloca(&mask);
			snd_pcm_access_mask_any(mask);
			snd_pcm_access_mask_set(mask, SND_PCM_ACCESS_MMAP_INTERLEAVED);
			snd_pcm_access_mask_set(mask, SND_PCM_ACCESS_MMAP_NONINTERLEAVED);
			snd_pcm_access_mask_set(mask, SND_PCM_ACCESS_MMAP_COMPLEX);
			err = snd_pcm_hw_params_set_access_mask(pcm, hw, mask);
			if (err < 0)
				return err;
			err = snd_pcm_hw_params_set_period_size(pcm, hw, str->alsa.mmap_period_bytes / str->frame_bytes, 0);
			if (err < 0)
				return err;
			err = snd_pcm_hw_params_set_buffer_size(pcm, hw, str->alsa.mmap_buffer_bytes / str->frame_bytes);
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
			err = snd_pcm_hw_params_set_periods_min(pcm, hw, &periods_min, 0);
			if (err < 0)
				return err;
			if (dsp->maxfrags > 0) {
				unsigned int periods_max = dsp->maxfrags;
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
		if (alsa_oss_debug)
			snd_pcm_dump_setup(pcm, stderr);
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
		str->oss.buffer_size = 1 << ld2(str->alsa.buffer_size);
		if (str->oss.buffer_size < str->alsa.buffer_size)
			str->oss.buffer_size *= 2;
		str->oss.period_size = 1 << ld2(str->alsa.period_size);
		if (str->oss.period_size < str->alsa.period_size)
			str->oss.period_size *= 2;
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
		snd_pcm_sw_params_alloca(&sw);
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
		str->alsa.boundary = _snd_pcm_boundary(pcm);
	}
	return 0;
}

static int oss_dsp_params(oss_dsp_t *dsp)
{
	int err;
	err = oss_dsp_hw_params(dsp);
	if (err < 0) 
		return err;
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

static int oss_dsp_open(int card, int device, int oflag, mode_t mode)
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
	if (mode & O_NONBLOCK)
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
		close(fd);
		free(xfd);
		errno = ENOMEM;
		return -1;
	}
	xfd->dsp = dsp;
	dsp->channels = 1;
	dsp->rate = 8000;
	dsp->oss_format = format;
	result = -EINVAL;
	for (k = 0; k < 2; ++k) {
		if (!(streams & (1 << k)))
			continue;
		result = snd_pcm_open(&dsp->streams[k].pcm, name, k, pcm_mode);
		if (result < 0)
			break;
	}
	if (result < 0) {
		result = 0;
		for (k = 0; k < 2; ++k) {
			if (dsp->streams[k].pcm) {
				snd_pcm_close(dsp->streams[k].pcm);
				dsp->streams[k].pcm = NULL;
			}
		}
		/* try to open the default pcm as fallback */
		if (card == 0 && (device == OSS_DEVICE_DSP || device == OSS_DEVICE_AUDIO))
			strcpy(name, "default");
		else
			sprintf(name, "plughw:%d", card);
		for (k = 0; k < 2; ++k) {
			if (!(streams & (1 << k)))
				continue;
			result = snd_pcm_open(&dsp->streams[k].pcm, name, k, pcm_mode);
			if (result < 0)
				goto _error;
		}
	}
	result = oss_dsp_params(dsp);
	if (result < 0)
		goto _error;
	xfd->fileno = result;
	insert_fd(xfd);
	return fd;

 _error:
	close(fd);
	errno = -result;
	return -1;
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
	if (result == -EPIPE && 
	    snd_pcm_state(pcm) == SND_PCM_STATE_XRUN &&
	    (result = snd_pcm_prepare(pcm)) == 0)
		goto _again;
	if (result == -EPIPE && 
	    snd_pcm_state(pcm) == SND_PCM_STATE_SUSPENDED) {
	    	while ((result = snd_pcm_resume(pcm)) == -EAGAIN)
	    		sleep(1);
	    	if (result < 0 && (result = snd_pcm_prepare(pcm)) == 0)
	    		goto _again;
	}
	if (result < 0) {
		errno = -result;
		result = -1;
		goto _end;
	}
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
	if (result == -EPIPE && 
	    snd_pcm_state(pcm) == SND_PCM_STATE_XRUN &&
	    (result = snd_pcm_prepare(pcm)) == 0)
		goto _again;
	if (result == -EPIPE && 
	    snd_pcm_state(pcm) == SND_PCM_STATE_SUSPENDED) {
	    	while ((result = snd_pcm_resume(pcm)) == -EAGAIN)
	    		sleep(1);
	    	if (result < 0 && (result = snd_pcm_prepare(pcm)) == 0)
	    		goto _again;
	}
	if (result < 0) {
		errno = -result;
		result = -1;
		goto _end;
	}
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
			snd_pcm_reset(pcm);
			str->mmap_advance -= delay;
			if (str->mmap_advance > dsp->rate / 10)
				str->mmap_advance = dsp->rate / 10;
//			fprintf(stderr, "mmap_advance=%ld\n", str->mmap_advance);
		}
#if USE_REWIND
		err = snd_pcm_rewind(pcm, str->alsa.buffer_size);
		if (err < 0)
			return;
		size = str->mmap_advance;
//		fprintf(stderr, "delay=%ld rewind=%ld forward=%ld offset=%ld\n",
//			delay, err, size, snd_pcm_mmap_offset(pcm));
#else
		size = str->mmap_advance - delay;
#endif
		while (size > 0) {
			snd_pcm_uframes_t ofs;
			snd_pcm_uframes_t frames = size;
			snd_pcm_mmap_begin(pcm, &areas, &ofs, &frames);
//			fprintf(stderr, "copy %ld %ld %d\n", ofs, frames, dsp->format);
			snd_pcm_areas_copy(areas, ofs, str->mmap_areas, ofs, 
					   dsp->channels, frames,
					   dsp->format);
			err = snd_pcm_mmap_commit(pcm, ofs, frames);
			assert(err == (snd_pcm_sframes_t) frames);
			size -= frames;
		}
		break;
	case SND_PCM_STREAM_CAPTURE:
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
		}
		err = result;
		break;
	}
	case SNDCTL_DSP_SYNC:
	{
		int k;
		DEBUG("SNDCTL_DSP_SYNC)\n");
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
						snd_pcm_mmap_begin(pcm, &areas, &offset, &size);
						snd_pcm_areas_copy(areas, 0, str->mmap_areas, 0,
								   dsp->channels, size,
								   dsp->format);
						snd_pcm_mmap_commit(pcm, offset, size);
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
		snd_pcm_sframes_t delay = 0;
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
		if (state == SND_PCM_STATE_RUNNING) {
			snd_pcm_delay(pcm, &delay);
			if (str->mmap_buffer)
				oss_dsp_mmap_update(dsp, SND_PCM_STREAM_CAPTURE, delay);
		}
		/* FIXME */
		hw_ptr = _snd_pcm_mmap_hw_ptr(pcm);
		info->bytes = hw_ptr;
		info->bytes *= str->frame_bytes;
		info->ptr = hw_ptr % str->oss.buffer_size;
		info->ptr *= str->frame_bytes;
		if (str->mmap_buffer) {
			ssize_t n = (hw_ptr / str->oss.period_size) - (str->alsa.old_hw_ptr / str->oss.period_size);
			if (n < 0)
				n += str->alsa.boundary / str->oss.period_size;
			info->blocks = n;
			str->alsa.old_hw_ptr = hw_ptr;
		} else
			info->blocks = delay / str->oss.period_size;
		DEBUG("SNDCTL_DSP_GETIPTR, %p) -> {%d %d %d}\n", arg,
		      info->bytes,
		      info->blocks,
		      info->ptr);
		break;
	}
	case SNDCTL_DSP_GETOPTR:
	{
		snd_pcm_sframes_t delay = 0;
		snd_pcm_uframes_t hw_ptr;
		snd_pcm_state_t state;
		count_info *info = arg;
		str = &dsp->streams[SND_PCM_STREAM_PLAYBACK];
		pcm = str->pcm;
		if (!pcm) {
			err = -EINVAL;
			break;
		}
		state = snd_pcm_state(pcm);
		if (state == SND_PCM_STATE_RUNNING || 
		    state == SND_PCM_STATE_DRAINING) {
			snd_pcm_delay(pcm, &delay);
			if (str->mmap_buffer)
				oss_dsp_mmap_update(dsp, SND_PCM_STREAM_PLAYBACK, delay);
		}
		/* FIXME */
		hw_ptr = _snd_pcm_mmap_hw_ptr(pcm);
		info->bytes = hw_ptr;
		info->bytes *= str->frame_bytes;
		info->ptr = hw_ptr % str->oss.buffer_size;
		info->ptr *= str->frame_bytes;
		if (str->mmap_buffer) {
			ssize_t n = (hw_ptr / str->oss.period_size) - (str->alsa.old_hw_ptr / str->oss.period_size);
			if (n < 0)
				n += str->alsa.boundary / str->oss.period_size;
			info->blocks = n;
			str->alsa.old_hw_ptr = hw_ptr;
		} else
			info->blocks = delay / str->oss.period_size;
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
		return NULL;
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