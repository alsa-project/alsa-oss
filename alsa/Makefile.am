lib_LTLIBRARIES = libaoss.la
bin_SCRIPTS = aoss
man_MANS = aoss.1
EXTRA_DIST = aoss.1

libaoss_la_SOURCES = alsa-oss.c
libaoss_la_LIBADD = -ldl -lasound

clean:
	rm -f aoss
