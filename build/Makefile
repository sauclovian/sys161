CPUS=
CPUS+=mipseb

MYCPU=mipseb
TYPES=fast trace

all depend tidy clean distclean:
	@for c in $(CPUS); do for t in $(TYPES); do \
		echo "*** $${c}-$${t}"; \
		(cd $${c}-$${t} && $(MAKE) $@); \
		done; done

install:
	@for c in $(MYCPU); do for t in $(TYPES); do \
		echo "*** $${c}-$${t}"; \
		(cd $${c}-$${t} && $(MAKE) $@); \
		done; done
