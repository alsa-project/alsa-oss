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
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

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
#include <sys/asoundlib.h>

snd_pcm_uframes_t _snd_pcm_boundary(snd_pcm_t *pcm);
snd_pcm_uframes_t _snd_pcm_mmap_hw_ptr(snd_pcm_t *pcm);

#define _GNU_SOURCE

static int debug = 0;

#if __GNUC__ > 2 || (__GNUC__ == 2 && __GNUC_MINOR__ >= 95)
#define NEW_MACRO_VARARGS
#endif

#if 1
#define DEBUG_POLL
#define DEBUG_SELECT
#ifdef NEW_MACRO_VARARGS
#define DEBUG(...) do { if (debug) fprintf(stderr, __VA_ARGS__); } while (0)
#else /* !NEW_MACRO_VARARGS */
#define DEBUG(args...) do { if (debug) fprintf(stderr, ##args); } while (0)
#endif
#else
#ifdef NEW_MACRO_VARARGS
#define DEBUG(...)
#else /* !NEW_MACRO_VARARGS */
#define DEBUG(args...)
#endif
#endif

int (*_select)(int n, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout);
int (*_poll)(struct pollfd *ufds, unsigned int nfds, int timeout);

/* Note that to do a fool proof job we need also to trap:
   fopen, fdopen, freopen, fclose, fwrite, fread, etc.
   I hope that no applications use stdio to access OSS devices */

int (*_open)(const char *file, int oflag, ...);
int (*_close)(int fd);
ssize_t (*_write)(int fd, const void *buf, size_t n);
ssize_t (*_read)(int fd, void *buf, size_t n);
int (*_ioctl)(int fd, unsigned long request, ...);
int (*_fcntl)(int fd, int cmd, ...);
void *(*_mmap)(void *addr, size_t len, int prot, int flags, int fd, off_t offset);
int (*_munmap)(void* addr, size_t len);

typedef struct ops {
	int (*open)(const char *file, int oflag, ...);
	int (*close)(int fd);
	ssize_t (*write)(int fd, const void *buf, size_t n);
	ssize_t (*read)(int fd, void *buf, size_t n);
	int (*ioctl)(int fd, unsigned long request, ...);
	int (*fcntl)(int fd, int cmd, ...);
	void *(*mmap)(void *addr, size_t len, int prot, int flags, int fd, off_t offset);
	int (*munmap)(int fd, void* addr, size_t len);
} ops_t;


typedef struct {
	snd_pcm_t *pcm;
	size_t frame_bytes;
	snd_pcm_uframes_t period_size;
	unsigned int periods;
	snd_pcm_uframes_t buffer_size;
	size_t bytes;
	snd_pcm_uframes_t boundary;
	snd_pcm_uframes_t old_hw_ptr;
	unsigned int mmap:1,
		     disabled:1;
	size_t mmap_bytes;
} oss_dsp_stream_t;

typedef struct {
	int channels;
	int rate;
	int format;
	int fragshift;
	int maxfrags;
	int subdivision;
	oss_dsp_stream_t streams[2];
} oss_dsp_t;


typedef struct {
	snd_mixer_t *mix;
	unsigned int modify_counter;
	snd_mixer_elem_t *elems[SOUND_MIXER_NRDEVICES];
} oss_mixer_t;

typedef enum {
  FD_OSS_DSP,
  FD_OSS_MIXER,
  FD_CLASSES,
} fd_class_t;

static ops_t ops[FD_CLASSES];

typedef struct {
	int count;
	fd_class_t class;
	void *private;
	void *mmap_area;
} fd_t;

static int open_max;
static fd_t **fds;

#define RETRY open_max
#define OSS_MAJOR 14
#define OSS_DEVICE_MIXER 0
#define OSS_DEVICE_SEQUENCER 1
#define OSS_DEVICE_MIDI 2
#define OSS_DEVICE_DSP 3
#define OSS_DEVICE_AUDIO 4
#define OSS_DEVICE_DSPW 5
#define OSS_DEVICE_SNDSTAT 6
#define OSS_DEVICE_MUSIC 8
#define OSS_DEVICE_DMMIDI 9
#define OSS_DEVICE_DMFM 10
#define OSS_DEVICE_AMIXER 11
#define OSS_DEVICE_ADSP 12
#define OSS_DEVICE_AMIDI 13
#define OSS_DEVICE_ADMMIDI 14

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
	switch (snd_enum_to_int(format)) {
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
		snd_pcm_format_t format;
		int err;
		unsigned int periods_min;
		if (!pcm)
			continue;
		snd_pcm_hw_params_alloca(&hw);
		snd_pcm_hw_params_any(pcm, hw);
		if (str->mmap) {
			snd_pcm_uframes_t max = str->mmap_bytes / str->frame_bytes;
			err = snd_pcm_hw_params_set_buffer_size_max(pcm, hw, &max);
			if (err < 0)
				return err;
			err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_MMAP_INTERLEAVED);
		} else
			err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
		if (err < 0)
			return err;
		format = oss_format_to_alsa(dsp->format);

		err = snd_pcm_hw_params_set_format(pcm, hw, format);
		if (err < 0)
			return err;
		err = snd_pcm_hw_params_set_channels(pcm, hw, dsp->channels);
		if (err < 0)
			return err;

		err = snd_pcm_hw_params_set_periods_integer(pcm, hw);
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
		err = snd_pcm_hw_params_set_rate_near(pcm, hw, dsp->rate, 0);
		assert(err >= 0);
		if (dsp->fragshift > 0)
			err = snd_pcm_hw_params_set_period_size_near(pcm, hw, (1 << dsp->fragshift) / str->frame_bytes, 0);
		else
			err = snd_pcm_hw_params_set_period_time_near(pcm, hw, 250000, 0);
		assert(err >= 0);

		err = snd_pcm_hw_params(pcm, hw);
		if (err < 0)
			return err;
#if 0
		if (debug)
			snd_pcm_dump_setup(pcm, stderr);
#endif
		dsp->rate = snd_pcm_hw_params_get_rate(hw, 0);
		dsp->format = alsa_format_to_oss(format);
		str->frame_bytes = snd_pcm_format_physical_width(format) * dsp->channels / 8;
		str->period_size = snd_pcm_hw_params_get_period_size(hw, 0);
		str->periods = snd_pcm_hw_params_get_periods(hw, 0);
		str->buffer_size = str->periods * str->period_size;
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
		snd_pcm_sw_params_set_start_mode(pcm, sw, 
						 str->disabled ? SND_PCM_START_EXPLICIT :
						 SND_PCM_START_DATA);
#if 1
		snd_pcm_sw_params_set_xrun_mode(pcm, sw,
				     str->mmap ? SND_PCM_XRUN_NONE :
				     SND_PCM_XRUN_STOP);
#else
		snd_pcm_sw_params_set_xrun_mode(pcm, sw,
						SND_PCM_XRUN_NONE);
		snd_pcm_sw_params_set_silence_threshold(pcm, sw,
						       str->period_size);
		snd_pcm_sw_params_set_silence_size(pcm, sw,
						   str->period_size);
#endif
		err = snd_pcm_sw_params(pcm, sw);
		if (err < 0)
			return err;
		str->boundary = _snd_pcm_boundary(pcm);
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
	return 0;
}

static int oss_dsp_close(int fd)
{
	int result = 0;
	int k;
	oss_dsp_t *dsp = fds[fd]->private;
	for (k = 0; k < 2; ++k) {
		int err;
		oss_dsp_stream_t *str = &dsp->streams[k];
		if (!str->pcm)
			continue;
		err = snd_pcm_close(str->pcm);
		if (err < 0)
			result = err;
	}
	free(dsp);
	if (result < 0) {
		errno = -result;
		result = -1;
	}
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
	int result;
	char name[64];

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
		return RETRY;
	}
	if (mode & O_NONBLOCK)
		pcm_mode = SND_PCM_NONBLOCK;
	switch (oflag & O_ACCMODE) {
	case O_RDONLY:
		streams = 1 << snd_enum_to_int(SND_PCM_STREAM_CAPTURE);
		break;
	case O_WRONLY:
		streams = 1 << snd_enum_to_int(SND_PCM_STREAM_PLAYBACK);
		break;
	case O_RDWR:
		streams = ((1 << snd_enum_to_int(SND_PCM_STREAM_PLAYBACK)) | 
			   (1 << snd_enum_to_int(SND_PCM_STREAM_CAPTURE)));
		break;
	default:
		errno = EINVAL;
		return -1;
	}
	fd = _open("/dev/null", oflag & O_ACCMODE);
	assert(fd >= 0);
	fds[fd] = calloc(1, sizeof(fd_t));
	fds[fd]->class = FD_OSS_DSP;
	dsp = calloc(1, sizeof(oss_dsp_t));
	if (!dsp) {
		errno = -ENOMEM;
		return -1;
	}
	fds[fd]->private = dsp;
	dsp->channels = 1;
	dsp->rate = 8000;
	dsp->format = format;
	for (k = 0; k < 2; ++k) {
		if (!(streams & (1 << k)))
			continue;
		result = snd_pcm_open(&dsp->streams[k].pcm, name, snd_int_to_enum(k), pcm_mode);
		if (result < 0)
			goto _error;
	}
	result = oss_dsp_params(dsp);
	if (result < 0)
		goto _error;
	return fd;

 _error:
	close(fd);
	errno = -result;
	return -1;
}

int oss_mixer_dev(const char *name, unsigned int index)
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

static int oss_mixer_close(int fd)
{
	int err, result = 0;
	oss_mixer_t *mixer = fds[fd]->private;
	err = snd_mixer_close(mixer->mix);
	if (err < 0)
		result = err;
	free(mixer);
	if (result < 0) {
		errno = -result;
		result = -1;
	}
	DEBUG("close(%d) -> %d", fd, result);
	if (result < 0)
		DEBUG("(errno=%d)\n", errno);
	else
		DEBUG("\n");
	return 0;
}

int mixer_elem_callback(snd_mixer_elem_t *elem, unsigned int mask)
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

int mixer_callback(snd_mixer_t *mixer, unsigned int mask, 
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
			snd_mixer_elem_set_callback(elem, mixer_elem_callback);
			snd_mixer_elem_set_callback_private(elem, snd_mixer_get_callback_private(mixer));
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

	switch (device) {
	case OSS_DEVICE_MIXER:
		sprintf(name, "mixer%d", card);
		break;
	case OSS_DEVICE_AMIXER:
		sprintf(name, "amixer%d", card);
		break;
	default:
		return RETRY;
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
	fd = _open("/dev/null", oflag & O_ACCMODE);
	assert(fd >= 0);
	fds[fd] = calloc(1, sizeof(fd_t));
	fds[fd]->class = FD_OSS_MIXER;
	mixer = calloc(1, sizeof(oss_mixer_t));
	if (!mixer) {
		errno = -ENOMEM;
		return -1;
	}
	fds[fd]->private = mixer;
	result = snd_mixer_open(&mixer->mix);
	if (result < 0)
		goto _error;
	result = snd_mixer_attach(mixer->mix, name);
	if (result < 0)
		goto _error1;
	result = snd_mixer_selem_register(mixer->mix, NULL, NULL);
	if (result < 0)
		goto _error1;
	snd_mixer_set_callback(mixer->mix, mixer_callback);
	snd_mixer_set_callback_private(mixer->mix, mixer);
	result = snd_mixer_load(mixer->mix);
	if (result < 0)
		goto _error1;
	return fd;
 _error1:
	snd_mixer_close(mixer->mix);
 _error:
	close(fd);
	errno = -result;
	return -1;
}

static int oss_open(const char *file, int oflag, ...)
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
	if (result < 0)
		return RETRY;
	if (!S_ISCHR(s.st_mode) || ((s.st_rdev >> 8) & 0xff) != OSS_MAJOR)
		return RETRY;
	minor = s.st_rdev & 0xff;
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
	case OSS_DEVICE_MIXER:
	case OSS_DEVICE_AMIXER:
		result = oss_mixer_open(card, device, oflag, mode);
		DEBUG("open(\"%s\", %d, %d) -> %d\n", file, oflag, mode, result);
		return result;
	default:
		return RETRY;
	}
}

static ssize_t oss_dsp_write(int fd, const void *buf, size_t n)
{
	ssize_t result;
	oss_dsp_t *dsp = fds[fd]->private;
	oss_dsp_stream_t *str = &dsp->streams[snd_enum_to_int(SND_PCM_STREAM_PLAYBACK)];
	snd_pcm_t *pcm = str->pcm;
	snd_pcm_uframes_t frames;
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
	if (result < 0) {
		errno = -result;
		result = -1;
		goto _end;
	}
	result *= str->frame_bytes;
	str->bytes += result;
 _end:
	DEBUG("write(%d, %p, %ld) -> %ld", fd, buf, (long)n, (long)result);
	if (result < 0)
		DEBUG("(errno=%d)\n", errno);
	else
		DEBUG("\n");
	return result;
}

static ssize_t oss_dsp_read(int fd, void *buf, size_t n)
{
	ssize_t result;
	oss_dsp_t *dsp = fds[fd]->private;
	oss_dsp_stream_t *str = &dsp->streams[snd_enum_to_int(SND_PCM_STREAM_CAPTURE)];
	snd_pcm_t *pcm = str->pcm;
	snd_pcm_uframes_t frames;
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
	if (result < 0) {
		errno = -result;
		result = -1;
		goto _end;
	}
	result *= str->frame_bytes;
	str->bytes += result;
 _end:
	DEBUG("read(%d, %p, %ld) -> %ld", fd, buf, (long)n, (long)result);
	if (result < 0)
		DEBUG("(errno=%d)\n", errno);
	else
		DEBUG("\n");
	return result;
}

static int oss_dsp_ioctl(int fd, unsigned long cmd, ...)
{
	int result, err = 0;
	va_list args;
	void *arg;
	oss_dsp_t *dsp = fds[fd]->private;
	oss_dsp_stream_t *str;
	snd_pcm_t *pcm;

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
			str->bytes = 0;
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
			dsp->format = *(int *)arg;
			err = oss_dsp_params(dsp);
			if (err < 0)
				break;
		}
		DEBUG("SNDCTL_DSP_SETFMT, %p[%d]) -> [%d]\n", arg, *(int *)arg, dsp->format);
		*(int *) arg = dsp->format;
		break;
	case SNDCTL_DSP_GETBLKSIZE:
		str = &dsp->streams[snd_enum_to_int(SND_PCM_STREAM_PLAYBACK)];
		if (!str->pcm)
			str = &dsp->streams[snd_enum_to_int(SND_PCM_STREAM_CAPTURE)];
		pcm = str->pcm;
		*(int *) arg = str->period_size * str->frame_bytes;
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
		int k;
		DEBUG("SNDCTL_DSP_NONBLOCK)\n");
		result = 0;
		for (k = 0; k < 2; ++k) {
			pcm = dsp->streams[k].pcm;
			if (!pcm)
				continue;
			err = snd_pcm_nonblock(pcm, 1);
			if (err < 0)
				result = err;
		}
		result = err;
		break;
	}
	case SNDCTL_DSP_GETCAPS:
	{
		result = DSP_CAP_REALTIME | DSP_CAP_TRIGGER | DSP_CAP_MMAP;
		if (dsp->streams[snd_enum_to_int(SND_PCM_STREAM_PLAYBACK)].pcm && 
		    dsp->streams[snd_enum_to_int(SND_PCM_STREAM_CAPTURE)].pcm)
			result |= DSP_CAP_DUPLEX;
		*(int*)arg = result;
		DEBUG("SNDCTL_DSP_GETCAPS, %p) -> [%d]\n", arg, *(int*)arg);
		break;
	}
	case SNDCTL_DSP_GETTRIGGER:
	{
		int s = 0;
		pcm = dsp->streams[snd_enum_to_int(SND_PCM_STREAM_PLAYBACK)].pcm;
		if (pcm) {
			if (snd_pcm_state(pcm) == SND_PCM_STATE_RUNNING)
				s |= PCM_ENABLE_OUTPUT;
		}
		pcm = dsp->streams[snd_enum_to_int(SND_PCM_STREAM_CAPTURE)].pcm;
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
		str = &dsp->streams[snd_enum_to_int(SND_PCM_STREAM_CAPTURE)];
		pcm = str->pcm;
		if (pcm) {
			if (result & PCM_ENABLE_INPUT) {
				if (str->disabled) {
					str->disabled = 0;
					err = oss_dsp_sw_params(dsp);
					if (err < 0)
						break;
					err = snd_pcm_start(pcm);
					if (err < 0)
						break;
				}
			} else {
				if (!str->disabled) {
					str->disabled = 1;
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
		str = &dsp->streams[snd_enum_to_int(SND_PCM_STREAM_PLAYBACK)];
		pcm = str->pcm;
		if (pcm) {
			if (result & PCM_ENABLE_OUTPUT) {
				if (str->disabled) {
					str->disabled = 0;
					err = oss_dsp_sw_params(dsp);
					if (err < 0)
						break;
					err = snd_pcm_start(pcm);
					if (err < 0)
						break;
				}
			} else {
				if (!str->disabled) {
					str->disabled = 1;
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
		audio_buf_info *info = arg;
		str = &dsp->streams[snd_enum_to_int(SND_PCM_STREAM_CAPTURE)];
		pcm = str->pcm;
		if (!pcm) {
			err = -EINVAL;
			break;
		}
		if (snd_pcm_state(pcm) == SND_PCM_STATE_RUNNING) {
			snd_pcm_delay(pcm, &delay);
		}
		avail = snd_pcm_avail_update(pcm);
		if (avail < 0)
			avail = 0;
		info->fragsize = str->period_size * str->frame_bytes;
		info->fragstotal = str->periods;
		info->bytes = avail * str->frame_bytes;
		info->fragments = avail / str->period_size;
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
		audio_buf_info *info = arg;
		str = &dsp->streams[snd_enum_to_int(SND_PCM_STREAM_PLAYBACK)];
		pcm = str->pcm;
		if (!pcm) {
			err = -EINVAL;
			break;
		}
		if (snd_pcm_state(pcm) == SND_PCM_STATE_RUNNING) {
			snd_pcm_delay(pcm, &delay);
		}
		avail = snd_pcm_avail_update(pcm);
		if (avail < 0)
			avail = str->buffer_size;
		info->fragsize = str->period_size * str->frame_bytes;
		info->fragstotal = str->periods;
		info->bytes = avail * str->frame_bytes;
		info->fragments = avail / str->period_size;
		DEBUG("SNDCTL_DSP_GETOSPACE, %p) -> {%d %d %d %d}\n", arg,
		      info->fragments,
		      info->fragstotal,
		      info->fragsize,
		      info->bytes);
		break;
	}
	case SNDCTL_DSP_GETIPTR:
	{
		snd_pcm_sframes_t delay;
		snd_pcm_uframes_t hw_ptr;
		count_info *info = arg;
		str = &dsp->streams[snd_enum_to_int(SND_PCM_STREAM_CAPTURE)];
		pcm = str->pcm;
		if (!pcm) {
			err = -EINVAL;
			break;
		}
		if (snd_pcm_state(pcm) == SND_PCM_STATE_RUNNING) {
			err = snd_pcm_delay(pcm, &delay);
			if (err < 0)
				break;
		} else
			delay = 0;
		/* FIXME */
		hw_ptr = _snd_pcm_mmap_hw_ptr(pcm);
		info->bytes = hw_ptr;
		info->bytes *= str->frame_bytes;
		info->ptr = hw_ptr % str->buffer_size;
		info->ptr *= str->frame_bytes;
		if (str->mmap) {
			ssize_t n = (hw_ptr / str->period_size) - (str->old_hw_ptr / str->period_size);
			if (n < 0)
				n += str->boundary / str->period_size;
			info->blocks = n;
			str->old_hw_ptr = hw_ptr;
		} else
			info->blocks = delay / str->period_size;
		DEBUG("SNDCTL_DSP_GETIPTR, %p) -> {%d %d %d}\n", arg,
		      info->bytes,
		      info->blocks,
		      info->ptr);
		break;
	}
	case SNDCTL_DSP_GETOPTR:
	{
		snd_pcm_sframes_t delay;
		snd_pcm_uframes_t hw_ptr;
		count_info *info = arg;
		str = &dsp->streams[snd_enum_to_int(SND_PCM_STREAM_PLAYBACK)];
		pcm = str->pcm;
		if (!pcm) {
			err = -EINVAL;
			break;
		}
		err = snd_pcm_delay(pcm, &delay);
		if (snd_pcm_state(pcm) == SND_PCM_STATE_RUNNING) {
			if (err < 0)
				break;
		} else
			delay = 0;
		/* FIXME */
		hw_ptr = _snd_pcm_mmap_hw_ptr(pcm);
		info->bytes = hw_ptr;
		info->bytes *= str->frame_bytes;
		info->ptr = hw_ptr % str->buffer_size;
		info->ptr *= str->frame_bytes;
		if (str->mmap) {
			ssize_t n = (hw_ptr / str->period_size) - (str->old_hw_ptr / str->period_size);
			if (n < 0)
				n += str->boundary / str->period_size;
			info->blocks = n;
			str->old_hw_ptr = hw_ptr;
		} else
			info->blocks = delay / str->period_size;
		DEBUG("SNDCTL_DSP_GETOPTR, %p) -> {%d %d %d}\n", arg,
		      info->bytes,
		      info->blocks,
		      info->ptr);
		break;
	}
	case SNDCTL_DSP_GETODELAY:
	{
		snd_pcm_sframes_t delay;
		str = &dsp->streams[snd_enum_to_int(SND_PCM_STREAM_PLAYBACK)];
		pcm = str->pcm;
		if (!pcm) {
			err = -EINVAL;
			break;
		}
		if (snd_pcm_state(pcm) != SND_PCM_STATE_RUNNING ||
		    snd_pcm_delay(pcm, &delay) < 0)
			delay = 0;
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
		*(int *)arg = snd_pcm_format_width(oss_format_to_alsa(dsp->format));
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
	errno = -err;
	return -1;
}

static int oss_dsp_fcntl(int fd, int cmd, ...)
{
	int result;
	va_list args;
	long arg;

	va_start(args, cmd);
	arg = va_arg(args, long);
	va_end(args);
	
	DEBUG("fcntl(%d, ", fd);
	result = _fcntl(fd, cmd, arg);
	if (result < 0)
		return result;
	switch (cmd) {
	case F_DUPFD:
		DEBUG("F_DUPFD, %ld)\n", arg);
		fds[arg] = fds[fd];
		return result;
	case F_SETFL:
	{
		int k;
		int err;
		snd_pcm_t *pcm;
		oss_dsp_t *dsp = fds[fd]->private;
		DEBUG("F_SETFL, %ld)\n", arg);
		for (k = 0; k < 2; ++k) {
			pcm = dsp->streams[k].pcm;
			if (!pcm)
				continue;
			err = snd_pcm_nonblock(pcm, !!(arg & O_NONBLOCK));
			if (err < 0)
				result = err;
		}
		if (result < 0) {
			errno = -result;
			return -1;
		}
		return 0;
	}
	default:
		DEBUG("%x, %ld)\n", cmd, arg);
		return result;
	}
	return -1;
}

static void *oss_dsp_mmap(void *addr ATTRIBUTE_UNUSED, size_t len ATTRIBUTE_UNUSED, int prot, int flags ATTRIBUTE_UNUSED, int fd, off_t offset ATTRIBUTE_UNUSED)
{
	int err;
	void *result;
	oss_dsp_t *dsp = fds[fd]->private;
	oss_dsp_stream_t *str;
	switch (prot & (PROT_READ | PROT_WRITE)) {
	case PROT_READ:
		str = &dsp->streams[snd_enum_to_int(SND_PCM_STREAM_CAPTURE)];
		break;
	case PROT_WRITE:
		str = &dsp->streams[snd_enum_to_int(SND_PCM_STREAM_PLAYBACK)];
		break;
	case PROT_READ | PROT_WRITE:
		str = &dsp->streams[snd_enum_to_int(SND_PCM_STREAM_PLAYBACK)];
		if (!str->pcm)
			str = &dsp->streams[snd_enum_to_int(SND_PCM_STREAM_CAPTURE)];
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
	str->mmap = 1;
	str->mmap_bytes = len;
	err = oss_dsp_params(dsp);
	if (err < 0) {
		errno = -err;
		result = MAP_FAILED;
		goto _end;
	}
	result = snd_pcm_mmap_areas(str->pcm)->addr;
 _end:
	DEBUG("mmap(%p, %lu, %d, %d, %d, %ld) -> %p\n", addr, (unsigned long)len, prot, flags, fd, offset, result);
	return result;
}

static int oss_dsp_munmap(int fd, void *addr ATTRIBUTE_UNUSED, size_t len ATTRIBUTE_UNUSED)
{
	int err;
	oss_dsp_t *dsp = fds[fd]->private;
	oss_dsp_stream_t *str;
	DEBUG("munmap(%p, %lu)\n", addr, (unsigned long)len);
	str = &dsp->streams[snd_enum_to_int(SND_PCM_STREAM_PLAYBACK)];
	if (!str->pcm)
		str = &dsp->streams[snd_enum_to_int(SND_PCM_STREAM_CAPTURE)];
	str->mmap = 0;
	str->mmap_bytes = 0;
	err = oss_dsp_params(dsp);
	if (err < 0) {
		errno = -err;
		return -1;
	}
	return 0;
}

static ssize_t oss_mixer_write(int fd ATTRIBUTE_UNUSED, const void *buf ATTRIBUTE_UNUSED, size_t n ATTRIBUTE_UNUSED)
{
	errno = -EBADFD;
	return -1;
}

static ssize_t oss_mixer_read(int fd ATTRIBUTE_UNUSED, void *buf ATTRIBUTE_UNUSED, size_t n ATTRIBUTE_UNUSED)
{
	errno = -EBADFD;
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


static int oss_mixer_ioctl(int fd, unsigned long cmd, ...)
{
	int err = 0;
	va_list args;
	void *arg;
	oss_mixer_t *mixer = fds[fd]->private;
	snd_mixer_t *mix = mixer->mix;
	unsigned int dev;

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
			    snd_mixer_selem_has_playback_volume(elem))
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
			if (!snd_mixer_selem_has_playback_volume(elem)) {
				err = -EINVAL;
				break;
			}
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
			if (!snd_mixer_selem_has_capture_volume(elem))
				break;
			err = snd_mixer_selem_set_capture_volume(elem, SND_MIXER_SCHN_FRONT_LEFT, lvol);
			if (err < 0) 
				break;
			if (!snd_mixer_selem_is_capture_mono(elem)) {
				err = snd_mixer_selem_set_capture_volume(elem, SND_MIXER_SCHN_FRONT_RIGHT, rvol);
				if (err < 0) 
					break;
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
			if (!snd_mixer_selem_has_playback_volume(elem)) {
				err = -EINVAL;
				break;
			}
			err = snd_mixer_selem_get_playback_switch(elem, SND_MIXER_SCHN_FRONT_LEFT, &sw);
			if (err < 0) 
				break;
			if (sw) {
				err = snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_FRONT_LEFT, &lvol);
				if (err < 0) 
					break;
			} else
				lvol = 0;
			if (snd_mixer_selem_is_playback_mono(elem)) {
				rvol = lvol;
			} else {
				err = snd_mixer_selem_get_playback_switch(elem, SND_MIXER_SCHN_FRONT_RIGHT, &sw);
				if (err < 0) 
					break;
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
		DEBUG("%lx, %p)\n", cmd, arg);
		err = -ENXIO;
		break;
	}
	if (err >= 0)
		return 0;
	errno = -err;
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
	
	DEBUG("fcntl(%d, ", fd);
	result = _fcntl(fd, cmd, arg);
	if (result < 0)
		return result;
	switch (cmd) {
	case F_DUPFD:
		DEBUG("F_DUPFD, %ld)\n", arg);
		fds[arg] = fds[fd];
		return result;
	default:
		DEBUG("%x, %ld)\n", cmd, arg);
		return result;
	}
	return -1;
}

static void *oss_mixer_mmap(void *addr ATTRIBUTE_UNUSED, size_t len ATTRIBUTE_UNUSED, int prot ATTRIBUTE_UNUSED, int flags ATTRIBUTE_UNUSED, int fd ATTRIBUTE_UNUSED, off_t offset ATTRIBUTE_UNUSED)
{
	errno = -EBADFD;
	return MAP_FAILED;
}

static int oss_mixer_munmap(int fd ATTRIBUTE_UNUSED, void *addr ATTRIBUTE_UNUSED, size_t len ATTRIBUTE_UNUSED)
{
	errno = -EBADFD;
	return -1;
}

static ops_t ops[FD_CLASSES] = {
	[FD_OSS_DSP] = {
		open: oss_open,
		close: oss_dsp_close,
		write: oss_dsp_write,
		read: oss_dsp_read,
		ioctl: oss_dsp_ioctl,
		fcntl: oss_dsp_fcntl,
		mmap: oss_dsp_mmap,
		munmap: oss_dsp_munmap,
	},
	[FD_OSS_MIXER] = {
		open: oss_open,
		close: oss_mixer_close,
		write: oss_mixer_write,
		read: oss_mixer_read,
		ioctl: oss_mixer_ioctl,
		fcntl: oss_mixer_fcntl,
		mmap: oss_mixer_mmap,
		munmap: oss_mixer_munmap,
	},
};

int open(const char *file, int oflag, ...)
{
	va_list args;
	mode_t mode = 0;
	int k;
	int fd;

	if (oflag & O_CREAT) {
		va_start(args, oflag);
		mode = va_arg(args, mode_t);
		va_end(args);
	}
	for (k = 0; k < FD_CLASSES; ++k) {
		if (!ops[k].open)
			continue;
		fd = ops[k].open(file, oflag, mode);
		if (fd != RETRY) {
			if (fd >= 0)
				fds[fd]->count++;
			return fd;
		}
	}
	fd = _open(file, oflag, mode);
	if (fd >= 0)
		assert(!fds[fd]);
	return fd;
}

int close(int fd)
{
	int result = _close(fd);
	if (result < 0 || fd < 0 || fd >= open_max || !fds[fd])
		return result;
	if (--fds[fd]->count == 0) {
		int err;
		err = ops[fds[fd]->class].close(fd);
		assert(err >= 0);
		free(fds[fd]);
	}
	fds[fd] = 0;
	return result;
}

ssize_t write(int fd, const void *buf, size_t n)
{
	if (fd < 0 || fd >= open_max || !fds[fd])
		return _write(fd, buf, n);
	else
		return ops[fds[fd]->class].write(fd, buf, n);
}

ssize_t read(int fd, void *buf, size_t n)
{
	if (fd < 0 || fd >= open_max || !fds[fd])
		return _read(fd, buf, n);
	else
		return ops[fds[fd]->class].read(fd, buf, n);
}

int ioctl(int fd, unsigned long request, ...)
{
	va_list args;
	void *arg;

	va_start(args, request);
	arg = va_arg(args, void *);
	va_end(args);
	if (fd < 0 || fd >= open_max || !fds[fd])
		return _ioctl(fd, request, arg);
	else 
		return ops[fds[fd]->class].ioctl(fd, request, arg);
}

int fcntl(int fd, int cmd, ...)
{
	va_list args;
	void *arg;

	va_start(args, cmd);
	arg = va_arg(args, void *);
	va_end(args);
	if (fd < 0 || fd >= open_max || !fds[fd])
		return _fcntl(fd, cmd, arg);
	else
		return ops[fds[fd]->class].fcntl(fd, cmd, arg);
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset)
{
	void *result;
	if (fd < 0 || fd >= open_max || !fds[fd])
		return _mmap(addr, len, prot, flags, fd, offset);
	result = ops[fds[fd]->class].mmap(addr, len, prot, flags, fd, offset);
	if (result != NULL && result != MAP_FAILED)
		fds[fd]->mmap_area = result;
	return result;
}

int munmap(void *addr, size_t len)
{
	int fd;
#if 0
	/* Tricky here: matches snd_pcm_munmap */
	if (errno == 12345)
		return _munmap(addr, len);
#endif
	for (fd = 0; fd < open_max; ++fd) {
		if (fds[fd] && fds[fd]->mmap_area == addr)
			break;
	}
	if (fd >= open_max)
		return _munmap(addr, len);
	fds[fd]->mmap_area = 0;
	return ops[fds[fd]->class].munmap(fd, addr, len);
}

#ifdef DEBUG_POLL
void dump_poll(struct pollfd *pfds, unsigned long nfds, int timeout)
{
	unsigned int k;
	printf("POLL nfds: %ld, timeout: %d\n", nfds, timeout);
	for (k = 0; k < nfds; ++k) {
		printf("fd=%d, events=%x, revents=%x\n", 
		       pfds[k].fd, pfds[k].events, pfds[k].revents);
	}
}
#endif

#ifdef DEBUG_SELECT
void dump_select(int nfds, fd_set *rfds, fd_set *wfds, fd_set *efds,
		 struct timeval *timeout)
{
	int k;
	printf("SELECT nfds: %d, ", nfds);
	if (timeout)
		printf("timeout: %ld.%06ld\n", timeout->tv_sec, timeout->tv_usec);
	else
		printf("no timeout\n");
	if (rfds) {
		printf("rfds: ");
		for (k = 0; k < nfds; ++k) {
			if (FD_ISSET(k, rfds))
				putchar('1');
			else
				putchar('0');
		}
		putchar('\n');
	}
	if (wfds) {
		printf("wfds: ");
		for (k = 0; k < nfds; ++k) {
			if (FD_ISSET(k, wfds))
				putchar('1');
			else
				putchar('0');
		}
		putchar('\n');
	}
	if (efds) {
		printf("efds: ");
		for (k = 0; k < nfds; ++k) {
			if (FD_ISSET(k, efds))
				putchar('1');
			else
				putchar('0');
		}
		putchar('\n');
	}
}
#endif

int snd_pcm_poll_descriptor(snd_pcm_t *pcm)
{
	int err;
	struct pollfd pfds[2];
	err = snd_pcm_poll_descriptors(pcm, pfds, 2);
	assert(err == 1);
	return pfds[0].fd;
}

int poll(struct pollfd *pfds, unsigned long nfds, int timeout)
{
	unsigned int k;
	unsigned int nfds1;
	int count, count1;
	int direct = 1;
	struct pollfd pfds1[nfds * 2];
	nfds1 = 0;
	for (k = 0; k < nfds; ++k) {
		int fd = pfds[k].fd;
		pfds[k].revents = 0;
		if (fd >= open_max || !fds[fd])
			goto _std1;
		switch (fds[fd]->class) {
		case FD_OSS_DSP:
		{
			oss_dsp_t *dsp = fds[fd]->private;
			oss_dsp_stream_t *str;
			int j;
			for (j = 0; j < 2; ++j) {
				str = &dsp->streams[j];
				if (str->pcm) {
					pfds1[nfds1].fd = snd_pcm_poll_descriptor(str->pcm);
					pfds1[nfds1].events = pfds[k].events;
					pfds1[nfds1].revents = 0;
					nfds1++;
				}
			}
			direct = 0;
			break;
		}
		default:
		_std1:
			pfds1[nfds1].fd = pfds[k].fd;
			pfds1[nfds1].events = pfds[k].events;
			pfds1[nfds1].revents = 0;
			nfds1++;
			break;
		}
	}
	if (direct)
		return _poll(pfds, nfds, timeout);
#ifdef DEBUG_POLL
	if (debug) {
		printf("Orig enter ");
		dump_poll(pfds, nfds, timeout);
		printf("Changed enter ");
		dump_poll(pfds1, nfds1, timeout);
	}
#endif
	count = _poll(pfds1, nfds1, timeout);
	if (count <= 0)
		return count;
	nfds1 = 0;
	count1 = 0;
	for (k = 0; k < nfds; ++k) {
		int fd = pfds[k].fd;
		unsigned int revents;
		if (fd >= open_max || !fds[fd])
			goto _std2;
		switch (fds[fd]->class) {
		case FD_OSS_DSP:
		{
			oss_dsp_t *dsp = fds[fd]->private;
			oss_dsp_stream_t *str;
			int j;
			revents = 0;
			for (j = 0; j < 2; ++j) {
				str = &dsp->streams[j];
				if (str->pcm) {
					revents |= pfds1[nfds1].revents;
					nfds1++;
				}
			}
			break;
		}
		default:
		_std2:
			revents = pfds1[nfds1].revents;
			nfds1++;
			break;
		}
		pfds[k].revents = revents;
		if (revents)
			count1++;
	}
#ifdef DEBUG_POLL
	if (debug) {
		printf("Changed exit ");
		dump_poll(pfds1, nfds1, timeout);
		printf("Orig exit ");
		dump_poll(pfds, nfds, timeout);
	}
#endif
	return count1;
}

int select(int nfds, fd_set *rfds, fd_set *wfds, fd_set *efds,
	   struct timeval *timeout)
{
	fd_set _rfds1, _wfds1, _efds1;
	fd_set *rfds1, *wfds1, *efds1;
	int nfds1 = nfds;
	int count, count1;
	int fd;
	int direct = 1;
	if (rfds) {
		_rfds1 = *rfds;
		rfds1 = &_rfds1;
	} else
		rfds1 = NULL;
	if (wfds) {
		_wfds1 = *wfds;
		wfds1 = &_wfds1;
	} else
		wfds1 = NULL;
	if (efds) {
		_efds1 = *efds;
		efds1 = &_efds1;
	} else
		efds1 = NULL;
	for (fd = 0; fd < nfds; ++fd) {
		int r = (rfds && FD_ISSET(fd, rfds));
		int w = (wfds && FD_ISSET(fd, wfds));
		int e = (efds && FD_ISSET(fd, efds));
		if (!(r || w || e))
			continue;
		if (!fds[fd])
			continue;
		switch (fds[fd]->class) {
		case FD_OSS_DSP:
		{
			oss_dsp_t *dsp = fds[fd]->private;
			oss_dsp_stream_t *str;
			int j;
			if (r)
				FD_CLR(fd, rfds1);
			if (w)
				FD_CLR(fd, wfds1);
			if (e)
				FD_CLR(fd, efds1);
			for (j = 0; j < 2; ++j) {
				str = &dsp->streams[j];
				if (str->pcm) {
					int fd1 = snd_pcm_poll_descriptor(str->pcm);
					if (fd1 >= nfds1)
						nfds1 = fd1 + 1;
					if (r)
						FD_SET(fd1, rfds1);
					if (w)
						FD_SET(fd1, wfds1);
					if (e)
						FD_SET(fd1, efds1);
				}
			}
			direct = 0;
			break;
		}
		default:
			break;
		}
	}
	if (direct)
		return _select(nfds, rfds, wfds, efds, timeout);
#ifdef DEBUG_SELECT
	if (debug) {
		printf("Orig enter ");
		dump_select(nfds, rfds, wfds, efds, timeout);
		printf("Changed enter ");
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
	count1 = 0;
	for (fd = 0; fd < nfds; ++fd) {
		int r = (rfds && FD_ISSET(fd, rfds));
		int w = (wfds && FD_ISSET(fd, wfds));
		int e = (efds && FD_ISSET(fd, efds));
		int r1, w1, e1;
		if (!(r || w || e))
			continue;
		if (!fds[fd])
			continue;
		switch (fds[fd]->class) {
		case FD_OSS_DSP:
		{
			oss_dsp_t *dsp = fds[fd]->private;
			oss_dsp_stream_t *str;
			int j;
			r1 = w1 = e1 = 0;
			for (j = 0; j < 2; ++j) {
				str = &dsp->streams[j];
				if (str->pcm) {
					int fd1 = snd_pcm_poll_descriptor(str->pcm);
					if (r && FD_ISSET(fd1, rfds1))
						r1++;
					if (w && FD_ISSET(fd1, wfds1))
						w1++;
					if (e && FD_ISSET(fd1, efds1))
						e1++;
				}
			}
			break;
		}
		default:
			r1 = (r && FD_ISSET(fd, rfds1));
			w1 = (w && FD_ISSET(fd, wfds1));
			e1 = (e && FD_ISSET(fd, efds1));
			break;
		}
		if (r && !r1)
			FD_CLR(fd, rfds);
		if (w && !w1)
			FD_CLR(fd, wfds);
		if (e && !e1)
			FD_CLR(fd, efds);
		if (r1 || w1 || e1)
			count1++;
	}
#ifdef DEBUG_SELECT
	if (debug) {
		printf("Changed exit ");
		dump_select(nfds1, rfds1, wfds1, efds1, timeout);
		printf("Orig exit ");
		dump_select(nfds, rfds, wfds, efds, timeout);
	}
#endif
	return count1;
}


int dup(int fd)
{
	return fcntl(fd, F_DUPFD, 0);
}

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

#ifndef O_LARGEFILE
#define O_LARGEFILE 0100000
#endif

int open64(const char *file, int oflag, ...)
{
	va_list args;
	mode_t mode = 0;

	if (oflag & O_CREAT) {
		va_start(args, oflag);
		mode = va_arg(args, mode_t);
		va_end(args);
	}
	return open(file, oflag | O_LARGEFILE, mode);
}

static void initialize() __attribute__ ((constructor));

static void initialize()
{
	char *s = getenv("ALSA_OSS_DEBUG");
	if (s)
		debug = 1;
	open_max = sysconf(_SC_OPEN_MAX);
	if (open_max < 0)
		exit(1);
	fds = calloc(open_max, sizeof(*fds));
	if (!fds)
		exit(1);
	_open = dlsym(RTLD_NEXT, "open");
	_close = dlsym(RTLD_NEXT, "close");
	_write = dlsym(RTLD_NEXT, "write");
	_read = dlsym(RTLD_NEXT, "read");
	_ioctl = dlsym(RTLD_NEXT, "ioctl");
	_fcntl = dlsym(RTLD_NEXT, "fcntl");
	_mmap = dlsym(RTLD_NEXT, "mmap");
	_munmap = dlsym(RTLD_NEXT, "munmap");
	_select = dlsym(RTLD_NEXT, "select");
	_poll = dlsym(RTLD_NEXT, "poll");
}

