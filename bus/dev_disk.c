#include <sys/types.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "console.h"
#include "clock.h"
#include "main.h"
#include "util.h"

#include "lamebus.h"
#include "busids.h"


const char rcsid_dev_disk_c[] = "$Id: dev_disk.c,v 1.8 2001/01/27 00:41:39 dholland Exp $";

/* Disk underlying I/O definitions */
#define HEADER_MESSAGE  "System/161 Disk Image"
#define HEADERSIZE      SECTSIZE

/* Disk physical parameters */
#define SECTSIZE               512   /* bytes */
#define SECTOR_FUDGE          1.06
#define OUTER_DIAM              80   /* mm */
#define INNER_DIAM              20   /* mm */
#define PLATTER_AREA   (1500 * PI)  /* square mm */
#define PI                 3.14159

/* Disk timing parameters */
#define HEAD_SWITCH_TIME     1000000   /* ns */
#define CACHE_READ_TIME      500       /* ns */

/* Register offsets */
#define DISKREG_NSECT 0
#define DISKREG_STAT  4
#define DISKREG_SECT  8
#define DISKREG_RPM   12

/* Transfer buffer offsets */
#define DISK_BUF_START  32768
#define DISK_BUF_END    (DISK_BUF_START + SECTSIZE)

/* Bits for status registers */
#define DISKBIT_INPROGRESS    1
#define DISKBIT_ISWRITE       2
#define DISKBIT_COMPLETE      4
#define DISKBIT_INVSECT       8
#define DISKBIT_MEDIAERR      16

/* The legal values that can be written to the status register */
#define DISKSTAT_IDLE          0
#define DISKSTAT_READING       (DISKBIT_INPROGRESS)
#define DISKSTAT_WRITING       (DISKBIT_INPROGRESS|DISKBIT_ISWRITE)

/* Masks for the other values for the status register */
#define DISKSTAT_COMPLETE      (DISKBIT_COMPLETE)
#define DISKSTAT_INVSECT       (DISKBIT_COMPLETE|DISKBIT_INVSECT)
#define DISKSTAT_MEDIAERR      (DISKBIT_COMPLETE|DISKBIT_MEDIAERR)

/* Macros for manipulating status registers */
#define FINISH(r,bits)    ((r)=((r) & ~DISKBIT_INPROGRESS)|(bits))
#define COMPLETE(r)       FINISH(r, DISKSTAT_COMPLETE)
#define INVSECT(r)        FINISH(r, DISKSTAT_INVSECT)
#define MEDIAERR(r)       FINISH(r, DISKSTAT_MEDIAERR)

/*
 * Data for holding the device state
 */
struct disk_data {
	/*
	 * Bus info
	 */
	int dd_slot;

	/*
	 * Raw I/O
	 */
	int dd_fd;
	int dd_paranoid;     /* if nonzero, fsync on every write */

	/* 
	 * Geometry:
	 * dd_sectors[] has dd_cylinders entries. 
	 * sum(dd_sectors) * dd_heads should give dd_totsectors.
	 */
	u_int32_t *dd_sectors;
	u_int32_t dd_cylinders;
	u_int32_t dd_heads;
	u_int32_t dd_totsectors;
	u_int32_t dd_rpm;
	u_int32_t dd_nsecs_per_rev;

	/* 
	 * Timing status
	 */
	int dd_current_cyl;
	int dd_current_head;
	u_int32_t dd_trackarrival_secs;
	u_int32_t dd_trackarrival_nsecs;
	int dd_timedop;             /* nonzero if waiting for a timer event */

	/*
	 * Registers
	 */
	u_int32_t dd_stat;
	u_int32_t dd_sect;

	/*
	 * I/O buffer
	 */
	char dd_buf[SECTSIZE];
};

////////////////////////////////////////////////////////////
//
// Raw I/O


static
int
doread(int fd, off_t offset, char *buf, size_t bufsize)
{
	size_t tot=0;
	int r;

	if (lseek(fd, offset, SEEK_SET)) {
		return -1;
	}

	while (tot < bufsize) {
		r = read(fd, buf + tot, bufsize - tot);
		if (r<0 && (errno==EINTR || errno==EAGAIN)) {
			continue;
		}
		if (r<0) {
			return -1;
		}
		if (r==0) {
			/* Unexpected EOF */
			errno = ENXIO;  /* not a very good errno... */
			return -1;
		}
		tot += r;
	}

	return 0;
}

static
int
dowrite(int fd, off_t offset, const char *buf, size_t bufsize, int paranoid)
{
	size_t tot=0;
	int r;

	if (lseek(fd, offset, SEEK_SET)) {
		return -1;
	}

	while (tot < bufsize) {
		r = write(fd, buf + tot, bufsize - tot);
		if (r<0 && (errno==EINTR || errno==EAGAIN)) {
			continue;
		}
		if (r<0) {
			return -1;
		}
		if (r==0) {
			/* ? */
			errno = ENXIO;  /* not a very good errno... */
			return -1;
		}
		tot += r;
	}

	if (paranoid) {
		if (fsync(fd)) {
			return -1;
		}
	}

	return 0;
}

static
void
writeheader(struct disk_data *dd, const char *filename)
{
	off_t fsize;
	char buf[HEADERSIZE];

	memset(buf, 0, HEADERSIZE);
	strcpy(buf, HEADER_MESSAGE);

	if (dowrite(dd->dd_fd, 0, buf, HEADERSIZE, dd->dd_paranoid)) {
		msg("disk: slot %d: %s: Write of header: %s",
		    dd->dd_slot, filename, strerror(errno));
		die();
	}
	
	fsize = dd->dd_totsectors;
	fsize *= SECTSIZE;
	fsize += HEADERSIZE;

	if (ftruncate(dd->dd_fd, fsize)) {
		msg("disk: slot %d: %s: ftruncate: %s",
		    dd->dd_slot, filename, strerror(errno));
		die();
	}
}

static
void
readheader(struct disk_data *dd, const char *filename)
{
	char buf[HEADERSIZE];
	if (doread(dd->dd_fd, 0, buf, HEADERSIZE)) {
		msg("disk: slot %d: %s: Reading header: %s",
		    dd->dd_slot, filename, strerror(errno));
		die();
	}

	/* just in case */
	buf[HEADERSIZE-1] = 0;

	if (strcmp(buf, HEADER_MESSAGE)) {
		msg("disk: slot %d: %s is not a disk image",
		    dd->dd_slot, filename);
		die();
	}
}

static
void
disk_open(struct disk_data *dd, const char *filename)
{
	dd->dd_fd = open(filename, O_RDWR);
	if (dd->dd_fd<0 && errno==ENOENT) {
		dd->dd_fd = open(filename, O_RDWR|O_CREAT|O_EXCL, 0664);
		if (dd->dd_fd<0) {
			msg("disk: slot %d: %s: %s",
			    dd->dd_slot, filename, strerror(errno));
			die();
		}
		writeheader(dd, filename);
		return;
	}
	if (dd->dd_fd<0) {
		msg("disk: slot %d: %s: %s",
		    dd->dd_slot, filename, strerror(errno));
		die();
	}
	readheader(dd, filename);
}

static
void
disk_close(struct disk_data *dd)
{
	if (close(dd->dd_fd)) {
		smoke("disk: slot %d: close: %s", 
		      dd->dd_slot, strerror(errno));
	}
}

static
int
disk_readsector(struct disk_data *dd)
{
	off_t offset = dd->dd_sect;
	offset *= SECTSIZE;
	offset += HEADERSIZE;

	g_stats.s_rsects++;

	return doread(dd->dd_fd, offset, dd->dd_buf, SECTSIZE);
}

static
int
disk_writesector(struct disk_data *dd)
{
	off_t offset = dd->dd_sect;
	offset *= SECTSIZE;
	offset += HEADERSIZE;

	g_stats.s_wsects++;

	return dowrite(dd->dd_fd, offset, dd->dd_buf, SECTSIZE,
		       dd->dd_paranoid);
}

////////////////////////////////////////////////////////////
//
// Geometry modeling

static
int
compute_sectors(struct disk_data *dd)
{
	u_int32_t physsectors;      // total number of actual sectors
	u_int32_t sectorspertrack;  // average sectors per track
	u_int32_t sectorspercyl;    // average sectors per cylinder
	u_int32_t i, tot;

	double sectors_per_area;
	double trackwidth;

	/*
	 * Compute number of physical sectors. We use a bit more than the
	 * requested space so as to leave room for sector remapping. Not
	 * that we actually do sector remapping when computing latencies,
	 * but we could. Note that these spare sectors do not appear in
	 * the file we use for underlying storage.
	 */
	physsectors = (u_int32_t)(dd->dd_totsectors * SECTOR_FUDGE);
	if (physsectors < dd->dd_totsectors) {
		/* Overflow - didn't fit in u_int32_t */
		return -1;
	}

	/*
	 * Now, based on the number of sectors, heuristically write down
	 * the number of heads and the average number of sectors per track.
	 */
	if (physsectors < 2048) {
		dd->dd_heads = 1;
		sectorspertrack = 8;
	}
	else if (physsectors < 64*1024*2) {  /* 64 megs */
		sectorspertrack = (physsectors/2048)*9 - 1;
		dd->dd_heads = 2;
	}
	else if (physsectors < 180*1024*2) { /* 180 megs */
		sectorspertrack = ((physsectors-121953)/4096)*17 - 5;
		dd->dd_heads = 4;
	}
	else {
		sectorspertrack = 800 + (physsectors % 171);
		dd->dd_heads = 6;
	}

	/* average sectors per cylinder */
	sectorspercyl = sectorspertrack * dd->dd_heads;

	/* compute number of cylinders (rounding all fractions up) */
	dd->dd_cylinders = (physsectors + sectorspercyl - 1) / sectorspercyl;

	/* allocate space */
	dd->dd_sectors = domalloc(dd->dd_cylinders*sizeof(dd->dd_sectors[0]));

	/* compute the width of each track */
	trackwidth = ((OUTER_DIAM - INNER_DIAM)/2) / (double)dd->dd_cylinders;

	/* compute the number of sectors per unit area of disk */
	sectors_per_area = physsectors / (dd->dd_heads * PLATTER_AREA);

	/*
	 * Now, figure out how many sectors are on each track.
	 * We do this by computing the area of the track and multiplying
	 * by sectors_per_area, truncating to the next smallest integer.
	 * We reserve one sector on each track.
	 */
	for (i=0; i<dd->dd_cylinders; i++) {
		double inside = INNER_DIAM/2.0 + i*trackwidth;
		double outside = inside + trackwidth;
		/* 
		 * this track's area = pi*(outside^2 - inside^2)
		 *                   = pi*(outside + inside)*(outside - inside)
                 *                   = pi*(outside + inside)*(trackwidth)
		 */
		double trackarea = (outside+inside)*trackwidth*PI;
		double sectors = sectors_per_area * trackarea;
		if (sectors < 2.0) {
			/* Too small */
			return -1;
		}
		dd->dd_sectors[i] = ((int) sectors) - 1;
	}

	/* Now compute the total number of sectors available. */
	tot = 0;
	for (i=0; i<dd->dd_cylinders; i++) {
		tot += dd->dd_sectors[i];
	}
	tot *= dd->dd_heads;

	/* Make sure we've got enough space. */
	if (tot < dd->dd_totsectors) {
		/* 
		 * Shouldn't happen. If it does, increase SECTOR_FUDGE.
		 */
		return -1;
	}

	return 0;
}

static
void
locate_sector(struct disk_data *dd,
	      u_int32_t sector, int *track, int *head, int *rotoffset)
{
	/* 
	 * Assume sector has already been checked for being in bounds.
	 *
	 * Note that we start numbering sectors from the outermost
	 * (fastest) track.
	 */

	u_int32_t start=0;
	u_int32_t k;
	for (k=dd->dd_cylinders; k>0; k--) {
		u_int32_t cyl = k-1;
		u_int32_t end = start + dd->dd_heads*dd->dd_sectors[cyl];
		if (sector >= start && sector < end) {
			*track = cyl;
			sector -= start;
			*head = sector % dd->dd_heads;
			*rotoffset = sector / dd->dd_heads;
			return;
		}
	}

	smoke("Cannot locate sector %u\n", sector);
}

static
u_int32_t
disk_seektime(struct disk_data *dd, int ntracks)
{
	/*
	 * XXX - fix this.
	 */
	return ntracks * 2000000;
}

static
u_int32_t
disk_readrotdelay(struct disk_data *dd, u_int32_t cyl, u_int32_t rotoffset)
{
	u_int32_t nowsecs, nownsecs;

	/*
	 * Time for crossing a single sector.
	 */
	u_int32_t nsecs_per_sector = dd->dd_nsecs_per_rev/dd->dd_sectors[cyl];

	/*
	 * Next sector after the one we want.
	 */
	u_int32_t targsector = (rotoffset+1) % dd->dd_sectors[cyl];

	/*
	 * Compute when the next sector would first be reached after
	 * hitting the track. (When the next sector is reached, the
	 * sector we want is fully read.)
	 *
	 * Note that we require that there are an integral number of
	 * revs per second, and that we assume the platters are always
	 * at position 0 when nownsecs = 0.
	 */
	u_int32_t targsecs = dd->dd_trackarrival_secs;
	u_int32_t targnsecs = targsector * nsecs_per_sector;
	while (targnsecs < dd->dd_trackarrival_nsecs) {
		targnsecs += dd->dd_nsecs_per_rev;
	}
	while (targnsecs >= 1000000000) {
		targnsecs -= 1000000000;
		targsecs++;
	}

	clock_time(&nowsecs, &nownsecs);

	/*
	 * If we've reached that time, we've already crossed the
	 * sector and it's in our track buffer.
	 */
	if (targsecs < nowsecs ||
	    (targsecs == nowsecs && targnsecs <= nowsecs)) {
		return CACHE_READ_TIME;
	}

	/*
	 * Otherwise, we need to wait until that time.
	 */
	targsecs -= nowsecs;
	targnsecs -= nownsecs;
	targnsecs += 1000000000*targsecs;  // should not overflow

	return targnsecs;
}

static
u_int32_t
disk_writerotdelay(struct disk_data *dd, u_int32_t cyl, u_int32_t rotoffset)
{
	u_int32_t nowsecs, nownsecs;

	/*
	 * Time for crossing a single sector.
	 */
	u_int32_t nsecs_per_sector = dd->dd_nsecs_per_rev/dd->dd_sectors[cyl];

	/*
	 * Compute when the sector we want will next be reached.
	 * (Ignore seconds. The disk must be at least 60 rpm, so we
	 * can get to any sector without overflowing a u_int32_t of
	 * nsecs.)
	 */
	u_int32_t targnsecs = rotoffset * nsecs_per_sector;

	clock_time(&nowsecs, &nownsecs);

	while (targnsecs < nownsecs) {
		targnsecs += dd->dd_nsecs_per_rev;
	}

	/*
	 * Wait until that time plus how long it takes to do the write.
	 */

	return (targnsecs - nownsecs) + nsecs_per_sector;
}

////////////////////////////////////////////////////////////
//
// Setup


static
void *
disk_init(int slot, int argc, char *argv[])
{
	struct disk_data *dd = domalloc(sizeof(struct disk_data));
	const char *filename = NULL;
	u_int32_t totsectors=0;
	u_int32_t rpm = 3600;
	int i, paranoid=0;

	for (i=1; i<argc; i++) {
		if (!strncmp(argv[i], "rpm=", 4)) {
			rpm = atoi(argv[i]+4);
		}
		else if (!strncmp(argv[i], "sectors=", 8)) {
			totsectors = atoi(argv[i]+8);
		}
		else if (!strncmp(argv[i], "file=", 5)) {
			filename = argv[i]+5;
		}
		else if (!strcmp(argv[i], "paranoid")) {
			paranoid = 1;
		}
		else {
			msg("disk: slot %d: invalid option %s", slot, argv[i]);
			die();
		}
	}

	if (totsectors < 128) {
		msg("disk: slot %d: Too small", slot);
		die();
	}

	dd->dd_slot = slot;
	dd->dd_totsectors = totsectors;

	/* set dd_cylinders, dd_sectors, dd_heads */
	if (compute_sectors(dd)) {
		msg("disk: slot %d: Geometry initialization failed "
		    "(try another size)", slot);
		die();
	}

	if (dd->dd_heads < 1 || dd->dd_heads > 16) {
		msg("disk: slot %d: Computed geometry has invalid "
		    "number of heads (%d)", slot, dd->dd_heads);
		die();
	}

	if (rpm < 60) {
		msg("disk: slot %d: RPM too low (%d)", slot, rpm);
		die();
	}
	if (rpm % 60) {
		msg("disk: slot %d: RPM %d not a multiple of 60", slot, rpm);
		die();
	}

	dd->dd_rpm = rpm;
	dd->dd_nsecs_per_rev = 1000000000 / (dd->dd_rpm / 60);

	dd->dd_current_cyl = 0;
	dd->dd_current_head = 0;
	dd->dd_trackarrival_secs = 0;
	dd->dd_trackarrival_nsecs = 0;

	dd->dd_stat = DISKSTAT_IDLE;
	dd->dd_sect = 0;

	dd->dd_paranoid = paranoid;

	if (filename==NULL) {
		msg("disk: slot %d: No filename specified", slot);
		die();
	}

	disk_open(dd, filename);

	return dd;
}

static
void
disk_cleanup(void *data)
{
	struct disk_data *dd = data;
	disk_close(dd);
	free(dd);
}

////////////////////////////////////////////////////////////
//
// Operations

static void disk_update(struct disk_data *dd);

static
void
disk_seekdone(void *data, u_int32_t cyl)
{
	struct disk_data *dd = data;

	dd->dd_current_cyl = cyl;
	clock_time(&dd->dd_trackarrival_secs, &dd->dd_trackarrival_nsecs);

	dd->dd_timedop = 0;
	disk_update(dd);
}

static
void
disk_headswdone(void *data, u_int32_t head)
{
	struct disk_data *dd = data;

	dd->dd_current_head = head;
	clock_time(&dd->dd_trackarrival_secs, &dd->dd_trackarrival_nsecs);

	dd->dd_timedop = 0;
	disk_update(dd);
}

static
void
disk_rotdelaydone(void *data, u_int32_t nothing)
{
	struct disk_data *dd = data;
	(void)nothing;

	dd->dd_timedop = 0;
	disk_update(dd);
}

static
void
disk_work(struct disk_data *dd)
{
	int cyl, head, rotoffset;
	u_int32_t rotdelay;
	int err;

	if (dd->dd_timedop) {
		/*
		 * Something's presently happening. Nothing more happens until
		 * it finishes.
		 */
		return;
	}

	if ((dd->dd_stat & DISKBIT_INPROGRESS)==0) {
		/*
		 * Nothing to do.
		 */
		return;
	}

	if (dd->dd_sect >= dd->dd_totsectors) {
		INVSECT(dd->dd_stat);
		return;
	}

	locate_sector(dd, dd->dd_sect, &cyl, &head, &rotoffset);

	if (dd->dd_current_cyl != cyl) {
		/*
		 * Need to seek.
		 */
		u_int32_t nsecs;
		int distance;

		distance = cyl - dd->dd_current_cyl;
		if (distance<0) {
			distance = -distance;
		}
		
		nsecs = disk_seektime(dd, distance);
		
		dd->dd_timedop = 1;
		schedule_event(nsecs, dd, cyl, disk_seekdone);
		return;
	}

	if (dd->dd_current_head != head) {
		dd->dd_timedop = 1;
		schedule_event(HEAD_SWITCH_TIME, dd, head, disk_headswdone);
		return;
	}

	if (dd->dd_stat & DISKBIT_ISWRITE) {
		rotdelay = disk_writerotdelay(dd, cyl, rotoffset);
	}
	else {
		rotdelay = disk_readrotdelay(dd, cyl, rotoffset);
	}

	if (rotdelay > 0) {
		dd->dd_timedop = 1;
		schedule_event(rotdelay, dd, 0, disk_rotdelaydone);
		return;
	}

	/*
	 * We're here.
	 */
	if (dd->dd_stat & DISKBIT_ISWRITE) {
		err = disk_writesector(dd);
	}
	else {
		err = disk_readsector(dd);
	}

	if (err) {
		MEDIAERR(dd->dd_stat);
	}
	else {
		COMPLETE(dd->dd_stat);
	}
}


static
void
disk_update(struct disk_data *dd)
{
	disk_work(dd);

	if (dd->dd_stat & DISKBIT_COMPLETE) {
		RAISE_IRQ(dd->dd_slot);
	}
	else {
		LOWER_IRQ(dd->dd_slot);
	}
}



static
void
disk_setstatus(struct disk_data *dd, u_int32_t val)
{
	switch (val) {
	    case DISKSTAT_IDLE:
	    case DISKSTAT_READING:
	    case DISKSTAT_WRITING:
		break;
	    default:
		hang("disk: Invalid write %u to status register", val);
	}

	dd->dd_stat = val;

	disk_update(dd);
}

static
int
disk_fetch(void *data, u_int32_t offset, u_int32_t *ret)
{
	struct disk_data *dd = data;
	u_int32_t *ptr;

	if (offset >= DISK_BUF_START && offset < DISK_BUF_END) {
		offset -= DISK_BUF_START;
		ptr = (u_int32_t *)(dd->dd_buf + offset);
		*ret = *ptr;
		return 0;
	}

	switch (offset) {
	    case DISKREG_NSECT: *ret = dd->dd_totsectors; return 0;
	    case DISKREG_RPM: *ret = dd->dd_rpm; return 0;
	    case DISKREG_STAT: *ret = dd->dd_stat; return 0;
	    case DISKREG_SECT: *ret = dd->dd_sect; return 0;
	}
	return -1;
}

static
int
disk_store(void *data, u_int32_t offset, u_int32_t val)
{
	struct disk_data *dd = data;
	u_int32_t *ptr;

	if (offset >= DISK_BUF_START && offset < DISK_BUF_END) {
		offset -= DISK_BUF_START;
		ptr = (u_int32_t *)(dd->dd_buf + offset);
		*ptr = val;
		return 0;
	}

	switch (offset) {
	    case DISKREG_STAT: disk_setstatus(dd, val); break;
	    case DISKREG_SECT: dd->dd_sect = val; return 0;
	}

	return -1;
}

struct lamebus_device_info disk_device_info = {
	LBVEND_CS161,
	LBVEND_CS161_DISK,
	DISK_REVISION,
	disk_init,
	disk_fetch,
	disk_store,
	disk_cleanup,
};
