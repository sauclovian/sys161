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
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "util.h"
#include "console.h"

#include "lamebus.h"
#include "busids.h"


const char rcsid_dev_emufs_c[] = "$Id: dev_emufs.c,v 1.5 2001/01/25 04:49:45 dholland Exp $";


#define MAXHANDLES     64

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


struct emufs_data {
	int ed_slot;

	char ed_buf[EMU_BUF_SIZE];
	u_int32_t ed_handle;
	u_int32_t ed_offset;
	u_int32_t ed_iolen;
	u_int32_t ed_result;

	/* Handles from ed_handle are indexes into here */
	int ed_fds[MAXHANDLES];
};

static
int
pushdir(int fd, int h)
{
	int oldfd;

	oldfd = open(".", O_RDWR);
	if (oldfd<0) {
		smoke(".: %s", strerror(errno));
	}

	if (fchdir(fd)) {
		smoke("emufs: fchdir [handle %d]: %s", h, strerror(errno));
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
emufs_setresult(struct emufs_data *ed, u_int32_t result)
{
	ed->ed_result = result;
	if (ed->ed_result>0) {
		RAISE_IRQ(ed->ed_slot);
	}
	else {
		LOWER_IRQ(ed->ed_slot);
	}
}

static
u_int32_t
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

static
int
pickhandle(struct emufs_data *ed)
{
	int i;
	for (i=0; i<MAXHANDLES; i++) {
		if (ed->ed_fds[i]<0) {
			return i;
		}
	}
	return -1;
}

static
u_int32_t
emufs_open(struct emufs_data *ed, int flags)
{
	int handle;
	int curdir;
	struct stat sbuf;

	if (ed->ed_iolen >= EMU_BUF_SIZE) {
		return EMU_RES_BADSIZE;
	}

	/* ensure null termination */
	ed->ed_buf[ed->ed_iolen] = 0;

	handle = pickhandle(ed);
	if (handle < 0) {
		return EMU_RES_NOHANDLES;
	}

	curdir = pushdir(ed->ed_fds[ed->ed_handle], ed->ed_handle);

	if (stat(ed->ed_buf, &sbuf)) {
		popdir(curdir);
		return errno_to_code(errno);
	}

	if (S_ISDIR(sbuf.st_mode) && flags==0) {
		flags |= O_RDONLY;
	}
	else {
		flags |= O_RDWR;
	}
	
	ed->ed_fds[handle] = open(ed->ed_buf, flags, 0664);
	if (ed->ed_fds[handle]<0) {
		popdir(curdir);
		return errno_to_code(errno);
	}

	popdir(curdir);

	ed->ed_handle = handle;
	ed->ed_iolen = S_ISDIR(sbuf.st_mode)!=0;

	return EMU_RES_SUCCESS;
}

static
u_int32_t
emufs_close(struct emufs_data *ed)
{
	close(ed->ed_fds[ed->ed_handle]);
	ed->ed_fds[ed->ed_handle] = -1;
	return EMU_RES_SUCCESS;
}

static
u_int32_t
emufs_read(struct emufs_data *ed)
{
	int len;
	int fd;

	if (ed->ed_iolen >= EMU_BUF_SIZE) {
		return EMU_RES_BADSIZE;
	}

	fd = ed->ed_fds[ed->ed_handle];

	lseek(fd, ed->ed_offset, SEEK_SET);
	len = read(fd, ed->ed_buf, ed->ed_iolen);

	if (len < 0) {
		return errno_to_code(errno);
	}

	ed->ed_offset += len;
	ed->ed_iolen = len;

	return EMU_RES_SUCCESS;
}

static
u_int32_t
emufs_readdir(struct emufs_data *ed)
{
	/*
	 * Grr. There's no way to opendir() an fd.
	 */
	(void) ed;
	return EMU_RES_UNSUPP;
}

static
u_int32_t
emufs_write(struct emufs_data *ed)
{
	int len;
	int fd;

	if (ed->ed_iolen >= EMU_BUF_SIZE) {
		return EMU_RES_BADSIZE;
	}

	fd = ed->ed_fds[ed->ed_handle];

	lseek(fd, ed->ed_offset, SEEK_SET);
	len = write(fd, ed->ed_buf, ed->ed_iolen);

	if (len < 0) {
		return errno_to_code(errno);
	}

	ed->ed_offset += len;
	ed->ed_iolen = len;

	return EMU_RES_SUCCESS;
}

static
u_int32_t
emufs_getsize(struct emufs_data *ed)
{
	struct stat sb;
	int fd;

	fd = ed->ed_fds[ed->ed_handle];
	if (fstat(fd, &sb)) {
		return errno_to_code(errno);
	}

	ed->ed_iolen = sb.st_size;

	return EMU_RES_SUCCESS;
}

static
u_int32_t
emufs_trunc(struct emufs_data *ed)
{
	int fd;

	fd = ed->ed_fds[ed->ed_handle];
	if (ftruncate(fd, ed->ed_iolen)) {
		return errno_to_code(errno);
	}

	return EMU_RES_SUCCESS;
}

static
u_int32_t
emufs_op(struct emufs_data *ed, u_int32_t op)
{

	if (ed->ed_handle >= MAXHANDLES || ed->ed_fds[ed->ed_handle]<0) {
		return EMU_RES_BADHANDLE;
	}

	switch (op) {
	    case EMU_OP_OPEN:       return emufs_open(ed, 0);
	    case EMU_OP_CREATE:     return emufs_open(ed, O_CREAT);
	    case EMU_OP_EXCLCREATE: return emufs_open(ed, O_CREAT|O_EXCL);
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
	ed->ed_handle = 0;
	ed->ed_offset = 0;
	ed->ed_iolen = 0;
	ed->ed_result = 0;

	for (i=0; i<MAXHANDLES; i++) {
		ed->ed_fds[i] = -1;
	}

	// XXX

	return ed;
}

static
int
emufs_fetch(void *data, u_int32_t offset, u_int32_t *ret)
{
	struct emufs_data *ed = data;
	u_int32_t *ptr;

	if (offset >= EMU_BUF_START && offset < EMU_BUF_END) {
		offset -= EMU_BUF_START;
		ptr = (u_int32_t *)(ed->ed_buf + offset);
		*ret = *ptr;
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
emufs_store(void *data, u_int32_t offset, u_int32_t val)
{
	struct emufs_data *ed = data;
	u_int32_t *ptr;

	if (offset >= EMU_BUF_START && offset < EMU_BUF_END) {
		offset -= EMU_BUF_START;
		ptr = (u_int32_t *)(ed->ed_buf + offset);
		*ptr = val;
		return 0;
	}

	switch (offset) {
	    case EMUREG_HANDLE: ed->ed_handle = val; return 0;
	    case EMUREG_OFFSET: ed->ed_offset = val; return 0;
	    case EMUREG_IOLEN: ed->ed_iolen = val; return 0;
	    case EMUREG_OPER: emufs_setresult(ed, emufs_op(ed, val)); return 0;
	    case EMUREG_RESULT: emufs_setresult(ed, val); return 0;
	}
	return -1;
}

static
void
emufs_cleanup(void *data)
{
	struct emufs_data *ed = data;
	emufs_close(ed);
	free(ed);
}

struct lamebus_device_info emufs_device_info = {
	LBVEND_CS161,
	LBVEND_CS161_EMUFS,
	EMUFS_REVISION,
	emufs_init,
	emufs_fetch,
	emufs_store,
	emufs_cleanup,
};
