mips.o: ../../mipseb/mips.c ../../include/config.h ../../include/cpu.h \
 ../../include/bus.h ../../include/console.h ../../include/clock.h \
 ../../include/gdb.h ../../include/main.h ../../mipseb/mips-insn.h \
 ../../mipseb/mips-ex.h ../../mipseb/bootrom.h
bootrom.o: ../../mipseb/bootrom.c ../../mipseb/bootrom.h
lamebus.o: ../../bus/lamebus.c ../../include/bus.h ../../include/cpu.h \
 ../../include/speed.h ../../include/console.h ../../include/gdb.h \
 ../../include/onsel.h ../../include/main.h ../../bus/lamebus.h \
 ../../bus/busids.h ../../bus/busdefs.h ../../bus/ram.h
boot.o: ../../bus/boot.c ../../include/console.h ../../include/cpu.h \
 ../../include/elf.h ../../bus/ram.h
dev_disk.o: ../../bus/dev_disk.c ../../include/console.h \
 ../../include/clock.h ../../include/main.h ../../include/util.h \
 ../../bus/lamebus.h ../../bus/busids.h
dev_emufs.o: ../../bus/dev_emufs.c ../../include/util.h \
 ../../include/console.h ../../bus/lamebus.h ../../bus/busids.h
dev_net.o: ../../bus/dev_net.c ../../bus/lamebus.h
dev_random.o: ../../bus/dev_random.c ../../include/console.h \
 ../../bus/lamebus.h ../../bus/busids.h
dev_screen.o: ../../bus/dev_screen.c ../../bus/lamebus.h
dev_serial.o: ../../bus/dev_serial.c ../../include/speed.h \
 ../../include/console.h ../../include/clock.h ../../include/main.h \
 ../../include/util.h ../../bus/busids.h ../../bus/lamebus.h
dev_timer.o: ../../bus/dev_timer.c ../../include/bus.h \
 ../../include/console.h ../../include/speed.h ../../include/cpu.h \
 ../../include/clock.h ../../include/util.h ../../bus/busids.h \
 ../../bus/lamebus.h
gdb_fe.o: ../../gdb/gdb_fe.c ../../include/console.h \
 ../../include/onsel.h ../../include/cpu.h ../../include/gdb.h \
 ../../include/main.h ../../gdb/context.h
gdb_be.o: ../../gdb/gdb_be.c ../../include/console.h \
 ../../include/gdb.h ../../include/cpu.h ../../include/bus.h \
 ../../include/main.h ../../gdb/context.h
main.o: ../../main/main.c ../../include/console.h ../../include/gdb.h \
 ../../include/cpu.h ../../include/bus.h ../../include/clock.h \
 ../../include/speed.h ../../include/onsel.h ../../include/main.h
onsel.o: ../../main/onsel.c ../../include/console.h \
 ../../include/onsel.h
clock.o: ../../main/clock.c ../../include/console.h \
 ../../include/speed.h ../../include/clock.h ../../include/cpu.h \
 ../../include/bus.h ../../include/onsel.h ../../include/main.h
console.o: ../../main/console.c ../../include/onsel.h \
 ../../include/console.h
util.o: ../../main/util.c ../../include/console.h ../../include/util.h
