mips.o: ../../mipseb/mips.c ../../include/config.h ../../include/cpu.h \
 ../../include/bus.h ../../include/console.h ../../include/clock.h \
 ../../include/gdb.h ../../include/main.h ../../mipseb/mips-insn.h \
 ../../mipseb/mips-ex.h ../../mipseb/bootrom.h
bootrom.o: ../../mipseb/bootrom.c ../../include/config.h \
 ../../mipseb/bootrom.h
lamebus.o: ../../bus/lamebus.c ../../include/config.h \
 ../../include/bus.h ../../include/cpu.h ../../include/speed.h \
 ../../include/console.h ../../include/gdb.h ../../include/onsel.h \
 ../../include/clock.h ../../include/main.h ../../bus/lamebus.h \
 ../../bus/busids.h ../../bus/busdefs.h ../../bus/ram.h
boot.o: ../../bus/boot.c ../../include/config.h \
 ../../include/console.h ../../include/cpu.h ../../include/elf.h \
 ../../bus/ram.h
dev_disk.o: ../../bus/dev_disk.c ../../include/config.h \
 ../../include/console.h ../../include/clock.h ../../include/main.h \
 ../../include/util.h ../../bus/lamebus.h ../../bus/busids.h
dev_emufs.o: ../../bus/dev_emufs.c ../../include/config.h \
 ../../include/util.h ../../include/console.h ../../bus/lamebus.h \
 ../../bus/busids.h
dev_net.o: ../../bus/dev_net.c ../../include/config.h \
 ../../bus/lamebus.h
dev_random.o: ../../bus/dev_random.c ../../include/config.h \
 ../../include/console.h ../../bus/lamebus.h ../../bus/busids.h
dev_screen.o: ../../bus/dev_screen.c ../../include/config.h \
 ../../bus/lamebus.h
dev_serial.o: ../../bus/dev_serial.c ../../include/config.h \
 ../../include/speed.h ../../include/console.h ../../include/clock.h \
 ../../include/main.h ../../include/util.h ../../bus/busids.h \
 ../../bus/lamebus.h
dev_timer.o: ../../bus/dev_timer.c ../../include/config.h \
 ../../include/bus.h ../../include/console.h ../../include/speed.h \
 ../../include/cpu.h ../../include/clock.h ../../include/util.h \
 ../../bus/busids.h ../../bus/lamebus.h
gdb_fe.o: ../../gdb/gdb_fe.c ../../include/config.h \
 ../../include/console.h ../../include/onsel.h ../../include/cpu.h \
 ../../include/gdb.h ../../include/main.h ../../gdb/context.h
gdb_be.o: ../../gdb/gdb_be.c ../../include/config.h \
 ../../include/console.h ../../include/gdb.h ../../include/cpu.h \
 ../../include/bus.h ../../include/main.h ../../gdb/context.h
main.o: ../../main/main.c ../../include/config.h \
 ../../include/console.h ../../include/gdb.h ../../include/cpu.h \
 ../../include/bus.h ../../include/clock.h ../../include/speed.h \
 ../../include/onsel.h ../../include/main.h
onsel.o: ../../main/onsel.c ../../include/config.h \
 ../../include/console.h ../../include/onsel.h
clock.o: ../../main/clock.c ../../include/config.h \
 ../../include/console.h ../../include/speed.h ../../include/clock.h \
 ../../include/cpu.h ../../include/bus.h ../../include/onsel.h \
 ../../include/main.h
console.o: ../../main/console.c ../../include/config.h \
 ../../include/onsel.h ../../include/console.h
util.o: ../../main/util.c ../../include/console.h ../../include/util.h
