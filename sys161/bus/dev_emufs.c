/*
 * Emulator filesystem access.
 *
 * Registers (mapped starting at offset 4096, 32-bit access only):
 *    4 bytes: RFH  file handle number
 *    4 bytes: ROFF seek address
 *    4 bytes: RLEN length
 *    4 bytes: ROP  operation code (write triggers operation)
 *    4 bytes: RRES result register (0=nothing, 1=complete, 2+=error)
 *
 * 16k I/O buffer IOB is mapped at offset 32768.
 * Handle 0 is the "root" directory.
 *
 * Operations are:
 *   OPEN/CREATE/EXCLCREATE
 *           RFH:  handle for dir path is relative to
 *           RLEN: length of path
 *           ROP:  1/2/3 respectively
 *           IOB:  pathname
 *
 *           The named file is located and opened.
 *
 *           RFH:  on success, new handle for named file
 *           RLEN: on success, 0 for file, 1 for directory
 *           RRES: result code
 *
 *   CLOSE
 *           RFH:  handle
 *           ROFF: -
 *           RLEN: -
 *           ROP:  4
 *           RRES: -
 *           IOB:  -
 *
 *           On success, the selected handle is flushed and may be
 *           reused for further opens.
 *
 *           RRES: result code
 *
 *   READ
 *           RFH:  handle
 *           ROFF: file position to read at
 *           RLEN: maximum length to read
 *           ROP:  5
 *
 *           Data is read. 
 *
 *           ROFF: updated
 *           RLEN: length of read performed
 *           RRES: result code
 *           IOB:  contains data
 *
 *   READDIR
 *           RFH:  handle
 *           ROFF: file position to read at
 *           RLEN: maximum length to read
 *           ROP:  6
 *
 *           One filename is read from a directory.
 *
 *           ROFF: updated
 *           RLEN: length of read performed
 *           RRES: result code
 *           IOB:  contains data (one filename)
 *
 *   WRITE
 *           RFH:  handle
 *           ROFF: file position to write at
 *           RLEN: length to write
 *           ROP:  7
 *           IOB:  contains data
 *
 *           Data is written. 
 *
 *           ROFF: updated
 *           RLEN: length of write performed
 *           RRES: result code
 *
 *   GETSIZE
 *           RFH:  handle
 *           ROP:  8
 *
 *           The size of the file or directory is fetched.
 *
 *           RLEN: length of file
 *           RRES: result code
 *
 *   TRUNCATE
 *           RFH:  handle
 *           RLEN: new size for file
 *           ROP:  9
 *
 *           The file is truncated to the requested length.
 *
 *           RRES: result code
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include "config.h"

#include "util.h"
#include "bswap.h"
#include "console.h"
#include "speed.h"
#include "clock.h"
#include "main.h"

#include "lamebus.h"
#include "busids.h"


#define MAXHANDLES     64
#define EMU_ROOTHANDLE  0

#define EMU_BUF_START  32768
#define EMU_BUF_SIZE   16384
#define EMU_BUF_END    (EMU_BUF_START + EMU_BUF_SIZE)

#define EMUREG_HANDLE  0
#define EMUREG_OFFSET  4
#define EMUREG_IOLEN   8
#define EMUREG_OPER    12
#define EMUREG_RESULT  16

#define EMU_OP_OPEN          1
#define EMU_OP_CREATE        2
#define EMU_OP_EXCLCREATE    3
#define EMU_OP_CLOSE         4
#define EMU_OP_READ          5
#define EMU_OP_READDIR       6
#define EMU_OP_WRITE         7
#define EMU_OP_GETSIZE       8
#define EMU_OP_TRUNC         9

#define EMU_RES_SUCCESS      1
#define EMU_RES_BADHANDLE    2
#define EMU_RES_BADOP        3
#define EMU_RES_BADPATH      4
#define EMU_RES_BADSIZE      5
#define EMU_RES_EXISTS       6
#define EMU_RES_ISDIR        7
#define EMU_RES_MEDIA        8
#define EMU_RES_NOHANDLES    9
#define EMU_RES_NOSPACE      10
#define EMU_RES_NOTDIR       11
#define EMU_RES_UNKNOWN      12
#define EMU_RES_UNSUPP       13

struct emufs_handleinfo {
	int eh_fd;
	dev_t eh_dev;
	ino_t eh_ino;
};

struct emufs_data {
	int ed_slot;

	/* This used to be ed_buf[EMU_BUF_SIZE]; see dev_disk.c */
	char *ed_buf;
	uint32_t ed_handle;		/* file handle register */
	uint32_t ed_offset;		/* offset register */
	uint32_t ed_iolen;		/* iolen register */
	uint32_t ed_result;		/* result register */

	/* Handles from ed_handle are indexes into here */
	struct emufs_handleinfo ed_handles[MAXHANDLES];

	/* Timing stuff */
	int ed_busy;			/* true if operation in progress */
	uint32_t ed_busyresult;		/* result for ed_result when done */
};

static
int
pushdir(int fd, int h)
{
	int oldfd;

	oldfd = open(".", O_RDONLY);
	if (oldfd<0) {
		smoke(".: %s", strerror(errno));
	}

	if (fchdir(fd)) {
		smoke("emufs: fchdir [handle %d, fd %d]: %s", h, fd, 
		      strerror(errno));
	}

	return oldfd;
}

static
void
popdir(int oldfd)
{
	if (fchdir(oldfd)) {
		smoke("emufs: fchdir [back]: %s", strerror(errno));
	}
	close(oldfd);
}


static
void
emufs_setresult(struct emufs_data *ed, uint32_t result)
{
	ed->ed_result = result;
	if (ed->ed_result>0) {
		raise_irq(ed->ed_slot);
	}
	else {
		lower_irq(ed->ed_slot);
	}
}

static
uint32_t
errno_to_code(int err)
{
	switch (err) {
	    case 0: return EMU_RES_SUCCESS;
	    case EBADF: return EMU_RES_BADHANDLE;
	    case EINVAL: return EMU_RES_BADSIZE;
	    case ENOENT: return EMU_RES_BADPATH;
	    case EIO: return EMU_RES_MEDIA;
	    case ENOTDIR: return EMU_RES_NOTDIR;
	    case EISDIR: return EMU_RES_ISDIR;
	    case EEXIST: return EMU_RES_EXISTS;
	    case ENOSPC: return EMU_RES_NOSPACE;
	}
	return EMU_RES_UNKNOWN;
}

/*
 * Find an existing handle for this file (if any); otherwise allocate
 * a new one. XXX: this table should really use some kind of lookup
 * scheme and not linear search, although since MAXHANDLES is
 * currently only 64 it doesn't matter that much.
 */
static
int
pickhandle(struct emufs_data *ed, dev_t dev, ino_t ino)
{
	int i;
	int empty = -1;

	for (i=0; i<MAXHANDLES; i++) {
		if (ed->ed_handles[i].eh_fd < 0) {
			/* remember first unused handle */
			if (empty < 0) {
				empty = i;
			}
		}
		else if (ed->ed_handles[i].eh_dev == dev && 
			 ed->ed_handles[i].eh_ino == ino) {
			return i;
		}
	}
	ed->ed_handles[empty].eh_dev = dev;
	ed->ed_handles[empty].eh_ino = ino;
	return empty;
}

static
void
emufs_openfirst(struct emufs_data *ed, const char *dir)
{
	struct stat sbuf;
	int fd;

	Assert(ed->ed_handles[EMU_ROOTHANDLE].eh_fd < 0);

	fd = open(dir, O_RDONLY);
	if (fd<0) {
		msg("emufs: slot %d: %s: %s", ed->ed_slot, dir, 
		    strerror(errno));
		die();
	}
	if (fstat(fd, &sbuf) < 0) {
		msg("emufs: slot %d: %s: fstat: %s", ed->ed_slot, dir,
		    strerror(errno));
		die();
	}

	ed->ed_handles[EMU_ROOTHANDLE].eh_fd = fd;
	ed->ed_handles[EMU_ROOTHANDLE].eh_dev = sbuf.st_dev;
	ed->ed_handles[EMU_ROOTHANDLE].eh_ino = sbuf.st_ino;

	g_stats.s_memu++;
}

static
unsigned
emufs_open_and_stat(struct emufs_data *ed, int flags, struct stat *sbuf,
		    int *fd_ret)
{
	int fd, err;

	fd = open(ed->ed_buf, flags, 0664);
	if (fd < 0) {
		err = errno;
		HWTRACE(DOTRACE_EMUFS, "%s", strerror(err));
		return errno_to_code(err);
	}
	if (fstat(fd, sbuf) < 0) {
		err = errno;
		close(fd);
		HWTRACE(DOTRACE_EMUFS, "fstat: %s", strerror(err));
		return errno_to_code(err);
	}

	*fd_ret = fd;
	return EMU_RES_SUCCESS;
}

static
unsigned
emufs_open_create(struct emufs_data *ed, int flags, int *handle_ret)
{
	struct stat sbuf;
	unsigned status;
	int handle, fd = -1;

	status = emufs_open_and_stat(ed, flags, &sbuf, &fd);
	if (status != EMU_RES_SUCCESS) {
		return status;
	}

	handle = pickhandle(ed, sbuf.st_dev, sbuf.st_ino);
	if (handle < 0) {
		close(fd);
		HWTRACE(DOTRACE_EMUFS, "out of handles");
		return EMU_RES_NOHANDLES;
	}

	/*
	 * If we created a new file and got back a file we already
	 * have open, it means someone renamed the file under us
	 * between the first stat call and the open. (It can't mean
	 * that the old file was deleted and the inode number was
	 * recycled, because we still have it open.)
	 *
	 * This can't happen if O_EXCL was used (unless the fs we're
	 * running on is borked), but O_EXCL wasn't necessarily used;
	 * so if this happens just reuse the existing handle for the
	 * file and close the new fd.
	 */
	if (ed->ed_handles[handle].eh_fd >= 0) {
		close(fd);
	}
	else {
		ed->ed_handles[handle].eh_fd = fd;
	}
	*handle_ret = handle;
	return EMU_RES_SUCCESS;
}

static
unsigned
emufs_open_existing(struct emufs_data *ed, int flags,
		    dev_t expected_dev, ino_t expected_ino,
		    int *handle_ret)
{
	struct stat sbuf;
	unsigned status;
	int handle, fd = -1;

	while (1) {
		/*
		 * We might already have this file open, so look for
		 * it first.
		 */
		handle = pickhandle(ed, expected_dev, expected_ino);
		if (handle < 0) {
			HWTRACE(DOTRACE_EMUFS, "out of handles");
			return EMU_RES_NOHANDLES;
		}

		/* If so, just return it. */
		if (ed->ed_handles[handle].eh_fd >= 0) {
			*handle_ret = handle;
			return EMU_RES_SUCCESS;
		}

		status = emufs_open_and_stat(ed, flags, &sbuf, &fd);
		if (status != EMU_RES_SUCCESS) {
			return status;
		}

		/*
		 * If we didn't get the same object we scoped out,
		 * close and retry. This avoids allowing someone
		 * manipulating symlinks under us to cause us to open
		 * a second handle for the same file; that is mostly
		 * harmless, but for the root dir it isn't, especially
		 * when we finally get code to prohibit going outside
		 * the root dir.
		 *
		 * Note that if in the future we care about whether we
		 * followed a link and what link it is, we need to
		 * retry the initial stat as well as the open.
		 */
		if (sbuf.st_dev == expected_dev &&
		    sbuf.st_ino == expected_ino) {
			break;
		}
		close(fd);

		expected_dev = sbuf.st_dev;
		expected_ino = sbuf.st_ino;
	}

	ed->ed_handles[handle].eh_fd = fd;
	*handle_ret = handle;
	return EMU_RES_SUCCESS;
}


static
uint32_t
emufs_open(struct emufs_data *ed, int flags)
{
	int handle = -1;
	int curdir;
	struct stat sbuf;
	unsigned status;
	int isdir;

	if (ed->ed_iolen >= EMU_BUF_SIZE) {
		return EMU_RES_BADSIZE;
	}

	/* ensure null termination */
	ed->ed_buf[ed->ed_iolen] = 0;

	HWTRACEL(DOTRACE_EMUFS, "emufs: slot %d: open %s: ", ed->ed_slot,
			       ed->ed_buf);

	curdir = pushdir(ed->ed_handles[ed->ed_handle].eh_fd, ed->ed_handle);

	if (stat(ed->ed_buf, &sbuf)) {
		if (flags==0) {
			/* not creating; doesn't exist -> fail */
			int err = errno;
			HWTRACE(DOTRACE_EMUFS, "%s", strerror(err));
			popdir(curdir);
			return errno_to_code(err);
		}
		/* creating; ok if it doesn't exist, and it's not a dir */
		flags |= O_RDWR;
		isdir = 0;

		status = emufs_open_create(ed, flags, &handle);
	}
	else {
		isdir = S_ISDIR(sbuf.st_mode)!=0;
		if (isdir && flags==0) {
			flags |= O_RDONLY;
		}
		else {
			flags |= O_RDWR;
		}
		status = emufs_open_existing(ed, flags,
					     sbuf.st_dev, sbuf.st_ino,
					     &handle);
	}

	if (status != EMU_RES_SUCCESS) {
		popdir(curdir);
		return status;
	}
	Assert(handle >= 0);

	popdir(curdir);

	ed->ed_handle = handle;
	ed->ed_iolen = isdir;

	HWTRACE(DOTRACE_EMUFS, "succeeded, handle %d%s", handle,
		isdir ? " (directory)" : "");
	g_stats.s_memu++;

	return EMU_RES_SUCCESS;
}

static
uint32_t
emufs_close(struct emufs_data *ed)
{
	close(ed->ed_handles[ed->ed_handle].eh_fd);
	ed->ed_handles[ed->ed_handle].eh_fd = -1;
	HWTRACE(DOTRACE_EMUFS, "emufs: slot %d: close handle %d",
		ed->ed_slot, ed->ed_handle);
	g_stats.s_memu++;
	return EMU_RES_SUCCESS;
}

static
uint32_t
emufs_read(struct emufs_data *ed)
{
	int len;
	int fd;

	if (ed->ed_iolen > EMU_BUF_SIZE) {
		return EMU_RES_BADSIZE;
	}

	HWTRACEL(DOTRACE_EMUFS, "emufs: slot %d: read %u bytes, handle %d: ",
		 ed->ed_slot, ed->ed_iolen, ed->ed_handle);

	fd = ed->ed_handles[ed->ed_handle].eh_fd;

	lseek(fd, ed->ed_offset, SEEK_SET);
	len = read(fd, ed->ed_buf, ed->ed_iolen);

	if (len < 0) {
		int err = errno;
		HWTRACE(DOTRACE_EMUFS, "%s", strerror(err));
		return errno_to_code(err);
	}

	ed->ed_offset += len;
	ed->ed_iolen = len;

	HWTRACE(DOTRACE_EMUFS, "success");
	g_stats.s_remu++;

	return EMU_RES_SUCCESS;
}

static
uint32_t
emufs_readdir(struct emufs_data *ed)
{
	struct dirent *dp;
	DIR *d;

	uint32_t ct, len;
	int herefd, fd;

	if (ed->ed_iolen > EMU_BUF_SIZE) {
		return EMU_RES_BADSIZE;
	}

	HWTRACEL(DOTRACE_EMUFS,
		 "emufs: slot %d: readdir %u bytes, handle %d: ",
		 ed->ed_slot, ed->ed_iolen, ed->ed_handle);

	herefd = open(".", O_RDONLY);
	if (herefd<0) {
		int err = errno;
		HWTRACE(DOTRACE_EMUFS, "%s", strerror(err));
		return errno_to_code(err);
	}

	fd = ed->ed_handles[ed->ed_handle].eh_fd;

	if (fchdir(fd)<0) {
		int err = errno;
		HWTRACE(DOTRACE_EMUFS, "%s", strerror(err));
		close(herefd);
		return errno_to_code(err);
	}

	d = opendir(".");
	if (d==NULL) {
		int err = errno;
		HWTRACE(DOTRACE_EMUFS, "%s", strerror(err));
		fchdir(herefd);
		close(herefd);
		return errno_to_code(err);
	}

	dp = NULL;
	for (ct = 0; ct <= ed->ed_offset; ct++) {
		dp = readdir(d);
		if (dp == NULL) {
			break;
		}
	}
	if (dp != NULL) {
		HWTRACE(DOTRACE_EMUFS, "got %s", dp->d_name);
		len = strlen(dp->d_name);
		if (len > ed->ed_iolen) {
			len = ed->ed_iolen;
		}
		memcpy(ed->ed_buf, dp->d_name, len);
		ed->ed_iolen = len;
		ed->ed_offset++;
		g_stats.s_remu++;
	}
	else {
		HWTRACE(DOTRACE_EMUFS, "EOF");
		ed->ed_iolen = 0;
	}

	closedir(d);
	fchdir(herefd);
	close(herefd);

	return EMU_RES_SUCCESS;
#if 0	
	/*
	 * without fchdir, can't do it - there's no fdopendir() or equivalent.
	 */
	(void) ed;
	HWTRACE(DOTRACE_EMUFS, "emufs: slot %d: readdir unsupported",
		ed->ed_slot);

	return EMU_RES_UNSUPP;
#endif
}

static
uint32_t
emufs_write(struct emufs_data *ed)
{
	int len;
	int fd;

	if (ed->ed_iolen > EMU_BUF_SIZE) {
		return EMU_RES_BADSIZE;
	}

	HWTRACEL(DOTRACE_EMUFS, "emufs: slot %d: write %u bytes, handle %d: ",
		 ed->ed_slot, ed->ed_iolen, ed->ed_handle);

	fd = ed->ed_handles[ed->ed_handle].eh_fd;

	lseek(fd, ed->ed_offset, SEEK_SET);
	len = write(fd, ed->ed_buf, ed->ed_iolen);

	if (len < 0) {
		int err = errno;
		HWTRACE(DOTRACE_EMUFS, "%s", strerror(err));
		return errno_to_code(err);
	}

	ed->ed_offset += len;
	ed->ed_iolen = len;

	HWTRACE(DOTRACE_EMUFS, "success");
	g_stats.s_wemu++;

	return EMU_RES_SUCCESS;
}

static
uint32_t
emufs_getsize(struct emufs_data *ed)
{
	struct stat sb;
	int fd;

	HWTRACEL(DOTRACE_EMUFS, "emufs: slot %d: handle %d length: ",
		 ed->ed_slot, ed->ed_handle);

	fd = ed->ed_handles[ed->ed_handle].eh_fd;
	if (fstat(fd, &sb)) {
		int err = errno;
		HWTRACE(DOTRACE_EMUFS, "%s", strerror(err));
		return errno_to_code(err);
	}

	ed->ed_iolen = sb.st_size;

	HWTRACE(DOTRACE_EMUFS, "%u", ed->ed_iolen);
	g_stats.s_memu++;

	return EMU_RES_SUCCESS;
}

static
uint32_t
emufs_trunc(struct emufs_data *ed)
{
	int fd;

	HWTRACEL(DOTRACE_EMUFS, "emufs: slot %d: truncate handle %d to %u: ",
		 ed->ed_slot, ed->ed_handle, ed->ed_iolen);

	fd = ed->ed_handles[ed->ed_handle].eh_fd;
	if (ftruncate(fd, ed->ed_iolen)) {
		int err = errno;
		HWTRACE(DOTRACE_EMUFS, "%s", strerror(err));
		return errno_to_code(err);
	}

	HWTRACE(DOTRACE_EMUFS, "success");
	g_stats.s_wemu++;

	return EMU_RES_SUCCESS;
}

static
uint32_t
emufs_op(struct emufs_data *ed, uint32_t op)
{
	switch (op) {
	    case EMU_OP_OPEN:       return emufs_open(ed, 0);
	    case EMU_OP_CREATE:     return emufs_open(ed, O_CREAT);
	    case EMU_OP_EXCLCREATE: return emufs_open(ed, O_CREAT|O_EXCL);
	    default: break;
	}

	if (ed->ed_handle >= MAXHANDLES ||
	    ed->ed_handles[ed->ed_handle].eh_fd < 0) {
		return EMU_RES_BADHANDLE;
	}

	switch (op) {
	    case EMU_OP_OPEN:
	    case EMU_OP_CREATE:
	    case EMU_OP_EXCLCREATE:
		/* ? */
		break;
	    case EMU_OP_CLOSE:      return emufs_close(ed);
	    case EMU_OP_READ:       return emufs_read(ed);
	    case EMU_OP_READDIR:    return emufs_readdir(ed);
	    case EMU_OP_WRITE:      return emufs_write(ed);
	    case EMU_OP_GETSIZE:    return emufs_getsize(ed);
	    case EMU_OP_TRUNC:      return emufs_trunc(ed);
	}

	return EMU_RES_BADOP;
}

static
void
emufs_done(void *d, uint32_t gen)
{
	struct emufs_data *ed = d;
	(void)gen;

	if (ed->ed_busy != 1) {
		smoke("Spurious call of emufs_done");
	}
	emufs_setresult(ed, ed->ed_busyresult);
	ed->ed_busy = 0;
	ed->ed_busyresult = 0;
	HWTRACE(DOTRACE_EMUFS, "emufs: slot %d: Operation complete", 
		ed->ed_slot);
}

static
void
emufs_do_op(struct emufs_data *ed, uint32_t op)
{
	uint32_t res;

	if (ed->ed_busy != 0) {
		hang("emufs operation started while an operation "
		     "was already in progress");
		return;
	}

	res = emufs_op(ed, op);

	ed->ed_busy = 1;
	ed->ed_busyresult = res;

	schedule_event(EMUFS_NSECS, ed, 0, emufs_done, "emufs");
}

static
void *
emufs_init(int slot, int argc, char *argv[])
{
	struct emufs_data *ed = domalloc(sizeof(struct emufs_data));
	const char *dir = ".";
	int i;

	for (i=1; i<argc; i++) {
		if (!strncmp(argv[i], "dir=", 4)) {
			dir = argv[i]+4;
		}
		else {
			msg("emufs: slot %d: invalid option %s",slot, argv[i]);
			die();
		}
	}

	ed->ed_slot = slot;
	ed->ed_buf = domalloc(EMU_BUF_SIZE);
	memset(ed->ed_buf, 0, EMU_BUF_SIZE);
	ed->ed_handle = 0;
	ed->ed_offset = 0;
	ed->ed_iolen = 0;
	ed->ed_result = 0;

	for (i=0; i<MAXHANDLES; i++) {
		ed->ed_handles[i].eh_fd = -1;
		ed->ed_handles[i].eh_dev = 0;
		ed->ed_handles[i].eh_ino = 0;
	}

	ed->ed_busy = 0;
	ed->ed_busyresult = 0;

	emufs_openfirst(ed, dir);

	return ed;
}

static
int
emufs_fetch(unsigned cpunum, void *data, uint32_t offset, uint32_t *ret)
{
	struct emufs_data *ed = data;
	uint32_t *ptr;

	(void)cpunum;

	if (offset >= EMU_BUF_START && offset < EMU_BUF_END) {
		offset -= EMU_BUF_START;
		ptr = (uint32_t *)(ed->ed_buf + offset);
		*ret = ctoh32(*ptr);
		return 0;
	}

	switch (offset) {
	    case EMUREG_HANDLE: *ret = ed->ed_handle; return 0;
	    case EMUREG_OFFSET: *ret = ed->ed_offset; return 0;
	    case EMUREG_IOLEN: *ret = ed->ed_iolen; return 0;
	    case EMUREG_OPER: *ret = 0; return 0;
	    case EMUREG_RESULT: *ret = ed->ed_result; return 0;
	}
	return -1;
}

static
int
emufs_store(unsigned cpunum, void *data, uint32_t offset, uint32_t val)
{
	struct emufs_data *ed = data;
	uint32_t *ptr;

	(void)cpunum;

	if (offset >= EMU_BUF_START && offset < EMU_BUF_END) {
		offset -= EMU_BUF_START;
		ptr = (uint32_t *)(ed->ed_buf + offset);
		*ptr = htoc32(val);
		return 0;
	}

	switch (offset) {
	    case EMUREG_HANDLE: ed->ed_handle = val; return 0;
	    case EMUREG_OFFSET: ed->ed_offset = val; return 0;
	    case EMUREG_IOLEN: ed->ed_iolen = val; return 0;
	    case EMUREG_OPER: emufs_do_op(ed, val); return 0;
	    case EMUREG_RESULT: emufs_setresult(ed, val); return 0;
	}
	return -1;
}

static
void
emufs_dumpstate(void *data)
{
	struct emufs_data *ed = data;
	msg("System/161 emufs rev %d", EMUFS_REVISION);
	msg("    Registers: handle %lu  result %lu"
	    "    offset %lu (0x%lx)  iolen %lu (0x%lx)",
	    (unsigned long) ed->ed_handle,
	    (unsigned long) ed->ed_result,
	    (unsigned long) ed->ed_offset,
	    (unsigned long) ed->ed_offset,
	    (unsigned long) ed->ed_iolen,
	    (unsigned long) ed->ed_iolen);
	if (ed->ed_busy) {
		msg("    Presently working; result will be %lu",
		    (unsigned long) ed->ed_busyresult);
	}
	else {
		msg("    Presently idle");
	}
	msg("    Buffer:");
	dohexdump(ed->ed_buf, EMU_BUF_SIZE);
}

static
void
emufs_cleanup(void *data)
{
	struct emufs_data *ed = data;
	emufs_close(ed);
	free(ed->ed_buf);
	free(ed);
}

const struct lamebus_device_info emufs_device_info = {
	LBVEND_SYS161,
	LBVEND_SYS161_EMUFS,
	EMUFS_REVISION,
	emufs_init,
	emufs_fetch,
	emufs_store,
	emufs_dumpstate,
	emufs_cleanup,
};
