ROOT := .
DIRS := libcoz viewer

include $(ROOT)/common.mk

bench::
	@for dir in benchmarks ; do make -C $$dir $@; done

check::
	make -C libcoz $@

install::
	$(INSTALL) -D coz $(DESTDIR)$(bindir)/coz
	$(INSTALL) -D include/coz.h $(DESTDIR)$(incdir)/coz.h
	$(RST2MAN) docs/coz.rst $(DESTDIR)$(man1dir)/coz.1

distclean::
	@for dir in deps/libelfin ; do \
	if [ -d $$dir ] ; then make -C $$dir distclean || make -C $$dir clean; fi; \
	done
