if WITH_AOSS
ALSA_DIR = alsa
else
ALSA_DIR =
endif

SUBDIRS = $(ALSA_DIR) oss-redir test
AUTOMAKE_OPTIONS = foreign

dist-hook:
	echo $(VERSION) >> version
	-chmod -R a+r $(distdir)
	@if ! test -z "$(AMTAR)"; then \
		$(AMTAR) --create --verbose --file=- $(distdir) | bzip2 -c -9 > $(distdir).tar.bz2 ; \
	else \
		$(TAR) --create --verbose --file=- $(distdir) | bzip2 -c -9 > $(distdir).tar.bz2 ; \
	fi
