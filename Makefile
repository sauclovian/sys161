all depend install tidy clean:
	(cd build && $(MAKE) $@)

distclean:
	(cd build && $(MAKE) $@)
	rm -f include/config.h
	rm -f build/mk/defs.mk
