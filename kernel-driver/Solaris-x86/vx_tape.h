/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2005 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#ifndef _SYS_SCSI_ADAPTERS_VX_TAPE_H
#define	_SYS_SCSI_ADAPTERS_VX_TAPE_H

#pragma ident	"@(#)vx_tape.h	1.1	05/06/27 SMI"

/*
 * This file defines the commands and structures for three vx_tape ioctls,
 * that may be useful in speeding up tests involving large devices.  The
 * ioctls are documented at
 * http://lvm.central.sun.com/projects/lagavulin/vx_tape_design.html#ioctl.
 * Briefly, there are three ioctls:
 *
 *	VX_TAPE_WRITE_OFF - ignore all write operations to a specified block
 *		range.
 *	VX_TAPE_WRITE_ON - enable writes to a specified block range.
 *	VX_TAPE_ZERO_RANGE - zero all blocks in the specified range.
 *
 * The vx_tape_range structure is used to specify a block range for these
 * ioctls.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/inttypes.h>
#include <sys/types.h>

/*
 * vx_tape ioctl commands:
 */

#define	VX_TAPEIOC	('e' << 8)

#define	VX_TAPE_WRITE_OFF	(VX_TAPEIOC|37)
#define	VX_TAPE_WRITE_ON		(VX_TAPEIOC|38)
#define	VX_TAPE_ZERO_RANGE	(VX_TAPEIOC|39)

struct vx_tape_range {
	diskaddr_t	vx_tape_sb;	/* starting block # of range */
	uint64_t	vx_tape_blkcnt;	/* # of blocks in range */
};

typedef struct vx_tape_range vx_tape_range_t;

/*
 * Structure to use when specifying an ioctl for a range of blocks on a
 * specific target.
 */
struct vx_tape_tgt_range {
	vx_tape_range_t	vx_tape_blkrange; /* blocks affected by ioctl */
	ushort_t	vx_tape_target;	/* target number of disk */
	ushort_t	vx_tape_lun;	/* lun of disk */
};

typedef struct vx_tape_tgt_range vx_tape_tgt_range_t;

#ifdef __cplusplus
}
#endif

#endif /* _SYS_SCSI_ADAPTERS_VX_TAPE_H */
