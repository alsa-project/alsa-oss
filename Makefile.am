lib_LTLIBRARIES = libaoss.la
bin_SCRIPTS = aoss
man_MANS = aoss.1

libaoss_la_SOURCES = alsa-oss.c
libaoss_la_LIBADD = -ldl -lasound

aoss: aoss.in
	sed -e "s+PREFIX+$(libdir)+" $(srcdir)/aoss.in > aoss

clean:
	rm -f aoss

dist-hook:
	echo $(VERSION) >> version
	-chmod -R a+r $(distdir)
	$(TAR) cvIf $(distdir).tar.bz2 $(distdir)
