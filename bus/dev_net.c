#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include "config.h"

#include "console.h"
#include "clock.h"
#include "onsel.h"
#include "main.h"
#include "util.h"

#include "busids.h"
#include "lamebus.h"

const char rcsid_dev_net_c[] = 
    "$Id: dev_net.c,v 1.8 2001/06/05 20:05:06 dholland Exp $";

#define NETREG_READINTR    0
#define NETREG_WRITEINTR   4
#define NETREG_CONTROL     8
#define NETREG_STATUS      12

#define NET_READBUF     32768
#define NET_WRITEBUF    (NET_READBUF+NET_BUFSIZE)
#define NET_BUFSIZE     4096

#define HUB_ADDR        0x0000
#define BROADCAST_ADDR  0xffff

#define FRAME_MAGIC     0xa4b3

struct net_data {
	int nd_slot;

	struct sockaddr_un nd_hubaddr;
	int nd_socket;
	
	int nd_lostcarrier;

	u_int32_t nd_rirq;
	u_int32_t nd_wirq;
	u_int32_t nd_control;
	u_int32_t nd_status;

	char nd_rbuf[NET_BUFSIZE];
	char nd_wbuf[NET_BUFSIZE];
};

/* Fields in interrupt registers */
#define NDI_DONE         0x00000001
#define NDI_ZERO         0xfffffffe

/* Fields in control register */
#define NDC_PROMISC      0x00000001
#define NDC_START        0x00000002
#define NDC_ZERO         0xfffffffc

/* Fields in status register */
#define NDS_HWADDR       0x0000ffff
#define NDS_ZERO         0xffff0000

#define ND_STATUS(hw, c) (((c)?0x80000000:0) | ((u_int32_t)(hw)&0xffff))

struct linkheader {
	u_int16_t lh_frame;
	u_int16_t lh_from;
	u_int16_t lh_packetlen;
	u_int16_t lh_to;
};

////////////////////////////////////////////////////////////

static
void
chkint(struct net_data *nd)
{
	if (nd->nd_rirq || nd->nd_wirq) {
		RAISE_IRQ(nd->nd_slot);
	}
	else {
		LOWER_IRQ(nd->nd_slot);
	}
}

static
void
readdone(struct net_data *nd)
{
	nd->nd_rirq = NDI_DONE;
	chkint(nd);
}

static
void
writedone(struct net_data *nd)
{
	nd->nd_wirq = NDI_DONE;
	chkint(nd);
}

////////////////////////////////////////////////////////////

static
void
keepalive(void *data, u_int32_t junk)
{
	struct linkheader lh;
	struct net_data *nd = data;
	int r;
	(void)junk;

	/*
	 * Because we're using a connectionless socket, the hub won't
	 * know we exist until we send it a packet. So let's send it a
	 * packet. In fact, let's do this say once a second all along
	 * in case the hub crashes and restarts.
	 */

	lh.lh_frame = htons(FRAME_MAGIC);
	lh.lh_from = htons(nd->nd_status & NDS_HWADDR);
	lh.lh_packetlen = htons(sizeof(lh));
	lh.lh_to = htons(HUB_ADDR);

	r = sendto(nd->nd_socket, &lh, sizeof(lh), 0, 
	       (struct sockaddr *)&nd->nd_hubaddr, nd->nd_hubaddr.sun_len);

	if (r<0 && errno==ECONNREFUSED) {
		/*
		 * No carrier.
		 */
		if (!nd->nd_lostcarrier) {
			msg("nic: slot %d: lost carrier", nd->nd_slot);
			nd->nd_lostcarrier = 1;
		}
	}
	else if (r<0) {
		msg("nic: slot %d: keepalive failed: %s", nd->nd_slot,
		    strerror(errno));
	}
	else {
		if (nd->nd_lostcarrier) {
			msg("nic: slot %d: carrier detected", nd->nd_slot);
			nd->nd_lostcarrier = 0;
		}
	}

	schedule_event(1000000000, nd, 0, keepalive);
}

static
void
dosend(struct net_data *nd)
{
	struct linkheader *lh = (struct linkheader *)nd->nd_wbuf;
	u_int32_t len;
	int r;

	len = ntohs(lh->lh_packetlen);

	if (len > NET_BUFSIZE) {
		hang("Packet size too long");
		return;
	}

	/*
	 * Force the link-level header to the right values
	 */
	lh->lh_frame = htons(FRAME_MAGIC);
	lh->lh_from = htons(nd->nd_status & NDS_HWADDR);

	r = sendto(nd->nd_socket, nd->nd_wbuf, len, 0, 
	       (struct sockaddr *)&nd->nd_hubaddr, nd->nd_hubaddr.sun_len);
	if (r<0) {
		msg("nic: slot %d: sendto: %s", nd->nd_slot, strerror(errno));
	}

	g_stats.s_wpkts++;

	// XXX latency
	writedone(nd);
}

static
int
dorecv(void *data)
{
	struct net_data *nd = data;

	char junk[8];
	char *readbuf;
	size_t readbuflen;

	struct linkheader *lh;

	int overrun=0, r;

	if (nd->nd_rirq != 0) {
		/*
		 * The last packet we got hasn't cleared yet.
		 * Drop this one.
		 */
		overrun = 1;
		readbuf = junk;
		readbuflen = sizeof(junk);
	}
	else {
		readbuf = nd->nd_rbuf;
		readbuflen = NET_BUFSIZE;
	}

	r = read(nd->nd_socket, nd->nd_rbuf, NET_BUFSIZE);
	if (r<0) {
		msg("nic: slot %d: read: %s", nd->nd_slot, strerror(errno));
		return 0;
	}
	if (r < 8) {
		//msg("nic: slot %d: short packet", nd->nd_slot);
		g_stats.s_epkts++;
		return 0;
	}

	lh = (struct linkheader *)readbuf;

	if (ntohs(lh->lh_frame) != FRAME_MAGIC) {
		//msg("nic: slot %d: framing error", nd->nd_slot);
		g_stats.s_epkts++;
		return 0;
	}

	if (ntohs(lh->lh_to) != (nd->nd_status & NDS_HWADDR) &&
	    ntohs(lh->lh_to) != BROADCAST_ADDR && 
	    (nd->nd_control & NDC_PROMISC)==0) {
		return 0;
	}

	if (ntohs(lh->lh_packetlen) > r) {
		//msg("nic: slot %d: truncated packet", nd->nd_slot);
		g_stats.s_epkts++;
		return 0;
	}

	if (overrun) {
		//msg("nic: slot %d: overrun (packet dropped)", nd->nd_slot);
		g_stats.s_dpkts++;
		return 0;
	}

	g_stats.s_rpkts++;

	// XXX latency?

	readdone(nd);

	return 0;
}

////////////////////////////////////////////////////////////

static
void
setirq(struct net_data *nd, u_int32_t val, int isread)
{
	if ((val & NDI_ZERO) != 0) {
		hang("Illegal network interrupt register write");
	}
	else {
		if (isread) {
			nd->nd_rirq = val;
		}
		else {
			nd->nd_wirq = val;
		}
		chkint(nd);
	}
}

static
void
setctl(struct net_data *nd, u_int32_t val)
{
	if ((val & NDC_ZERO) != 0) {
		hang("Illegal network control register write");
	}
	else {
		if (val & NDC_START) {
			dosend(nd);
			val &= ~NDC_START;
		}
		nd->nd_control = val;
	}
}

////////////////////////////////////////////////////////////

static
int
net_fetch(void *d, u_int32_t offset, u_int32_t *val)
{
	struct net_data *nd = d;

	if (offset >= NET_READBUF && offset < NET_READBUF+NET_BUFSIZE) {
		char *ptr = &nd->nd_rbuf[offset - NET_READBUF];
		*val = *(u_int32_t *)ptr;
		return 0;
	}
	else if (offset >= NET_WRITEBUF && offset < NET_WRITEBUF+NET_BUFSIZE) {
		char *ptr = &nd->nd_wbuf[offset - NET_WRITEBUF];
		*val = *(u_int32_t *)ptr;
		return 0;
	}
	switch (offset) {
	    case NETREG_READINTR: *val = nd->nd_rirq; return 0;
	    case NETREG_WRITEINTR: *val = nd->nd_wirq; return 0;
	    case NETREG_CONTROL: *val = nd->nd_control; return 0;
	    case NETREG_STATUS: *val = nd->nd_status; return 0;
	}
	return -1;
}

static
int
net_store(void *d, u_int32_t offset, u_int32_t val)
{
	struct net_data *nd = d;

	if (offset >= NET_READBUF && offset < NET_READBUF+NET_BUFSIZE) {
		char *ptr = &nd->nd_rbuf[offset - NET_READBUF];
		*(u_int32_t *)ptr = val;
		return 0;
	}
	else if (offset >= NET_WRITEBUF && offset < NET_WRITEBUF+NET_BUFSIZE) {
		char *ptr = &nd->nd_wbuf[offset - NET_WRITEBUF];
		*(u_int32_t *)ptr = val;
		return 0;
	}
	switch (offset) {
	    case NETREG_READINTR: setirq(nd, val, 1); break;
	    case NETREG_WRITEINTR: setirq(nd, val, 0); break;
	    case NETREG_CONTROL: setctl(nd, val); break;
	    case NETREG_STATUS: return -1;
	    default: return -1;
	}
	return 0;
}

static
void
net_cleanup(void *d)
{
	struct net_data *nd = d;

	if (nd->nd_socket >= 0) {
		close(nd->nd_socket);
		nd->nd_socket = -1;
	}

	free(nd);
}

static
void *
net_init(int slot, int argc, char *argv[])
{
	struct net_data *nd = domalloc(sizeof(struct net_data));
	const char *hubname = ".sockets/hub";
	u_int16_t hwaddr = 0;

	struct sockaddr_un mysun;

	int i, one=1;

	for (i=1; i<argc; i++) {
		if (!strncmp(argv[i], "hub=", 7)) {
			hubname = argv[i]+7;
		}
		else if (!strncmp(argv[i], "hwaddr=", 7)) {
			hwaddr = atoi(argv[i]+7);
			if (hwaddr == BROADCAST_ADDR || hwaddr == HUB_ADDR) {
				msg("nic: slot %d: invalid hwaddr", slot);
				die();
			}
		}
		else {
			msg("nic: slot %d: invalid option %s", slot, argv[i]);
			die();
		}
	}

	nd->nd_slot = slot;

	nd->nd_status = ND_STATUS(hwaddr, 0);

	nd->nd_socket = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (nd->nd_socket < 0) {
		msg("nic: slot %d: socket: %s", slot, strerror(errno));
		die();
	}

	nd->nd_lostcarrier = 1;

	memset(&mysun, 0, sizeof(mysun));
	mysun.sun_family = AF_UNIX;
	snprintf(mysun.sun_path, sizeof(mysun.sun_path),
		 ".sockets/net-%04x", hwaddr);
	mysun.sun_len = SUN_LEN(&mysun);

	unlink(mysun.sun_path);
	setsockopt(nd->nd_socket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	if (bind(nd->nd_socket, (struct sockaddr *)&mysun, mysun.sun_len)<0) {
		msg("nic: slot %d: bind: %s", slot, strerror(errno));
		die();
	}

	memset(&nd->nd_hubaddr, 0, sizeof(nd->nd_hubaddr));
	nd->nd_hubaddr.sun_family = AF_UNIX;
	strcpy(nd->nd_hubaddr.sun_path, hubname);
	nd->nd_hubaddr.sun_len = SUN_LEN(&nd->nd_hubaddr);

	onselect(nd->nd_socket, nd, dorecv, NULL);

	keepalive(nd, 0);

	return nd;
}

const struct lamebus_device_info net_device_info = {
	LBVEND_CS161,
	LBVEND_CS161_NET,
	NET_REVISION,
	net_init,
	net_fetch,
	net_store,
	net_cleanup,
};
