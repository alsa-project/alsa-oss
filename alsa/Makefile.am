lib_LTLIBRARIES = libalsatoss.la libaoss.la
bin_SCRIPTS = aoss
man_MANS = aoss.1
EXTRA_DIST = aoss.1

libaoss_la_SOURCES = alsa-oss.c
libaoss_la_LIBADD = -ldl -lalsatoss

libalsatoss_la_SOURCES = pcm.c mixer.c
libalsatoss_la_LIBADD = -lasound

clean:
	rm -f aoss
