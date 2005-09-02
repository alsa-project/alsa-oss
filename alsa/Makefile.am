lib_LTLIBRARIES = libalsatoss.la libaoss.la
bin_SCRIPTS = aoss
noinst_SCRIPTS = aoss.old
man_MANS = aoss.1
EXTRA_DIST = aoss.1
COMPATNUM=@LIBTOOL_VERSION_INFO@

noinst_HEADERS = alsa-oss-emul.h alsa-local.h

EXTRA_libaoss_la_SOURCES = stdioemu.c
libaoss_la_SOURCES = alsa-oss.c
libaoss_la_LIBADD = libalsatoss.la
libaoss_la_LDFLAGS = -version-info $(COMPATNUM)

libalsatoss_la_CFLAGS = @ALSA_CFLAGS@
libalsatoss_la_SOURCES = pcm.c mixer.c
libalsatoss_la_LIBADD = @ALSA_LIBS@
libalsatoss_la_LDFLAGS = -version-info $(COMPATNUM)
