check_PROGRAMS=mmap_test mmap_test_redir

mmap_test_redir_LDADD=../oss-redir/libossredir.la

INCLUDES=-I$(top_srcdir)/oss-redir
CFLAGS=-static -Wall -pipe -g

EXTRA_DIST=

test_mmap_test: mmap_test_redir
	OSS_REDIRECTOR=libalsatoss.so mmap_test_redir
