#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include "config.h"

#include "bswap.h"
#include "console.h"
#include "bus.h"
#include "cpu.h"
#include "memdefs.h"
#include "prof.h"
#include "elf.h"
#include "cpu-elf.h"


static
void
doread(int fd, uint32_t pos, void *buf, size_t len)
{
	int r;

	if (lseek(fd, pos, SEEK_SET)<0) {
		msg("lseek on boot image: %s", strerror(errno));
		die();
	}

	r = read(fd, buf, len);
	if (r<0) {
		msg("read: boot image: %s", strerror(errno));
		die();
	}
	else if ((size_t)r < len) {
		msg("read: boot image: unexpected EOF");
		die();
	}
}


static
void
load_elf(int fd)
{
	Elf_Ehdr eh;
	Elf_Phdr ph;
	uint32_t paddr, i;
	uint32_t rambase;

	rambase = cpu_get_ram_paddr();

	doread(fd, 0, &eh, sizeof(eh));

	if (eh.e_ident[EI_MAG0] != ELFMAG0 ||
	    eh.e_ident[EI_MAG1] != ELFMAG1 ||
	    eh.e_ident[EI_MAG2] != ELFMAG2 ||
	    eh.e_ident[EI_MAG3] != ELFMAG3) {
		msg("Boot image is not an ELF executable");
		die();
	}

	if (eh.e_ident[EI_CLASS] != ELFCLASS_CPU) {
		msg("Boot image is not a 32-bit executable");
		die();
	}

	if (eh.e_ident[EI_DATA] != ELFDATA_CPU) {
		msg("Boot image has the wrong endianness");
		die();
	}

	eh.e_type = ctoh16(eh.e_type);
	eh.e_machine = ctoh16(eh.e_machine);
	eh.e_version = ctoh32(eh.e_version);
	eh.e_entry = ctoh32(eh.e_entry);
	eh.e_phoff = ctoh32(eh.e_phoff);
	eh.e_phentsize = ctoh16(eh.e_phentsize);
	eh.e_phnum = ctoh16(eh.e_phnum);

	if (eh.e_ident[EI_VERSION] != EV_CURRENT ||
	    eh.e_version != EV_CURRENT) {
		msg("Boot image is wrong ELF version");
		die();
	}

	/* Ignore EI_OSABI and EI_ABIVERSION */

	if (eh.e_type!=ET_EXEC) {
		msg("Boot image is ELF but not an executable");
		die();
	}

	if (eh.e_machine!=EM_CPU) {
		msg("Boot image is for wrong processor type");
		die();
	}

	for (i=0; i<eh.e_phnum; i++) {
		doread(fd, eh.e_phoff + i*eh.e_phentsize, &ph, sizeof(ph));

		ph.p_type = ctoh32(ph.p_type);
		ph.p_offset = ctoh32(ph.p_offset);
		ph.p_vaddr = ctoh32(ph.p_vaddr);
		ph.p_filesz = ctoh32(ph.p_filesz);
		ph.p_memsz = ctoh32(ph.p_memsz);
		ph.p_flags = ctoh32(ph.p_flags);

		switch (ph.p_type) {
		    case PT_NULL: /* skip */ continue;
		    case PT_PHDR: /* skip */ continue;
		    case PT_MIPS_REGINFO: /* skip */ continue;
		    case PT_NOTE: /* skip */ continue;
		    case PT_LOAD: break;
		    default:
			msg("Boot image contained unknown segment type %d", 
			    ph.p_type);
			die();
		}

		/*
		 * Virtual address must be in range.
		 */

		if (cpu_get_load_paddr(ph.p_vaddr, ph.p_memsz, &paddr)) {
			msg("Invalidly placed segment in boot image"
			    " (load address %u, size %u)", 
			    ph.p_vaddr, ph.p_memsz);
			die();
		}

		if (paddr + ph.p_memsz >= rambase + bus_ramsize) {
			msg("Boot image contained segment that did not"
			    " fit in RAM");
			die();
		}

		if (ph.p_filesz > ph.p_memsz) {
			ph.p_filesz = ph.p_memsz;
		}

		if (ph.p_flags & PF_X) {
			prof_addtext(ph.p_vaddr, ph.p_memsz);
		}

		doread(fd, ph.p_offset, ram + paddr - rambase, ph.p_filesz);
		paddr += ph.p_filesz;
		bzero(ram + paddr - rambase, ph.p_memsz - ph.p_filesz);
	}

	cpu_set_entrypoint(0, eh.e_entry);
}

static
void
setstack(const char *argument)
{
	uint32_t vaddr, paddr;
	uint32_t size;
	uint32_t rambase;

	rambase = cpu_get_ram_paddr();

	size = strlen(argument) + 1;

	/* align size upwards */
	size = (size+3) & ~(uint32_t)3;

	paddr = rambase + bus_ramsize - size;

	strcpy(ram + paddr - rambase, argument);

	/* convert to virtual addr */
	if (cpu_get_load_vaddr(paddr, size, &vaddr)) {
		msg("setstack: could not get initial stack vaddr");
		die();
	}

	cpu_set_stack(0, vaddr-4, vaddr);
}

void
load_kernel(const char *image, const char *argument)
{
	int fd;
	fd = open(image, O_RDONLY);
	if (fd<0) {
		msg("Cannot open boot image %s: %s", image, strerror(errno));
		die();
	}

	load_elf(fd);
	close(fd);

	setstack(argument);
}

