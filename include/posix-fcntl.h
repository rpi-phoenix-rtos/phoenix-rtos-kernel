/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * POSIX-compatibility definitions - fcntl
 *
 * Copyright 2018, 2024 Phoenix Systems
 * Author: Jan Sikorski, Lukasz Leczkowski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef _PH_POSIX_FCNTL_H_
#define _PH_POSIX_FCNTL_H_


#define FD_CLOEXEC 0x1U

#define O_RDONLY   0x00001U
#define O_WRONLY   0x00002U
#define O_RDWR     0x00004U
#define O_APPEND   0x00008U
#define O_CREAT    0x00100U
#define O_TRUNC    0x00200U
#define O_EXCL     0x00400U
#define O_SYNC     0x00800U
#define O_NONBLOCK 0x01000U
#define O_NDELAY   O_NONBLOCK
#define O_NOCTTY   0x02000U
#define O_CLOEXEC  0x04000U
#define O_RSYNC    0x08000U
#define O_DSYNC    0x10000U

#define O_ACCMODE (O_RDONLY | O_WRONLY | O_RDWR)

/* clang-format off */

/* fcntl() operations */
enum { F_DUPFD = 0, F_DUPFD_CLOEXEC, F_GETFD, F_SETFD, F_GETFL, F_SETFL,
	F_GETOWN, F_SETOWN, F_GETLK, F_SETLK, F_SETLKW };

/* Self-referential macros so portable code that probes with `#ifdef F_GETFL`
 * (autoconf, gnulib, bfd, ...) detects these operations — the C preprocessor
 * cannot see the enum constants above. Each macro expands to the same
 * identifier (the enum value), so the values and behaviour are unchanged. The
 * enum is fully parsed before these are defined, so it is not affected. */
#define F_DUPFD         F_DUPFD
#define F_DUPFD_CLOEXEC F_DUPFD_CLOEXEC
#define F_GETFD         F_GETFD
#define F_SETFD         F_SETFD
#define F_GETFL         F_GETFL
#define F_SETFL         F_SETFL
#define F_GETOWN        F_GETOWN
#define F_SETOWN        F_SETOWN
#define F_GETLK         F_GETLK
#define F_SETLK         F_SETLK
#define F_SETLKW        F_SETLKW

/* clang-format on */


#endif
