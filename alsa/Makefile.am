lib_LTLIBRARIES = libalsatoss.la libaoss.la
bin_SCRIPTS = aoss
man_MANS = aoss.1
EXTRA_DIST = aoss.1

noinst_HEADERS = alsa-oss-emul.h

libaoss_la_SOURCES = alsa-oss.c
libaoss_la_LIBADD = libalsatoss.la

libalsatoss_la_CFLAGS = @ALSA_CFLAGS@
libalsatoss_la_SOURCES = pcm.c mixer.c
libalsatoss_la_LIBADD = @ALSA_LIBS@
