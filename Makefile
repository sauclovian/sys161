all depend install tidy clean distclean:
	(cd build && $(MAKE) $@)

distclean: distcleanhere
distcleanhere:
	rm -f include/config.h
	rm -f build/mk/defs.mk
