#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/soundcard.h>
#include <sys/time.h>
#include <sys/poll.h>
#include <oss-redir.h>

//static char data[500000];
static int verbose;
static char *device = "/dev/dsp";
static int format = AFMT_S16_LE;
static int rate = 48000;
static int channels = 2;
static int omode = O_RDWR;
static int frag = 0xffff000c;	/* Max # periods of 2^13=8k bytes */
static int fd;
static audio_buf_info ospace;
static audio_buf_info ispace;
static int bufsize;
static int fragsize;
static char *wbuf = NULL, *rbuf = NULL;
static int loop = 40;

static void help(void)
{
	printf(
"Usage: mmap_test [OPTION]...\n"
"-h,--help      help\n"
"-D,--device    playback device\n"
"-r,--rate      stream rate in Hz\n"
"-c,--channels  count of channels in stream\n"
//"-f,--frequency	sine wave frequency in Hz\n"
"-F,--frag      OSS fragment settings (SNDCTL_DSP_SETFRAGMENT)\n"
"-M,--omode     open mode (read/write/duplex)\n"
"-m,--method    transfer method (rw, mmap_and_select, mmap_and_poll)\n"
"-L,--loop      set loop count\n"
"-v,--verbose   show more info\n"
"\n");
}

static void set_params(int do_mmap)
{
	int caps;

	if (oss_pcm_ioctl(fd, SNDCTL_DSP_SETFMT, &format) < 0) {
		perror("SNDCTL_DSP_SETFMT\n");
		exit(EXIT_FAILURE);
	}
	printf("Format set to %d\n", format);

	if (oss_pcm_ioctl(fd, SNDCTL_DSP_SPEED, &rate) < 0) {
		perror("SNDCTL_DSP_SPEED\n");
		exit(EXIT_FAILURE);
	}
	printf("Rate set to %d\n", rate);

	if (oss_pcm_ioctl(fd, SNDCTL_DSP_CHANNELS, &channels) < 0) {
		perror("SNDCTL_DSP_CHANNELS\n");
		exit(EXIT_FAILURE);
	}
	printf("Channels set to %d\n", channels);

	if (oss_pcm_ioctl(fd, SNDCTL_DSP_GETCAPS, &caps) < 0) {
		perror("/dev/dsp");
		fprintf(stderr, "Sorry but your sound driver is too old\n");
		exit(EXIT_FAILURE);
	}
	if (do_mmap &&
            (!(caps & DSP_CAP_TRIGGER) ||
	     !(caps & DSP_CAP_MMAP)))
	{
		fprintf(stderr, "Sorry but your soundcard can't do this\n");
		exit(EXIT_FAILURE);
	}
	if (oss_pcm_ioctl(fd, SNDCTL_DSP_SETFRAGMENT, &frag) < 0)
		perror("SNDCTL_DSP_SETFRAGMENT");
	bufsize = fragsize = -1;
	if (omode == O_RDWR || omode == O_WRONLY) {
		if (oss_pcm_ioctl(fd, SNDCTL_DSP_GETOSPACE, &ospace) < 0) {
			perror("SNDCTL_DSP_GETOSPACE");
			exit(EXIT_FAILURE);
		}
		bufsize = ospace.fragstotal * ospace.fragsize;
		fragsize = ospace.fragsize;
		printf("ospace.fragstotal = %i\n", ospace.fragstotal);
		printf("ospace.fragsize = %i\n", ospace.fragsize);
		printf("ospace.periods = %i\n", ospace.fragments);
		printf("ospace.bytes = %i\n", ospace.bytes);
		if (do_mmap) {
			if ((wbuf=mmap(NULL, bufsize, PROT_WRITE, MAP_FILE|MAP_SHARED, fd, 0))==MAP_FAILED) {
				perror("mmap (write)");
				exit(-1);
			}
			printf("mmap (out) returned %p\n", wbuf);
		}
	}
	if (omode == O_RDWR || omode == O_RDONLY) {
		if (oss_pcm_ioctl(fd, SNDCTL_DSP_GETISPACE, &ispace) < 0) {
			perror("SNDCTL_DSP_GETISPACE");
			if (omode == O_RDWR) {
				omode = O_WRONLY;
				fprintf(stderr, "Falling to write only mode\n");
			} else {
				exit(EXIT_FAILURE);
			}
		}
		if (omode != O_WRONLY) {
			if (bufsize < 0) {
				bufsize = ispace.fragstotal * ispace.fragsize;
				fragsize = ispace.fragsize;
			}
			printf("ispace.fragstotal = %i\n", ispace.fragstotal);
			printf("ispace.fragsize = %i\n", ispace.fragsize);
			printf("ispace.periods = %i\n", ispace.fragments);
			printf("ispace.bytes = %i\n", ispace.bytes);
			if (do_mmap) {
				if ((rbuf=mmap(NULL, bufsize, PROT_READ, MAP_FILE|MAP_SHARED, fd, 0))==MAP_FAILED) {
					perror("mmap (read)");
					exit(-1);
				}
				printf("mmap (in) returned %p\n", rbuf);
			}
		}
	}
}

static void set_trigger(void)
{
	int tmp;

	tmp = 0;
	if (oss_pcm_ioctl(fd, SNDCTL_DSP_SETTRIGGER, &tmp) < 0) {
		perror("SNDCTL_DSP_SETTRIGGER");
		exit(EXIT_FAILURE);
	}
	printf("Trigger set to %08x\n", tmp);

	if (omode == O_RDWR)
		tmp = PCM_ENABLE_OUTPUT|PCM_ENABLE_INPUT;
	else if (omode == O_RDONLY)
		tmp = PCM_ENABLE_INPUT;
	else if (omode == O_WRONLY)
		tmp = PCM_ENABLE_OUTPUT;
	if (oss_pcm_ioctl(fd, SNDCTL_DSP_SETTRIGGER, &tmp) < 0) {
		perror("SNDCTL_DSP_SETTRIGGER");
		exit(EXIT_FAILURE);
	}
	printf("Trigger set to %08x\n", tmp);
}

static void rw_loop(void)
{
	int idx, first = 1;

	for (idx=0; idx<loop; idx++) {
		char buf[1000];
		if (omode != O_RDONLY) {
			ssize_t res;
			if (first) {
				res = oss_pcm_write(fd, buf, sizeof(buf));
				if (verbose)
					printf("write: (%i) -> %i\n", sizeof(buf), (int)res);
			}
			res = oss_pcm_write(fd, buf, sizeof(buf));
			if (verbose)
				printf("write: (%i) -> %i\n", sizeof(buf), (int)res);
			first = 0;
		}
		if (omode != O_WRONLY) {
			ssize_t res = oss_pcm_read(fd, buf, sizeof(buf));
			if (verbose)
				printf("read: (%i) -> %i\n", sizeof(buf), (int)res);
		}
	}
}

static void rw_and_select_loop(void)
{
}

static void rw_and_poll_loop(void)
{
}

static void mmap_loop(void)
{
}

static void mmap_and_select_loop(void)
{
	int nfrag_in = 0, nfrag_out = 0, idx;
	struct timeval tim;
	fd_set writeset, readset;

	for (idx=0; idx<loop; idx++) {
		struct count_info count_in, count_out;
		int res, maxfd;

		FD_ZERO(&writeset);
		FD_ZERO(&readset);
		maxfd = oss_pcm_select_prepare(fd, omode, &readset, &writeset, NULL);

		tim.tv_sec = 10;
		tim.tv_usec = 0;

		res = select(maxfd + 1, &readset, &writeset, NULL, &tim);
		if (res < 0)
			perror("select\n");
		if (verbose) {
			printf("Select returned: %03d\n", res);
			fflush(stdout);
		}
		if (omode != O_WRONLY) {
			if (oss_pcm_ioctl(fd, SNDCTL_DSP_GETIPTR, &count_in) < 0) {
				perror("GETOPTR");
				exit(EXIT_FAILURE);
			}
			nfrag_in += count_in.blocks;
			if (verbose) {
				printf("GETIPTR: Total: %09d, Period: %03d, Ptr: %06d\n", count_in.bytes, nfrag_in, count_in.ptr);
				fflush(stdout);
			}
		}
		if (omode != O_RDONLY) {
			if (oss_pcm_ioctl(fd, SNDCTL_DSP_GETOPTR, &count_out) < 0) {
				perror("GETOPTR");
				exit(EXIT_FAILURE);
			}
			nfrag_out += count_out.blocks;
			if (verbose) {
				printf("GETOPTR: Total: %09d, Period: %03d, Ptr: %06d\n", count_out.bytes, nfrag_out, count_out.ptr);
				fflush(stdout);
			}
		}
	}
}

static void mmap_and_poll_loop(void)
{
	int nfrag_in = 0, nfrag_out = 0, idx;
	int fd_count;
	
	fd_count = oss_pcm_poll_fds(fd);

	for (idx=0; idx<loop; idx++) {
		struct pollfd ufds[fd_count];
		struct count_info count_in, count_out;
		int res;

		oss_pcm_poll_prepare(fd, omode, ufds);

		res = poll(ufds, fd_count, 10000);
		if (res < 0)
			perror("poll\n");
		if (verbose) {
			printf("Poll returned: %03d\n", res);
			fflush(stdout);
		}
		if (omode != O_WRONLY) {
			if (oss_pcm_ioctl(fd, SNDCTL_DSP_GETIPTR, &count_in) < 0) {
				perror("GETOPTR");
				exit(EXIT_FAILURE);
			}
			nfrag_in += count_in.blocks;
			if (verbose) {
				printf("GETIPTR: Total: %09d, Period: %03d, Ptr: %06d\n", count_in.bytes, nfrag_in, count_in.ptr);
				fflush(stdout);
			}
		}
		if (omode != O_RDONLY) {
			if (oss_pcm_ioctl(fd, SNDCTL_DSP_GETOPTR, &count_out) < 0) {
				perror("GETOPTR");
				exit(EXIT_FAILURE);
			}
			nfrag_out += count_out.blocks;
			if (verbose) {
				printf("GETOPTR: Total: %09d, Period: %03d, Ptr: %06d\n", count_out.bytes, nfrag_out, count_out.ptr);
				fflush(stdout);
			}
		}
	}
}

struct transfer_method {
	const char *name;
	int do_mmap;
	void (*transfer_loop)(void);
};

static struct transfer_method transfer_methods[] = {
	{ "rw", 0, rw_loop },
	{ "rw_and_select", 0, rw_and_select_loop },
	{ "rw_and_poll", 0, rw_and_poll_loop },
	{ "mmap", 1, mmap_loop },
	{ "mmap_and_select", 1, mmap_and_select_loop },
	{ "mmap_and_poll", 1, mmap_and_poll_loop },
	{ NULL, 0, NULL }
};

int main(int argc, char *argv[])
{
	int morehelp = 0;
	int method = 0;
        struct option long_option[] =
        {
		{"help", 0, NULL, 'h'},
		{"device", 1, NULL, 'D'},
                {"verbose", 1, NULL, 'v'},
		{"omode", 1, NULL, 'M'},
		{"mode", 1, NULL, 'm'},
		{"rate", 1, NULL, 'r'},
		{"channels", 1, NULL, 'c'},
		{"frag", 1, NULL, 'F'},
		{"loop", 1, NULL, 'L'},
                {NULL, 0, NULL, 0},
        };

        morehelp = 0;
	while (1) {
		int c;
		if ((c = getopt_long(argc, argv, "hD:M:m:r:c:F:L:v", long_option, NULL)) < 0)
			break;
		switch (c) {
		case 'h':
			morehelp++;
			break;
		case 'D':
			device = strdup(optarg);
			break;
		case 'M':
			if (!strcmp(optarg, "read"))
				omode = O_RDONLY;
			else if (!strcmp(optarg, "write"))
				omode = O_WRONLY;
			else
				omode = O_RDWR;
			break;
		case 'm':
			for (method = 0; transfer_methods[method].name; method++)
				if (!strcasecmp(transfer_methods[method].name, optarg))
					break;
			if (transfer_methods[method].name == NULL)
				method = 0;
			break;
		case 'r':
			rate = atoi(optarg);
			break;
		case 'c':
			channels = atoi(optarg);
			break;
		case 'F':
			frag = atoi(optarg);
			break;
		case 'L':
			loop = atoi(optarg);
			break;
		case 'v':
			verbose = 1;
			break;
		}
	}

        if (morehelp) {
                help();
                return 0;
        }

	printf("Using transfer method %s\n", transfer_methods[method].name);

	if ((fd=oss_pcm_open(device, O_RDWR, 0))==-1) {
		perror(device);
		exit(-1);
	}

	printf("Device %s opened sucessfully\n", device);

	set_params(transfer_methods[method].do_mmap);
	if (transfer_methods[method].do_mmap)
		set_trigger();

	transfer_methods[method].transfer_loop();

	close(fd);

	exit(0);
}
