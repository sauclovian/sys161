CFLAGS+=-I$S/include
SRCFILES+=bus     lamebus.c boot.c \
                  dev_disk.c dev_emufs.c dev_net.c dev_random.c \
                  dev_screen.c dev_serial.c dev_timer.c \
          gdb     gdb_fe.c gdb_be.c \
          main    main.c onsel.c clock.c console.c util.c

tidy:
	(find $S -name '*~' -print | xargs rm -f)

distclean clean: #tidy
	rm -f *.o $(PROG)

rules:
	echo $(SRCFILES) | ../mk/makerules.sh > rules.mk

depend:
	$(MAKE) rules
	$(MAKE) realdepend

realdepend:
	$(CC) $(CFLAGS) $(DEPINCLUDES) -MM $(SRCS) > depend.mk

install:
	cp $(PROG) $(INSTALLDIR)/$(PROG)

$(PROG): $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) $(LIBS) -o $(PROG)
