#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/soundcard.h>
#include <sys/time.h>
#include <oss-redir.h>

int main(void)
{
	int fd, sz, fsz, i, tmp, n, l, have_data=0, nfrag;
        int caps, idx;

	int sd, sl=0, sp;

	unsigned char data[500000], *dp = data;

	struct buffmem_desc imemd, omemd;
        caddr_t buf;
	struct timeval tim;

	unsigned char *op;
	
        struct audio_buf_info info;

	int frag = 0xffff000c;	/* Max # periods of 2^13=8k bytes */

	fd_set writeset;

	if ((fd=oss_pcm_open("/dev/dsp", O_RDWR, 0))==-1) {
		perror("/dev/dsp");
		exit(-1);
	}
	tmp = 48000;
	if (oss_pcm_ioctl(fd, SNDCTL_DSP_SPEED, &tmp) < 0) {
		perror("SNDCTL_DSP_SPEED\n");
		exit(EXIT_FAILURE);
	}
	printf("Speed set to %d\n", tmp);

	sl = sp = 0;
	if ((sd=open("smpl", O_RDONLY, 0)) >= 0) {
		sl = read(sd, data, sizeof(data));
		printf("%d bytes read from file.\n", sl);
		close(sd);
	} else
		perror("smpl");

	if (oss_pcm_ioctl(fd, SNDCTL_DSP_GETCAPS, &caps) < 0) {
		perror("/dev/dsp");
		fprintf(stderr, "Sorry but your sound driver is too old\n");
		exit(EXIT_FAILURE);
	}
	if (!(caps & DSP_CAP_TRIGGER) ||
	    !(caps & DSP_CAP_MMAP))
	{
		fprintf(stderr, "Sorry but your soundcard can't do this\n");
		exit(EXIT_FAILURE);
	}
	if (oss_pcm_ioctl(fd, SNDCTL_DSP_SETFRAGMENT, &frag) < 0)
		perror("SNDCTL_DSP_SETFRAGMENT");
	if (oss_pcm_ioctl(fd, SNDCTL_DSP_GETOSPACE, &info) < 0) {
		perror("SNDCTL_DSP_GETOSPACE");
		exit(EXIT_FAILURE);
	}
	sz = info.fragstotal * info.fragsize;
	fsz = info.fragsize;
	printf("info.fragstotal = %i\n", info.fragstotal);
	printf("info.fragsize = %i\n", info.fragsize);
	printf("info.periods = %i\n", info.fragments);
	printf("info.bytes = %i\n", info.bytes);
	if ((buf=mmap(NULL, sz, PROT_WRITE, MAP_FILE|MAP_SHARED, fd, 0))==MAP_FAILED) {
		perror("mmap (write)");
		exit(-1);
	}
	printf("mmap (out) returned %08x\n", buf);
	op=buf;

	tmp = 0;
	if (oss_pcm_ioctl(fd, SNDCTL_DSP_SETTRIGGER, &tmp) < 0) {
		perror("SNDCTL_DSP_SETTRIGGER");
		exit(EXIT_FAILURE);
	}
	printf("Trigger set to %08x\n", tmp);

	tmp = PCM_ENABLE_OUTPUT;
	if (oss_pcm_ioctl(fd, SNDCTL_DSP_SETTRIGGER, &tmp) < 0) {
		perror("SNDCTL_DSP_SETTRIGGER");
		exit(EXIT_FAILURE);
	}
	printf("Trigger set to %08x\n", tmp);

	nfrag = 0;
	for (idx=0; idx<40; idx++) {
		struct count_info count;
		int p, l, extra;

		FD_ZERO(&writeset);
		FD_SET(fd, &writeset);

		tim.tv_sec = 10;
		tim.tv_usec= 0;

		select(fd+1, NULL, &writeset, NULL, NULL);
		if (oss_pcm_ioctl(fd, SNDCTL_DSP_GETOPTR, &count) < 0) {
			perror("GETOPTR");
			exit(EXIT_FAILURE);
		}
		nfrag += count.blocks;
#ifdef VERBOSE
		printf("Total: %09d, Period: %03d, Ptr: %06d", count.bytes, nfrag, count.ptr);
		fflush(stdout);
#endif
		count.ptr = (count.ptr/fsz)*fsz;

#ifdef VERBOSE
		printf(" memcpy(%6d, %4d)\n", (dp-data), fsz);
		fflush(stdout);
#endif

/*
 * Set few bytes in the beginning of next period too.
 */
		if ((count.ptr+fsz+16) < sz)	/* Last period? */
		   extra = 16;
		else
		   extra = 0;

		memcpy(op+count.ptr, dp, fsz+extra);

		dp += fsz;
		if (dp > (data+sl-fsz))
		   dp = data;
	}

	close(fd);

	printf("second open test:\n");
	if ((fd=oss_pcm_open("/dev/dsp", O_RDWR, 0))==-1) {
		perror("/dev/dsp");
		exit(-1);
	}
	close(fd);
	printf("second open test passed\n");

	exit(0);
}
