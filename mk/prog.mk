#
# Makefile fragment for building programs
#

all: $(PROG)

include rules.mk
include depend.mk

distclean clean:
	rm -f *.o $(PROG)

rules:
	@echo Making rules...
	@echo $(SRCLIST) | $S/mk/makerules.sh > rules.mk

depend:
	$(MAKE) rules
	$(MAKE) realdepend

realdepend:
	$(CC) $(CFLAGS) $(DEPINCLUDES) -MM $(SRCS) > depend.mk

install:
	(umask 022; \
		[ -d "$(DESTDIR)$(BINDIR)" ] || mkdir -p $(DESTDIR)$(BINDIR))
	$S/mk/installit.sh "$(DESTDIR)$(BINDIR)" "$(PROG)" "$(VERSION)"

$(PROG): $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) $(LIBS) -lm -o $(PROG)
