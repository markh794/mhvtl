#ifndef _MHVTL_TRANSPORT_H
#define _MHVTL_TRANSPORT_H

#include "vtl_common.h"

struct vtl_transport {
	const char *name;

	/*
	 * open: Initialize the transport for a given LU.
	 *   - minor: the mhvtl device number (e.g. 11 for vtltape@11)
	 *   - ctl: LU identification (channel/target/lun)
	 * Returns an opaque handle (>= 0) or -1 on error.
	 *
	 * For the mhvtl backend this opens /dev/mhvtlN.
	 * For the TCMU backend this connects to the TCMU device via
	 * libtcmu (the LU must already exist in LIO configfs).
	 */
	int  (*open)(unsigned minor, struct mhvtl_ctl *ctl);

	/*
	 * poll_cmd: Check for a pending SCSI command.
	 *   - handle: from open()
	 *   - hdr: filled with CDB bytes + serial number on VTL_QUEUE_CMD
	 * Returns VTL_QUEUE_CMD, VTL_IDLE, or < 0 on error.
	 */
	int  (*poll_cmd)(int handle, struct mhvtl_header *hdr);

	/*
	 * get_data: Retrieve SCSI write data (DATA-OUT) for the current
	 *           command into ds->data.
	 *   - handle: from open()
	 *   - ds: ds->serialNo identifies the command; ds->sz is the
	 *         requested byte count; ds->data points to the target buffer.
	 * Returns number of bytes copied, or 0 on error.
	 */
	int  (*get_data)(int handle, struct mhvtl_ds *ds);

	/*
	 * put_data: Complete the current SCSI command with response data
	 *           (DATA-IN) and SAM status.
	 *   - handle: from open()
	 *   - ds: ds->serialNo identifies the command; ds->data points to
	 *         response data of ds->sz bytes; ds->sam_stat is the SAM
	 *         status; ds->sense_buf is the sense buffer (if CHECK
	 *         CONDITION).
	 */
	void (*put_data)(int handle, struct mhvtl_ds *ds);

	/*
	 * add_lu: Register a new logical unit.
	 *   - handle: from open()
	 *   - ctl: channel/target/lun
	 * Returns child pid on success (for waitpid), 0 on failure.
	 *
	 * mhvtl backend: writes to kernel module pseudo file (fork+write)
	 * TCMU backend: no-op (LU already exists via targetcli setup),
	 *               returns a dummy pid.
	 */
	pid_t (*add_lu)(unsigned minor, struct mhvtl_ctl *ctl);

	/*
	 * remove_lu: Deregister a logical unit.
	 *   - handle: from open()
	 *   - ctl: channel/target/lun
	 *
	 * mhvtl backend: ioctl(VTL_REMOVE_LU)
	 * TCMU backend: no-op (LU removed via targetcli teardown)
	 */
	void (*remove_lu)(int handle, struct mhvtl_ctl *ctl);

	/*
	 * close: Shut down the transport for this LU.
	 */
	void (*close)(int handle);
};

/* Global transport backend, selected at startup. */
extern struct vtl_transport *vtl_transport;

/* Backend constructors. */
struct vtl_transport *transport_mhvtl_init(void);
#ifdef MHVTL_TCMU_BACKEND
struct vtl_transport *transport_tcmu_init(void);
#endif
#ifdef MHVTL_USERLAND_BACKEND
struct vtl_transport *transport_userland_init(void);
#endif

/*
 * Select the transport backend based on environment variable
 * MHVTL_BACKEND ("mhvtl" or "tcmu") or compile-time default.
 */
void transport_select(void);

#endif /* _MHVTL_TRANSPORT_H */
