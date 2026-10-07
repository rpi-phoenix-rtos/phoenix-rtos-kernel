/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * Messages
 *
 * Copyright 2017 Phoenix Systems
 * Author: Pawel Pisarczyk, Jakub Sejdak
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _PH_MSG_H_
#define _PH_MSG_H_

/* Return id, allocated in msgReceive, used in msgRespond */
typedef int msg_rid_t;

/*
 * Message types
 */

/* clang-format off */

enum {
	/* File operations */
	mtOpen = 0, mtClose, mtRead, mtWrite, mtTruncate, mtDevCtl,

	/* Object operations */
	mtCreate, mtDestroy, mtSetAttr, mtGetAttr, mtGetAttrAll,

	/* Directory operations */
	mtLookup, mtLink, mtUnlink, mtReaddir,

	mtCount,

	mtStat = 0xf53 /* Moved from libphoenix. */
};

/* clang-format on */


/*
 * mtReaddir flags (i.readdir.flags)
 *
 * MSG_READDIR_NEXT: the client takes the position of the following entry from
 * o.readdir.next instead of adding d_reclen to i.readdir.offs. A position is
 * opaque, and a server that sets o.readdir.next (always > i.readdir.offs) can
 * use positions that survive the removal of other entries during a scan.
 * A server that does not know the flag leaves o.readdir.next untouched.
 */
#define MSG_READDIR_NEXT (1U << 0)


#pragma pack(push, 8)


struct _attr {
	long long val;
	int err;
};


struct _attrAll {
	struct _attr mode;
	struct _attr uid;
	struct _attr gid;
	struct _attr size;
	struct _attr blocks;
	struct _attr ioblock;
	struct _attr type;
	struct _attr port;
	struct _attr pollStatus;
	struct _attr eventMask;
	struct _attr cTime;
	struct _attr mTime;
	struct _attr aTime;
	struct _attr links;
	struct _attr dev;
};


typedef struct _msg_t {
	int type;
	int pid;
	int priority;
	oid_t oid;

	struct {
		union {
			/* OPEN/CLOSE */
			struct {
				unsigned int flags;
			} openclose;

			/* READ/WRITE/TRUNCATE */
			struct {
				off_t offs;
				size_t len;
				unsigned int mode;
			} io;

			/* CREATE */
			struct {
				int type;
				unsigned int mode;
				oid_t dev;
			} create;

			/* SETATTR/GETATTR */
			struct {
				long long val;
				int type;
			} attr;

			/* LINK/UNLINK */
			struct {
				oid_t oid;
			} ln;

			/* READDIR */
			struct {
				off_t offs;
				unsigned int flags; /* MSG_READDIR_* */
			} readdir;

			unsigned char raw[64];
		};

		size_t size;
		const void *data;
	} i;

	struct {
		union {
			/* ATTR */
			struct {
				long long val;
			} attr;

			/* WRITE */
			struct {
				off_t offs;
			} io;

			/* CREATE */
			struct {
				oid_t oid;
			} create;

			/* LOOKUP */
			struct {
				oid_t fil;
				oid_t dev;
			} lookup;

			/* READDIR, only with MSG_READDIR_NEXT */
			struct {
				off_t next;
			} readdir;

			unsigned char raw[64];
		};

		int err;
		size_t size;
		void *data;
	} o;

} msg_t;


#pragma pack(pop)


#endif
