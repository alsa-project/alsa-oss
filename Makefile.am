lib_LTLIBRARIES = libaoss.la
bin_SCRIPTS = aoss
man_MANS = aoss.1
EXTRA_DIST = aoss.1

libaoss_la_SOURCES = alsa-oss.c
libaoss_la_LIBADD = -ldl -lasound


clean:
	rm -f aoss

dist-hook:
	echo $(VERSION) >> version
	-chmod -R a+r $(distdir)
	@if ! test -z "$(AMTAR)"; then \
		$(AMTAR) --create --verbose --file=- $(distdir) | bzip2 -c -9 > $(distdir).tar.bz2 ; \
	else \
		$(TAR) --create --verbose --file=- $(distdir) | bzip2 -c -9 > $(distdir).tar.bz2 ; \
	fi
