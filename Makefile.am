lib_LTLIBRARIES = libaoss.la

libaoss_la_SOURCES = alsa-oss.c
libaoss_la_LIBADD = -ldl -lasound

dist-hook:
	echo $(VERSION) >> version
	-chmod -R a+r $(distdir)
	$(TAR) cvIf $(distdir).tar.bz2 $(distdir)
