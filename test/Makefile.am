check_PROGRAMS=mmap_test lmixer

mmap_test_LDADD=../oss-redir/libossredir.la
lmixer_LDADD=../oss-redir/libossredir.la
lmixer_SOURCES=lmixer.cc

INCLUDES=-I$(top_srcdir)/oss-redir
CFLAGS=-static -Wall -pipe -g

EXTRA_DIST=

test_mmap_test: mmap_test_redir
	OSS_REDIRECTOR=libalsatoss.so mmap_test_redir
