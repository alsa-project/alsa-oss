/*
 *  OSS -> ALSA sequencer compatibility layer
 *  Copyright (c) by Takashi Iwai <tiwai@suse.de>
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
 */

#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <alsa/asoundlib.h>
#include <linux/soundcard.h>
#include "alsa-local.h"

/* OSS sequencer modes */
#define OSS_SEQ_MODE_SYNTH  0   /* /dev/sequencer */
#define OSS_SEQ_MODE_MUSIC  1   /* /dev/music */

/* OSS event sizes */
#define SHORT_EVENT_SIZE 4
#define LONG_EVENT_SIZE  8

/* Read queue size (events) */
#define READQ_SIZE 1024

/* SEQ_EXTENDED sub-command indices (0-7, not MIDI status bytes) */
#define SEQ_EXT_NOTEOFF   0
#define SEQ_EXT_NOTEON    1
#define SEQ_EXT_PGMCHANGE 3

/* OSS event record union (matches kernel layout) */
union evrec {
	struct { unsigned char code, parm1, dev, parm2; } s;
	struct { unsigned char code, chn, note, vel; } n;
	struct { unsigned char code, dev, cmd, chn, note, parm; unsigned short dummy; } v;
	struct { unsigned char code, dev, cmd, chn, p1, p2, p3, p4; } e;
	struct { unsigned char code, dev, cmd, chn, p1, p2; short val; } l;
	struct { unsigned char code, cmd, dummy1, dummy2; unsigned int time; } t;
	struct { unsigned char code, dev; unsigned char buf[6]; } x;
	unsigned int echo;
	unsigned char c[8];
};

#define ev_is_long(ev) ((ev)->s.code >= 128)

/* MIDI device entry */
typedef struct {
	int client;
	int port;
	snd_seq_addr_t addr;
	unsigned int caps;
	char name[64];
	int opened;		/* PERM_READ | PERM_WRITE */
	snd_midi_event_t *coder;
} oss_midi_dev_t;

/* Timer state */
typedef struct {
	int oss_tempo;		/* BPM */
	int oss_timebase;	/* PPQ */
	unsigned int cur_tick;
	int realtime;
	int running;
} oss_seq_timer_t;

/* Read queue (ring buffer of OSS events) */
typedef struct {
	union evrec buf[READQ_SIZE];
	int head, tail, qlen;
	int pipe_rd, pipe_wr;
	unsigned int input_tick;
} oss_readq_t;

/* Main sequencer state */
typedef struct oss_seq_s {
	int fileno;		/* /dev/null fd used as OSS fd */
	snd_seq_t *seq;
	int client, port, queue;
	snd_seq_addr_t addr;
	int seq_mode;		/* OSS_SEQ_MODE_SYNTH or OSS_SEQ_MODE_MUSIC */
	int file_mode;		/* O_RDONLY / O_WRONLY / O_RDWR */
	int nonblock;
	int max_mididev;
	oss_midi_dev_t *midi_devs;
	oss_seq_timer_t timer;
	oss_readq_t *readq;
	struct oss_seq_s *next;
} oss_seq_t;

static oss_seq_t *seq_list = NULL;

/* Internal helpers */

static oss_seq_t *find_seq(int fd)
{
	oss_seq_t *dp;

	for (dp = seq_list; dp; dp = dp->next) {
		if (dp->fileno == fd)
			break;
	}
	return dp;
}

static void readq_put(oss_readq_t *rq, union evrec *ev, int long_ev)
{
	int len = long_ev ? LONG_EVENT_SIZE : SHORT_EVENT_SIZE;
	char c = 0;

	if (rq->qlen < READQ_SIZE) {
		memcpy(&rq->buf[rq->tail], ev, len);
		rq->tail = (rq->tail + 1) % READQ_SIZE;
		rq->qlen++;
		write(rq->pipe_wr, &c, 1);
	}
}

/* Put SEQ_WAIT timestamp if tick changed (SYNTH mode) */
static void readq_put_timestamp_synth(oss_readq_t *rq, unsigned int tick)
{
	union evrec ev;

	if (tick == rq->input_tick)
		return;
	rq->input_tick = tick;
	memset(&ev, 0, sizeof(ev));
	ev.echo = (tick << 8) | SEQ_WAIT;
	readq_put(rq, &ev, 0);
}

/* Put EV_TIMING/TMR_WAIT_ABS timestamp if tick changed (MUSIC mode) */
static void readq_put_timestamp_music(oss_readq_t *rq, unsigned int tick)
{
	union evrec ev;

	if (tick == rq->input_tick)
		return;
	rq->input_tick = tick;
	memset(&ev, 0, sizeof(ev));
	ev.t.code = EV_TIMING;
	ev.t.cmd = TMR_WAIT_ABS;
	ev.t.time = tick;
	readq_put(rq, &ev, 1);
}

/* ALSA queue / timer control */

static void apply_queue_tempo(oss_seq_t *dp)
{
	snd_seq_queue_tempo_t *tempo;

	snd_seq_queue_tempo_alloca(&tempo);
	snd_seq_queue_tempo_set_tempo(tempo, 60000000 / dp->timer.oss_tempo);
	snd_seq_queue_tempo_set_ppq(tempo, dp->timer.oss_timebase);
	snd_seq_set_queue_tempo(dp->seq, dp->queue, tempo);
}

static void send_queue_control(oss_seq_t *dp, int type)
{
	snd_seq_event_t ev;

	snd_seq_ev_clear(&ev);
	ev.type = type;
	snd_seq_ev_set_fixed(&ev);
	ev.data.queue.queue = dp->queue;
	snd_seq_ev_set_dest(&ev, SND_SEQ_CLIENT_SYSTEM, SND_SEQ_PORT_SYSTEM_TIMER);
	snd_seq_ev_set_direct(&ev);
	snd_seq_event_output_direct(dp->seq, &ev);
}

static void timer_start(oss_seq_t *dp)
{
	apply_queue_tempo(dp);
	send_queue_control(dp, SND_SEQ_EVENT_START);
	dp->timer.running = 1;
	dp->timer.cur_tick = 0;
	dp->timer.realtime = 0;
}

static void timer_stop(oss_seq_t *dp)
{
	send_queue_control(dp, SND_SEQ_EVENT_STOP);
	dp->timer.running = 0;
}

static void timer_continue(oss_seq_t *dp)
{
	send_queue_control(dp, SND_SEQ_EVENT_CONTINUE);
	dp->timer.running = 1;
}

/* Event processing: OSS to ALSA */

/*
 * Returns 1 if the event is a pure timing event (consumed), 0 otherwise.
 */
static int process_timer_event(oss_seq_t *dp, union evrec *ev)
{
	unsigned int parm, tick;

	if (ev->t.code == EV_TIMING) {
		parm = ev->t.time;
		switch (ev->t.cmd) {
		case TMR_WAIT_REL:
			dp->timer.cur_tick += parm;
			dp->timer.realtime = 0;
			return 1;
		case TMR_WAIT_ABS:
			if (parm == 0)
				dp->timer.realtime = 1;
			else if (parm >= dp->timer.cur_tick) {
				dp->timer.realtime = 0;
				dp->timer.cur_tick = parm;
			}
			return 1;
		case TMR_START:
			timer_start(dp);
			return 1;
		case TMR_STOP:
			timer_stop(dp);
			return 1;
		case TMR_CONTINUE:
			timer_continue(dp);
			return 1;
		case TMR_TEMPO:
			if (parm > 0)
				dp->timer.oss_tempo = parm;
			if (dp->timer.running)
				apply_queue_tempo(dp);
			return 1;
		default:
			return 0;
		}
	} else if (ev->s.code == SEQ_WAIT) {
		/* 4-byte wait: tick in bytes 1-3 */
		tick = (ev->echo >> 8) & 0xffffff;
		if (tick > dp->timer.cur_tick) {
			dp->timer.cur_tick = tick;
			dp->timer.realtime = 0;
		}
		return 1;
	}
	return 0;
}

static void set_dest_midi(oss_seq_t *dp, snd_seq_event_t *ev, int dev)
{
	if (dev >= 0 && dev < dp->max_mididev)
		snd_seq_ev_set_dest(ev, dp->midi_devs[dev].client, dp->midi_devs[dev].port);
	else
		snd_seq_ev_set_dest(ev, SND_SEQ_ADDRESS_SUBSCRIBERS, 0);
}

/*
 * Translate a single OSS event record to an ALSA event.
 * Returns 0 on success, negative on error/skip, 1 if already sent (sysex/midiputc).
 */
static int process_event(oss_seq_t *dp, union evrec *ev, snd_seq_event_t *out)
{
	int dev, chn, note, vel, sysex_len, i;
	union evrec tmp;
	oss_midi_dev_t *md;
	snd_seq_event_t mev;
	long encode_rc;
	unsigned char sysex_buf[6];

	snd_seq_ev_clear(out);
	out->source = dp->addr;

	switch (ev->s.code) {
	case EV_CHN_VOICE:
		dev = ev->v.dev;
		chn = ev->v.chn;
		note = ev->v.note;
		vel = ev->v.parm;
		set_dest_midi(dp, out, dev);
		switch (ev->v.cmd) {
		case MIDI_NOTEON:
			if (vel == 0)
				snd_seq_ev_set_noteoff(out, chn, note, 0);
			else
				snd_seq_ev_set_noteon(out, chn, note, vel);
			break;
		case MIDI_NOTEOFF:
			snd_seq_ev_set_noteoff(out, chn, note, vel);
			break;
		case MIDI_KEY_PRESSURE:
			out->type = SND_SEQ_EVENT_KEYPRESS;
			out->data.note.channel = chn;
			out->data.note.note = note;
			out->data.note.velocity = vel;
			break;
		default:
			return -EINVAL;
		}
		break;

	case EV_CHN_COMMON:
		dev = ev->l.dev;
		chn = ev->l.chn;
		set_dest_midi(dp, out, dev);
		switch (ev->l.cmd) {
		case MIDI_PGM_CHANGE:
			snd_seq_ev_set_pgmchange(out, chn, ev->l.p1);
			break;
		case MIDI_CTL_CHANGE:
			snd_seq_ev_set_controller(out, chn, ev->l.p1, ev->l.val);
			break;
		case MIDI_PITCH_BEND:
			snd_seq_ev_set_pitchbend(out, chn, ev->l.val - 8192);
			break;
		case MIDI_CHN_PRESSURE:
			snd_seq_ev_set_chanpress(out, chn, ev->l.val);
			break;
		default:
			return -EINVAL;
		}
		break;

	case EV_TIMING:
		switch (ev->t.cmd) {
		case TMR_ECHO:
			/* schedule an ECHO event that loops back */
			if (dp->seq_mode == OSS_SEQ_MODE_SYNTH) {
				memset(&tmp, 0, sizeof(tmp));
				tmp.echo = (ev->t.time << 8) | SEQ_ECHO;
			} else {
				memcpy(&tmp, ev, LONG_EVENT_SIZE);
			}
			out->type = SND_SEQ_EVENT_ECHO;
			snd_seq_ev_set_dest(out, dp->addr.client, dp->addr.port);
			memcpy(&out->data, &tmp, LONG_EVENT_SIZE);
			break;
		/* TMR_START/STOP/CONTINUE/TEMPO already handled in process_timer_event */
		default:
			return -EINVAL;
		}
		break;

	case SEQ_EXTENDED:
		if (dp->seq_mode != OSS_SEQ_MODE_SYNTH)
			return -EINVAL;
		dev = ev->e.dev;
		chn = ev->e.chn;
		set_dest_midi(dp, out, dev);
		switch (ev->e.cmd) {
		case SEQ_EXT_NOTEOFF:
			snd_seq_ev_set_noteoff(out, chn, ev->e.p1, ev->e.p2);
			break;
		case SEQ_EXT_NOTEON:
			snd_seq_ev_set_noteon(out, chn, ev->e.p1, ev->e.p2);
			break;
		case SEQ_EXT_PGMCHANGE:
			snd_seq_ev_set_pgmchange(out, chn, ev->e.p1);
			break;
		case SEQ_AFTERTOUCH:  /* 4 */
			snd_seq_ev_set_chanpress(out, chn, ev->e.p1);
			break;
		case SEQ_BALANCE:     /* 5 */
			snd_seq_ev_set_controller(out, chn, 10 /* CTL_PAN */, ev->e.p1);
			break;
		case SEQ_CONTROLLER:  /* 6 */
			snd_seq_ev_set_controller(out, chn, ev->e.p1, ev->e.p2);
			break;
		default:
			return -EINVAL;
		}
		break;

	case SEQ_MIDIPUTC:
		dev = ev->s.dev;
		if (dev < 0 || dev >= dp->max_mididev)
			return -ENXIO;
		md = &dp->midi_devs[dev];
		if (!md->coder)
			return -ENXIO;
		snd_seq_ev_clear(&mev);
		mev.source = dp->addr;
		set_dest_midi(dp, &mev, dev);
		encode_rc = snd_midi_event_encode_byte(md->coder, ev->s.parm1, &mev);
		if (encode_rc > 0) {
			if (dp->timer.realtime || !dp->timer.running)
				snd_seq_ev_set_direct(&mev);
			else
				snd_seq_ev_schedule_tick(&mev, dp->queue, 0, dp->timer.cur_tick);
			snd_seq_event_output(dp->seq, &mev);
		}
		return 1; /* already handled */

	case EV_SYSEX:
		dev = ev->x.dev;
		sysex_len = 0;
		for (i = 0; i < 6; i++) {
			sysex_buf[i] = ev->x.buf[i];
			if (sysex_buf[i] == 0xff)
				break;
			sysex_len++;
		}
		if (sysex_len == 0)
			return -EINVAL;
		set_dest_midi(dp, out, dev);
		snd_seq_ev_set_variable(out, sysex_len, sysex_buf);
		out->type = SND_SEQ_EVENT_SYSEX;
		/* must output immediately since sysex_buf is stack-allocated */
		if (dp->timer.realtime || !dp->timer.running)
			snd_seq_ev_set_direct(out);
		else
			snd_seq_ev_schedule_tick(out, dp->queue, 0, dp->timer.cur_tick);
		snd_seq_event_output(dp->seq, out);
		return 1; /* already handled */

	case SEQ_ECHO:
		if (dp->seq_mode == OSS_SEQ_MODE_MUSIC)
			return -EINVAL;
		out->type = SND_SEQ_EVENT_ECHO;
		snd_seq_ev_set_dest(out, dp->addr.client, dp->addr.port);
		memcpy(&out->data, ev, SHORT_EVENT_SIZE);
		break;

	/* Old-style 4-byte events (SYNTH mode only) */
	default:
		if (dp->seq_mode == OSS_SEQ_MODE_MUSIC)
			return -EINVAL;
		switch (ev->s.code) {
		case SEQ_NOTEON:
			set_dest_midi(dp, out, 0);
			snd_seq_ev_set_noteon(out, ev->n.chn, ev->n.note, ev->n.vel);
			break;
		case SEQ_NOTEOFF:
			set_dest_midi(dp, out, 0);
			snd_seq_ev_set_noteoff(out, ev->n.chn, ev->n.note, 0);
			break;
		case SEQ_PGMCHANGE:
			set_dest_midi(dp, out, 0);
			snd_seq_ev_set_pgmchange(out, ev->n.chn, ev->n.note);
			break;
		case SEQ_SYNCTIMER:
			timer_start(dp);
			return -EINVAL;
		default:
			return -EINVAL;
		}
		break;
	}
	return 0;
}

/* Event processing: ALSA to OSS */

/* Decode ALSA event to OSS SEQ_MIDIPUTC records */
static void send_midi_bytes(oss_seq_t *dp, int dev, snd_seq_event_t *ev)
{
	oss_midi_dev_t *md;
	unsigned char buf[16];
	unsigned int tick;
	union evrec rec;
	long len;
	int i;

	if (dev < 0 || dev >= dp->max_mididev)
		return;
	md = &dp->midi_devs[dev];
	if (!md->coder)
		return;
	len = snd_midi_event_decode(md->coder, buf, sizeof(buf), ev);
	if (len <= 0)
		return;
	tick = ev->time.tick;
	readq_put_timestamp_synth(dp->readq, tick);
	for (i = 0; i < len; i++) {
		memset(&rec, 0, sizeof(rec));
		rec.c[0] = SEQ_MIDIPUTC;
		rec.c[1] = buf[i];
		rec.c[2] = dev;
		readq_put(dp->readq, &rec, 0);
	}
}

/* Find which midi_dev this ALSA address belongs to */
static int find_midi_dev_by_addr(oss_seq_t *dp, snd_seq_addr_t *addr)
{
	int i;

	for (i = 0; i < dp->max_mididev; i++) {
		if (dp->midi_devs[i].addr.client == addr->client &&
		    dp->midi_devs[i].addr.port == addr->port)
			return i;
	}
	return -1;
}

/* Translate ALSA event to OSS EV_CHN_VOICE/EV_CHN_COMMON (MUSIC mode) */
static void send_synth_event(oss_seq_t *dp, int dev, snd_seq_event_t *ev)
{
	unsigned int tick = ev->time.tick;
	union evrec rec;

	readq_put_timestamp_music(dp->readq, tick);
	memset(&rec, 0, sizeof(rec));
	switch (ev->type) {
	case SND_SEQ_EVENT_NOTEON:
		rec.v.code = EV_CHN_VOICE;
		rec.v.dev = dev;
		rec.v.cmd = MIDI_NOTEON;
		rec.v.chn = ev->data.note.channel;
		rec.v.note = ev->data.note.note;
		rec.v.parm = ev->data.note.velocity;
		readq_put(dp->readq, &rec, 1);
		break;
	case SND_SEQ_EVENT_NOTEOFF:
		rec.v.code = EV_CHN_VOICE;
		rec.v.dev = dev;
		rec.v.cmd = MIDI_NOTEOFF;
		rec.v.chn = ev->data.note.channel;
		rec.v.note = ev->data.note.note;
		rec.v.parm = ev->data.note.velocity;
		readq_put(dp->readq, &rec, 1);
		break;
	case SND_SEQ_EVENT_KEYPRESS:
		rec.v.code = EV_CHN_VOICE;
		rec.v.dev = dev;
		rec.v.cmd = MIDI_KEY_PRESSURE;
		rec.v.chn = ev->data.note.channel;
		rec.v.note = ev->data.note.note;
		rec.v.parm = ev->data.note.velocity;
		readq_put(dp->readq, &rec, 1);
		break;
	case SND_SEQ_EVENT_CONTROLLER:
		rec.l.code = EV_CHN_COMMON;
		rec.l.dev = dev;
		rec.l.cmd = MIDI_CTL_CHANGE;
		rec.l.chn = ev->data.control.channel;
		rec.l.p1 = ev->data.control.param;
		rec.l.val = ev->data.control.value;
		readq_put(dp->readq, &rec, 1);
		break;
	case SND_SEQ_EVENT_PGMCHANGE:
		rec.l.code = EV_CHN_COMMON;
		rec.l.dev = dev;
		rec.l.cmd = MIDI_PGM_CHANGE;
		rec.l.chn = ev->data.control.channel;
		rec.l.p1 = ev->data.control.value;
		readq_put(dp->readq, &rec, 1);
		break;
	case SND_SEQ_EVENT_PITCHBEND:
		rec.l.code = EV_CHN_COMMON;
		rec.l.dev = dev;
		rec.l.cmd = MIDI_PITCH_BEND;
		rec.l.chn = ev->data.control.channel;
		rec.l.val = ev->data.control.value + 8192;
		readq_put(dp->readq, &rec, 1);
		break;
	case SND_SEQ_EVENT_CHANPRESS:
		rec.l.code = EV_CHN_COMMON;
		rec.l.dev = dev;
		rec.l.cmd = MIDI_CHN_PRESSURE;
		rec.l.chn = ev->data.control.channel;
		rec.l.val = ev->data.control.value;
		readq_put(dp->readq, &rec, 1);
		break;
	default:
		break;
	}
}

/*
 * Fetch available ALSA events and dispatch them.
 * Non-echo events go into readq; echo events go into readq unless want_sync
 * is set and the echo is a SEQ_SYNCTIMER, in which case returns 1 (sync done).
 */
static int handle_alsa_input(oss_seq_t *dp, int want_sync)
{
	snd_seq_event_t *ev;
	int dev;

	snd_seq_event_input_pending(dp->seq, 1);
	while (snd_seq_event_input_pending(dp->seq, 0) > 0) {
		if (snd_seq_event_input(dp->seq, &ev) < 0)
			break;
		if (ev->type == SND_SEQ_EVENT_ECHO) {
			union evrec *rec = (union evrec *)&ev->data;
			if (want_sync && rec->s.code == SEQ_SYNCTIMER)
				return 1;
			if (dp->readq)
				readq_put(dp->readq, rec, ev_is_long(rec));
		} else if (dp->readq) {
			dev = find_midi_dev_by_addr(dp, &ev->source);
			if (dp->seq_mode == OSS_SEQ_MODE_MUSIC)
				send_synth_event(dp, dev, ev);
			else
				send_midi_bytes(dp, dev, ev);
		}
	}
	return 0;
}

/* MIDI device enumeration */

#define PERM_READ  (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ)
#define PERM_WRITE (SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE)

static int enumerate_midi_devs(oss_seq_t *dp)
{
	snd_seq_client_info_t *cinfo;
	snd_seq_port_info_t *pinfo;
	oss_midi_dev_t *md, *tmp;
	unsigned int type, caps;
	int count = 0;
	int alloc = 16;
	int cli;

	snd_seq_client_info_alloca(&cinfo);
	snd_seq_port_info_alloca(&pinfo);

	dp->midi_devs = malloc(alloc * sizeof(oss_midi_dev_t));
	if (!dp->midi_devs)
		return -ENOMEM;

	snd_seq_client_info_set_client(cinfo, -1);
	while (snd_seq_query_next_client(dp->seq, cinfo) >= 0) {
		cli = snd_seq_client_info_get_client(cinfo);
		if (cli == dp->client)
			continue;
		snd_seq_port_info_set_client(pinfo, cli);
		snd_seq_port_info_set_port(pinfo, -1);
		while (snd_seq_query_next_port(dp->seq, pinfo) >= 0) {
			type = snd_seq_port_info_get_type(pinfo);
			caps = snd_seq_port_info_get_capability(pinfo);
			if (!(type & SND_SEQ_PORT_TYPE_MIDI_GENERIC))
				continue;
			if (!(caps & (PERM_READ | PERM_WRITE)))
				continue;
			if (count >= alloc) {
				alloc *= 2;
				tmp = realloc(dp->midi_devs,
					      alloc * sizeof(oss_midi_dev_t));
				if (!tmp)
					return -ENOMEM;
				dp->midi_devs = tmp;
			}
			md = &dp->midi_devs[count];
			memset(md, 0, sizeof(*md));
			md->client = snd_seq_port_info_get_client(pinfo);
			md->port   = snd_seq_port_info_get_port(pinfo);
			md->addr.client = md->client;
			md->addr.port   = md->port;
			md->caps   = caps;
			strncpy(md->name, snd_seq_port_info_get_name(pinfo),
				sizeof(md->name) - 1);
			count++;
		}
	}
	dp->max_mididev = count;
	return 0;
}

/* Subscribe / unsubscribe MIDI ports */

static int subscribe_midi(oss_seq_t *dp, int dev, int for_read)
{
	snd_seq_port_subscribe_t *sub;
	oss_midi_dev_t *md;
	int rc;

	snd_seq_port_subscribe_alloca(&sub);
	md = &dp->midi_devs[dev];

	if (for_read) {
		snd_seq_port_subscribe_set_sender(sub, &md->addr);
		snd_seq_port_subscribe_set_dest(sub, &dp->addr);
		snd_seq_port_subscribe_set_time_update(sub, 1);
		snd_seq_port_subscribe_set_queue(sub, dp->queue);
		snd_seq_port_subscribe_set_time_real(sub, 0);
		rc = snd_seq_subscribe_port(dp->seq, sub);
		if (rc < 0)
			return rc;
		md->opened |= PERM_READ;
	} else {
		snd_seq_port_subscribe_set_sender(sub, &dp->addr);
		snd_seq_port_subscribe_set_dest(sub, &md->addr);
		rc = snd_seq_subscribe_port(dp->seq, sub);
		if (rc < 0)
			return rc;
		md->opened |= PERM_WRITE;
	}
	return 0;
}

static void unsubscribe_midi(oss_seq_t *dp, int dev)
{
	snd_seq_port_subscribe_t *sub;
	oss_midi_dev_t *md;

	snd_seq_port_subscribe_alloca(&sub);
	md = &dp->midi_devs[dev];

	if (md->opened & PERM_READ) {
		snd_seq_port_subscribe_set_sender(sub, &md->addr);
		snd_seq_port_subscribe_set_dest(sub, &dp->addr);
		snd_seq_unsubscribe_port(dp->seq, sub);
	}
	if (md->opened & PERM_WRITE) {
		snd_seq_port_subscribe_set_sender(sub, &dp->addr);
		snd_seq_port_subscribe_set_dest(sub, &md->addr);
		snd_seq_unsubscribe_port(dp->seq, sub);
	}
	md->opened = 0;
}

/* Wait for the ALSA queue to finish playing before close */
static void drain_at_close(oss_seq_t *dp)
{
	snd_seq_event_t ev, *evp;
	union evrec rec;
	struct pollfd pfds[4];
	int nfds, rc;

	if (!dp->timer.running || dp->timer.realtime)
		return;

	/*
	 * Schedule a sentinel ECHO at cur_tick; when it arrives back, the
	 * queue has delivered all events up to that tick
	 */
	snd_seq_ev_clear(&ev);
	ev.type = SND_SEQ_EVENT_ECHO;
	memset(&rec, 0, sizeof(rec));
	rec.t.code = SEQ_SYNCTIMER;
	rec.t.time = dp->timer.cur_tick;
	memcpy(&ev.data, &rec, sizeof(rec));
	snd_seq_ev_set_dest(&ev, dp->addr.client, dp->addr.port);
	snd_seq_ev_schedule_tick(&ev, dp->queue, 0, dp->timer.cur_tick);
	snd_seq_event_output(dp->seq, &ev);
	snd_seq_drain_output(dp->seq);

	nfds = snd_seq_poll_descriptors_count(dp->seq, POLLIN);
	if (nfds <= 0 || nfds > (int)(sizeof(pfds) / sizeof(pfds[0])))
		return;
	snd_seq_poll_descriptors(dp->seq, pfds, nfds, POLLIN);

	for (;;) {
		rc = poll(pfds, nfds, 5000);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (rc == 0)
			break; /* timeout: give up waiting */
		/* fetch from kernel into userspace buffer */
		snd_seq_event_input_pending(dp->seq, 1);
		while (snd_seq_event_input_pending(dp->seq, 0) > 0) {
			if (snd_seq_event_input(dp->seq, &evp) < 0)
				return;
			if (evp->type == SND_SEQ_EVENT_ECHO) {
				union evrec *r = (union evrec *)&evp->data;
				if (r->s.code == SEQ_SYNCTIMER)
					return;
			}
		}
	}
}

/* Reset helpers */

static void seq_all_notes_off(oss_seq_t *dp)
{
	snd_seq_event_t ev;
	int i, ch;

	for (i = 0; i < dp->max_mididev; i++) {
		if (!(dp->midi_devs[i].opened & PERM_WRITE))
			continue;
		snd_seq_ev_clear(&ev);
		ev.source = dp->addr;
		snd_seq_ev_set_dest(&ev, dp->midi_devs[i].client, dp->midi_devs[i].port);
		snd_seq_ev_set_direct(&ev);
		for (ch = 0; ch < 16; ch++) {
			snd_seq_ev_set_controller(&ev, ch, 123 /* all notes off */, 0);
			snd_seq_event_output_direct(dp->seq, &ev);
		}
	}
}

static void readq_clear(oss_readq_t *rq)
{
	char buf[64];

	rq->head = rq->tail = rq->qlen = 0;
	while (read(rq->pipe_rd, buf, sizeof(buf)) > 0)
		;
}

/* Public API: open / close */

int lib_oss_seq_open(const char *pathname, int flags)
{
	int seq_mode, oflags, rc, readable, writable, i;
	int pipefds[2];
	unsigned int port_caps, port_type;
	oss_seq_t *dp;

	if (strcmp(pathname, "/dev/music") == 0)
		seq_mode = OSS_SEQ_MODE_MUSIC;
	else
		seq_mode = OSS_SEQ_MODE_SYNTH;

	dp = calloc(1, sizeof(*dp));
	if (!dp)
		return -ENOMEM;

	dp->seq_mode = seq_mode;
	dp->file_mode = flags & O_ACCMODE;
	dp->timer.oss_tempo = 60;
	dp->timer.oss_timebase = 100;
	dp->timer.realtime = 1;

	oflags = flags & O_ACCMODE;
	dp->fileno = open("/dev/null", oflags);
	if (dp->fileno < 0) {
		free(dp);
		return -errno;
	}

	rc = snd_seq_open(&dp->seq, "default", SND_SEQ_OPEN_DUPLEX, 0);
	if (rc < 0) {
		close(dp->fileno);
		free(dp);
		errno = -rc;
		return -1;
	}

	snd_seq_set_client_name(dp->seq, "OSS sequencer");
	dp->client = snd_seq_client_id(dp->seq);

	port_caps = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_WRITE |
		    SND_SEQ_PORT_CAP_SUBS_READ | SND_SEQ_PORT_CAP_SUBS_WRITE;
	port_type = SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION;
	dp->port = snd_seq_create_simple_port(dp->seq, "OSS port", port_caps, port_type);
	if (dp->port < 0) {
		rc = dp->port;
		goto err_close_seq;
	}
	dp->addr.client = dp->client;
	dp->addr.port   = dp->port;

	dp->queue = snd_seq_alloc_queue(dp->seq);
	if (dp->queue < 0) {
		rc = dp->queue;
		goto err_delete_port;
	}
	apply_queue_tempo(dp);

	rc = enumerate_midi_devs(dp);
	if (rc < 0)
		goto err_free_queue;

	readable = (dp->file_mode == O_RDONLY || dp->file_mode == O_RDWR);
	if (readable) {
		dp->readq = calloc(1, sizeof(*dp->readq));
		if (!dp->readq) {
			rc = -ENOMEM;
			goto err_free_devs;
		}
		if (pipe2(pipefds, O_NONBLOCK) < 0) {
			rc = -errno;
			goto err_free_readq;
		}
		dp->readq->pipe_rd = pipefds[0];
		dp->readq->pipe_wr = pipefds[1];

		for (i = 0; i < dp->max_mididev; i++) {
			if (dp->midi_devs[i].caps & PERM_READ)
				subscribe_midi(dp, i, 1);
		}
	}

	writable = (dp->file_mode == O_WRONLY || dp->file_mode == O_RDWR);
	if (writable) {
		for (i = 0; i < dp->max_mididev; i++) {
			if (dp->midi_devs[i].caps & PERM_WRITE)
				subscribe_midi(dp, i, 0);
		}
		for (i = 0; i < dp->max_mididev; i++) {
			snd_midi_event_new(256, &dp->midi_devs[i].coder);
			if (dp->midi_devs[i].coder)
				snd_midi_event_no_status(dp->midi_devs[i].coder, 1);
		}
	}

	dp->next = seq_list;
	seq_list = dp;

	return dp->fileno;

err_free_readq:
	if (dp->readq) {
		free(dp->readq);
	}
err_free_devs:
	for (i = 0; i < dp->max_mididev; i++) {
		if (dp->midi_devs[i].coder)
			snd_midi_event_free(dp->midi_devs[i].coder);
	}
	free(dp->midi_devs);
err_free_queue:
	snd_seq_free_queue(dp->seq, dp->queue);
err_delete_port:
	snd_seq_delete_simple_port(dp->seq, dp->port);
err_close_seq:
	snd_seq_close(dp->seq);
	close(dp->fileno);
	free(dp);
	errno = -rc;
	return -1;
}

int lib_oss_seq_close(int fd)
{
	oss_seq_t *dp, *prev = NULL;
	int i;

	for (dp = seq_list; dp; prev = dp, dp = dp->next) {
		if (dp->fileno == fd)
			break;
	}
	if (!dp) {
		errno = EBADF;
		return -1;
	}
	if (prev)
		prev->next = dp->next;
	else
		seq_list = dp->next;

	drain_at_close(dp);

	for (i = 0; i < dp->max_mididev; i++) {
		unsubscribe_midi(dp, i);
		if (dp->midi_devs[i].coder)
			snd_midi_event_free(dp->midi_devs[i].coder);
	}
	free(dp->midi_devs);

	snd_seq_drop_output(dp->seq);
	snd_seq_free_queue(dp->seq, dp->queue);
	snd_seq_delete_simple_port(dp->seq, dp->port);
	snd_seq_close(dp->seq);

	if (dp->readq) {
		close(dp->readq->pipe_rd);
		close(dp->readq->pipe_wr);
		free(dp->readq);
	}

	close(fd);

	free(dp);
	return 0;
}

/* Public API: read / write */

ssize_t lib_oss_seq_read(int fd, void *buf, size_t count)
{
	oss_seq_t *dp = find_seq(fd);
	oss_readq_t *rq;
	unsigned char *p = buf;
	union evrec *rec;
	char tmp[64];
	size_t copied = 0;
	int sz, nfds;
	struct pollfd pfds[8];

	if (!dp) {
		errno = EBADF;
		return -1;
	}
	if (!dp->readq) {
		errno = EINVAL;
		return -1;
	}
	rq = dp->readq;

	while (copied == 0) {
		if (rq->qlen == 0) {
			if (dp->nonblock) {
				handle_alsa_input(dp, 0);
				if (rq->qlen == 0) {
					errno = EAGAIN;
					return -1;
				}
			} else {
				nfds = snd_seq_poll_descriptors_count(dp->seq, POLLIN);
				if (nfds > (int)(sizeof(pfds) / sizeof(pfds[0])))
					nfds = sizeof(pfds) / sizeof(pfds[0]);
				do {
					snd_seq_poll_descriptors(dp->seq, pfds, nfds, POLLIN);
					while (poll(pfds, nfds, -1) < 0 && errno == EINTR)
						;
					handle_alsa_input(dp, 0);
				} while (rq->qlen == 0);
			}
			continue;
		}
		while (rq->qlen > 0 && copied + LONG_EVENT_SIZE <= count) {
			rec = &rq->buf[rq->head];
			sz = ev_is_long(rec) ? LONG_EVENT_SIZE : SHORT_EVENT_SIZE;
			if (copied + sz > count)
				break;
			memcpy(p + copied, rec, sz);
			copied += sz;
			rq->head = (rq->head + 1) % READQ_SIZE;
			rq->qlen--;
		}
		if (copied == 0 && count < SHORT_EVENT_SIZE) {
			errno = EINVAL;
			return -1;
		}
		if (copied == 0)
			break; /* event too large for buffer */
	}
	if (rq->qlen == 0) {
		while (read(rq->pipe_rd, tmp, sizeof(tmp)) > 0)
			;
	}
	return copied;
}

ssize_t lib_oss_seq_write(int fd, const void *buf, size_t count)
{
	oss_seq_t *dp = find_seq(fd);
	const unsigned char *p = buf;
	union evrec ev;
	snd_seq_event_t sev;
	size_t pos = 0;
	int is_long, evlen, rc;

	if (!dp) {
		errno = EBADF;
		return -1;
	}

	while (pos + SHORT_EVENT_SIZE <= count) {
		memset(&ev, 0, sizeof(ev));
		memcpy(&ev, p + pos, SHORT_EVENT_SIZE);

		if (ev.s.code == SEQ_FULLSIZE) {
			pos += SHORT_EVENT_SIZE;
			continue;
		}

		is_long = ev_is_long(&ev);
		evlen = is_long ? LONG_EVENT_SIZE : SHORT_EVENT_SIZE;

		if (pos + evlen > count)
			break;

		if (is_long)
			memcpy(&ev, p + pos, LONG_EVENT_SIZE);

		pos += evlen;

		if (process_timer_event(dp, &ev))
			continue;

		rc = process_event(dp, &ev, &sev);
		if (rc < 0)
			continue;
		if (rc == 1)
			continue; /* already sent (sysex/midiputc) */

		if (dp->timer.realtime || !dp->timer.running)
			snd_seq_ev_set_direct(&sev);
		else
			snd_seq_ev_schedule_tick(&sev, dp->queue, 0, dp->timer.cur_tick);

		snd_seq_event_output(dp->seq, &sev);
	}

	snd_seq_drain_output(dp->seq);
	return pos;
}

/* Public API: ioctl */

int lib_oss_seq_ioctl(int fd, unsigned long request, ...)
{
	va_list args;
	void *arg;
	oss_seq_t *dp;
	snd_seq_event_t sev;
	union evrec ev, rec;
	snd_seq_client_pool_t *pool;
	struct synth_info *sinfo;
	struct midi_info *minfo;
	struct pollfd pfds[4];
	int dev, val, rc, nfds;

	va_start(args, request);
	arg = va_arg(args, void *);
	va_end(args);

	dp = find_seq(fd);
	if (!dp) {
		errno = EBADF;
		return -1;
	}

	switch (request) {
	case SNDCTL_SEQ_RESET:
		seq_all_notes_off(dp);
		snd_seq_drop_output(dp->seq);
		if (dp->readq)
			readq_clear(dp->readq);
		dp->timer.cur_tick = 0;
		dp->timer.realtime = 1;
		return 0;

	case SNDCTL_SEQ_PANIC:
		seq_all_notes_off(dp);
		snd_seq_drop_output(dp->seq);
		if (dp->readq)
			readq_clear(dp->readq);
		errno = EINVAL;
		return -1;

	case SNDCTL_SEQ_SYNC:
		snd_seq_ev_clear(&sev);
		sev.type = SND_SEQ_EVENT_ECHO;
		memset(&rec, 0, sizeof(rec));
		rec.t.code = SEQ_SYNCTIMER;
		rec.t.time = dp->timer.cur_tick;
		memcpy(&sev.data, &rec, sizeof(rec));
		snd_seq_ev_set_dest(&sev, dp->addr.client, dp->addr.port);
		if (!dp->timer.running || dp->timer.realtime)
			snd_seq_ev_set_direct(&sev);
		else
			snd_seq_ev_schedule_tick(&sev, dp->queue, 0, dp->timer.cur_tick);
		snd_seq_event_output(dp->seq, &sev);
		snd_seq_drain_output(dp->seq);

		nfds = snd_seq_poll_descriptors_count(dp->seq, POLLIN);
		if (nfds > (int)(sizeof(pfds) / sizeof(pfds[0])))
			nfds = sizeof(pfds) / sizeof(pfds[0]);
		for (;;) {
			snd_seq_poll_descriptors(dp->seq, pfds, nfds, POLLIN);
			rc = poll(pfds, nfds, 5000);
			if (rc < 0) {
				if (errno == EINTR)
					continue;
				break;
			}
			if (rc == 0)
				break; /* timeout */
			if (handle_alsa_input(dp, 1))
				break; /* sync echo received */
		}
		return 0;

	case SNDCTL_SEQ_NRSYNTHS:
		if (dp->seq_mode == OSS_SEQ_MODE_MUSIC)
			*(int *)arg = dp->max_mididev;
		else
			*(int *)arg = 0;
		return 0;

	case SNDCTL_SEQ_NRMIDIS:
		*(int *)arg = dp->max_mididev;
		return 0;

	case SNDCTL_SEQ_GETTIME:
		*(int *)arg = dp->timer.cur_tick;
		return 0;

	case SNDCTL_SEQ_CTRLRATE:
		*(int *)arg = (dp->timer.oss_tempo * dp->timer.oss_timebase + 30) / 60;
		return 0;

	case SNDCTL_SEQ_GETINCOUNT:
		*(int *)arg = dp->readq ? dp->readq->qlen : 0;
		return 0;

	case SNDCTL_SEQ_GETOUTCOUNT:
		snd_seq_client_pool_alloca(&pool);
		if (snd_seq_get_client_pool(dp->seq, pool) >= 0)
			*(int *)arg = snd_seq_client_pool_get_output_room(pool);
		else
			*(int *)arg = 0;
		return 0;

	case SNDCTL_SEQ_THRESHOLD:
		return 0;

	case SNDCTL_SEQ_RESETSAMPLES:
	case SNDCTL_SYNTH_MEMAVL:
	case SNDCTL_FM_4OP_ENABLE:
		errno = EINVAL;
		return -1;

	case SNDCTL_SEQ_TESTMIDI:
		dev = *(int *)arg;
		if (dev < 0 || dev >= dp->max_mididev) {
			errno = ENXIO;
			return -1;
		}
		return 0;

	case SNDCTL_SEQ_OUTOFBAND:
		if (!arg) {
			errno = EINVAL;
			return -1;
		}
		memcpy(&ev, arg, LONG_EVENT_SIZE);
		snd_seq_ev_clear(&sev);
		rc = process_event(dp, &ev, &sev);
		if (rc < 0) {
			errno = -rc;
			return -1;
		}
		if (rc == 0) {
			snd_seq_ev_set_direct(&sev);
			snd_seq_event_output_direct(dp->seq, &sev);
		}
		return 0;

	case SNDCTL_SYNTH_INFO:
	case SNDCTL_SYNTH_ID:
		sinfo = arg;
		dev = sinfo->device;
		if (dp->seq_mode != OSS_SEQ_MODE_MUSIC || dev < 0 || dev >= dp->max_mididev) {
			errno = ENXIO;
			return -1;
		}
		memset(sinfo, 0, sizeof(*sinfo));
		sinfo->device = dev;
		sinfo->synth_type = SYNTH_TYPE_MIDI;
		sinfo->synth_subtype = 0;
		sinfo->nr_voices = 16;
		snprintf(sinfo->name, sizeof(sinfo->name), "%s", dp->midi_devs[dev].name);
		return 0;

	case SNDCTL_MIDI_INFO:
		minfo = arg;
		dev = minfo->device;
		if (dev < 0 || dev >= dp->max_mididev) {
			errno = ENXIO;
			return -1;
		}
		memset(minfo, 0, sizeof(*minfo));
		minfo->device = dev;
		snprintf(minfo->name, sizeof(minfo->name), "%s", dp->midi_devs[dev].name);
		return 0;

	case SNDCTL_MIDI_PRETIME:
		return 0;

	case SNDCTL_TMR_TIMEBASE:
		val = *(int *)arg;
		if (val > 0) {
			dp->timer.oss_timebase = val;
			if (dp->timer.running)
				apply_queue_tempo(dp);
		}
		*(int *)arg = dp->timer.oss_timebase;
		return 0;

	case SNDCTL_TMR_TEMPO:
		val = *(int *)arg;
		if (val > 0) {
			dp->timer.oss_tempo = val;
			if (dp->timer.running)
				apply_queue_tempo(dp);
		}
		*(int *)arg = dp->timer.oss_tempo;
		return 0;

	case SNDCTL_TMR_START:
		timer_start(dp);
		return 0;

	case SNDCTL_TMR_STOP:
		timer_stop(dp);
		return 0;

	case SNDCTL_TMR_CONTINUE:
		timer_continue(dp);
		return 0;

	case SNDCTL_TMR_METRONOME:
		return 0;

	case SNDCTL_TMR_SOURCE:
		*(int *)arg = TMR_INTERNAL;
		return 0;

	case SNDCTL_TMR_SELECT:
		return 0;

	case OSS_GETVERSION:
		*(int *)arg = 0x030901;
		return 0;

	default:
		errno = EINVAL;
		return -1;
	}
}

/* Public API: poll support */

int lib_oss_seq_poll_fds(int fd)
{
	oss_seq_t *dp = find_seq(fd);
	int n = 0;

	if (!dp)
		return 0;
	if (dp->readq) {
		n++;  /* pipe_rd */
		n += snd_seq_poll_descriptors_count(dp->seq, POLLIN);
	}
	if (dp->file_mode == O_WRONLY || dp->file_mode == O_RDWR)
		n += snd_seq_poll_descriptors_count(dp->seq, POLLOUT);
	/* subtract 1 because the caller already accounts for the base fd */
	return n > 0 ? n - 1 : 0;
}

int lib_oss_seq_poll_prepare(int fd, int fmode, struct pollfd *ufds)
{
	oss_seq_t *dp = find_seq(fd);
	int n = 0, cnt;

	if (!dp)
		return 0;
	if ((fmode == O_RDONLY || fmode == O_RDWR) && dp->readq) {
		ufds[n].fd = dp->readq->pipe_rd;
		ufds[n].events = POLLIN;
		ufds[n].revents = 0;
		n++;
		cnt = snd_seq_poll_descriptors(dp->seq, &ufds[n],
					       snd_seq_poll_descriptors_count(dp->seq, POLLIN),
					       POLLIN);
		n += cnt;
	}
	if (fmode == O_WRONLY || fmode == O_RDWR) {
		cnt = snd_seq_poll_descriptors(dp->seq, &ufds[n],
					       snd_seq_poll_descriptors_count(dp->seq, POLLOUT),
					       POLLOUT);
		n += cnt;
	}
	return n;
}

int lib_oss_seq_poll_result(int fd, struct pollfd *ufds)
{
	oss_seq_t *dp = find_seq(fd);
	unsigned short revents = 0;
	int result = 0, n = 0, cnt;

	if (!dp)
		return OSS_WAIT_EVENT_ERROR;
	if (dp->readq) {
		if (ufds[n].revents & (POLLIN | POLLHUP))
			result |= OSS_WAIT_EVENT_READ;
		if (ufds[n].revents & POLLERR)
			result |= OSS_WAIT_EVENT_ERROR;
		n++;
		cnt = snd_seq_poll_descriptors_count(dp->seq, POLLIN);
		snd_seq_poll_descriptors_revents(dp->seq, &ufds[n], cnt, &revents);
		if (revents & POLLIN)
			result |= OSS_WAIT_EVENT_READ;
		if (revents & POLLERR)
			result |= OSS_WAIT_EVENT_ERROR;
		n += cnt;
	}
	if (dp->file_mode == O_WRONLY || dp->file_mode == O_RDWR) {
		cnt = snd_seq_poll_descriptors_count(dp->seq, POLLOUT);
		snd_seq_poll_descriptors_revents(dp->seq, &ufds[n], cnt, &revents);
		if (revents & POLLOUT)
			result |= OSS_WAIT_EVENT_WRITE;
		if (revents & POLLERR)
			result |= OSS_WAIT_EVENT_ERROR;
	}
	return result;
}

/* select wrappers (thin; poll is the primary interface) */

int lib_oss_seq_select_prepare(int fd, int fmode, fd_set *rfds,
			       fd_set *wfds __attribute__((unused)),
			       fd_set *efds __attribute__((unused)))
{
	oss_seq_t *dp = find_seq(fd);
	struct pollfd tmp[8];
	int maxfd = 0, n, i;

	if (!dp)
		return 0;
	if ((fmode == O_RDONLY || fmode == O_RDWR) && dp->readq) {
		FD_SET(dp->readq->pipe_rd, rfds);
		if (dp->readq->pipe_rd > maxfd)
			maxfd = dp->readq->pipe_rd;
		n = snd_seq_poll_descriptors_count(dp->seq, POLLIN);
		if (n > (int)(sizeof(tmp) / sizeof(tmp[0])))
			n = sizeof(tmp) / sizeof(tmp[0]);
		n = snd_seq_poll_descriptors(dp->seq, tmp, n, POLLIN);
		for (i = 0; i < n; i++) {
			if (tmp[i].events & POLLIN) {
				FD_SET(tmp[i].fd, rfds);
				if (tmp[i].fd > maxfd)
					maxfd = tmp[i].fd;
			}
		}
	}
	return maxfd;
}

int lib_oss_seq_select_result(int fd, fd_set *rfds,
			      fd_set *wfds __attribute__((unused)),
			      fd_set *efds __attribute__((unused)))
{
	oss_seq_t *dp = find_seq(fd);
	struct pollfd tmp[8];
	int result = 0, n, i;

	if (!dp)
		return OSS_WAIT_EVENT_ERROR;
	if (dp->readq && rfds) {
		if (FD_ISSET(dp->readq->pipe_rd, rfds))
			result |= OSS_WAIT_EVENT_READ;
		n = snd_seq_poll_descriptors_count(dp->seq, POLLIN);
		if (n > (int)(sizeof(tmp) / sizeof(tmp[0])))
			n = sizeof(tmp) / sizeof(tmp[0]);
		n = snd_seq_poll_descriptors(dp->seq, tmp, n, POLLIN);
		for (i = 0; i < n; i++) {
			if ((tmp[i].events & POLLIN) && FD_ISSET(tmp[i].fd, rfds)) {
				result |= OSS_WAIT_EVENT_READ;
				break;
			}
		}
	}
	return result;
}
