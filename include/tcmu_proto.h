/*
 * tcmu_proto.h — Wire protocol between mhvtl TCMU handler and tape daemons.
 *
 * Communication is over a unix domain socket. The handler daemon
 * listens, each tape/library daemon connects.
 *
 * Message flow:
 *
 *   Daemon → Handler:  MSG_REGISTER   (on connect)
 *   Handler → Daemon:  MSG_REGISTER_OK / MSG_REGISTER_ERR
 *
 *   Handler → Daemon:  MSG_CDB        (SCSI command from kernel)
 *   Daemon → Handler:  MSG_CDB_RESP   (response + data + status)
 *
 *   Handler → Daemon:  MSG_DATA_OUT   (write data, follows MSG_CDB for WRITE cmds)
 *   Daemon → Handler:  MSG_DATA_REQ   (daemon requests DATA-OUT)
 *
 * All messages are prefixed with struct tcmu_msg_hdr.
 * Data follows the header inline (no separate data channel).
 */

#ifndef _MHVTL_TCMU_PROTO_H
#define _MHVTL_TCMU_PROTO_H

#include <stdint.h>

#define TCMU_SOCK_PATH "/var/run/mhvtl/tcmu_handler.sock"
#define TCMU_MAX_CDB_SIZE 16
#define TCMU_MAX_SENSE_SIZE 96
#define TCMU_MAX_DATA_SIZE (2 * 1024 * 1024)  /* 2MB, matches mhvtl bufsize */

/* Message types */
enum tcmu_msg_type {
	MSG_REGISTER      = 1,  /* daemon → handler: register this device */
	MSG_REGISTER_OK   = 2,  /* handler → daemon: registration succeeded */
	MSG_REGISTER_ERR  = 3,  /* handler → daemon: registration failed */
	MSG_CDB           = 4,  /* handler → daemon: here's a SCSI command */
	MSG_CDB_RESP      = 5,  /* daemon → handler: command response */
	MSG_DATA_REQ      = 6,  /* daemon → handler: send me DATA-OUT */
	MSG_DATA_OUT      = 7,  /* handler → daemon: here's DATA-OUT data */
	MSG_SHUTDOWN      = 8,  /* handler → daemon: device going away */
};

/* Common header for all messages */
struct tcmu_msg_hdr {
	uint32_t type;          /* enum tcmu_msg_type */
	uint32_t data_len;      /* bytes following this header */
};

/* MSG_REGISTER: daemon tells handler what device it serves */
struct tcmu_msg_register {
	struct tcmu_msg_hdr hdr;
	uint32_t minor;         /* device minor number (e.g. 11) */
	uint32_t dev_type;      /* SCSI device type: 1=tape, 8=changer */
	uint32_t channel;       /* SCSI channel from device.conf */
	uint32_t target;        /* SCSI target from device.conf */
	uint32_t lun;           /* SCSI LUN from device.conf */
	char     bs_name[32];   /* backstore name: "tape11" or "lib10" */
};

/* MSG_CDB: handler sends a SCSI command to the daemon */
struct tcmu_msg_cdb {
	struct tcmu_msg_hdr hdr;
	uint64_t cmd_id;        /* opaque ID to correlate response */
	uint8_t  cdb[TCMU_MAX_CDB_SIZE];
	uint32_t data_len;      /* expected DATA-IN length (for reads) */
};

/* MSG_CDB_RESP: daemon sends command result back */
struct tcmu_msg_cdb_resp {
	struct tcmu_msg_hdr hdr;
	uint64_t cmd_id;        /* matches MSG_CDB.cmd_id */
	uint8_t  sam_stat;      /* SAM status (0=GOOD, 2=CHECK CONDITION) */
	uint8_t  sense[TCMU_MAX_SENSE_SIZE];
	uint32_t data_len;      /* DATA-IN bytes following this struct */
	/* data_len bytes of DATA-IN follow immediately */
};

/* MSG_DATA_REQ: daemon requests DATA-OUT for a WRITE command */
struct tcmu_msg_data_req {
	struct tcmu_msg_hdr hdr;
	uint64_t cmd_id;
	uint32_t data_len;      /* how many bytes the daemon wants */
};

/* MSG_DATA_OUT: handler sends DATA-OUT to daemon */
struct tcmu_msg_data_out {
	struct tcmu_msg_hdr hdr;
	uint64_t cmd_id;
	uint32_t data_len;      /* bytes following this struct */
	/* data_len bytes of DATA-OUT follow immediately */
};

#endif /* _MHVTL_TCMU_PROTO_H */
