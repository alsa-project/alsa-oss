check_PROGRAMS=osstest lmixer

osstest_LDADD=../oss-redir/libossredir.la
lmixer_LDADD=../oss-redir/libossredir.la
lmixer_SOURCES=lmixer.cc

noinst_HEADERS = mixctl.h

INCLUDES=-I$(top_srcdir)/oss-redir
AM_CFLAGS=-static -Wall -pipe -g

EXTRA_DIST=

test_mmap_test: mmap_test_redir
	OSS_REDIRECTOR=libalsatoss.so mmap_test_redir
