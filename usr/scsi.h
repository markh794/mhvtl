/*
 * The userspace tape/library header file for the vtl virtual
 * tape kernel module.
 *
 * Copyright (C) 2005 Mark Harvey markh794 at gmail dot com
 *                                mark.harvey at veritas dot com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ifndef	_SCSI_SCSI_H
#define	_SCSI_SCSI_H	1

/*
 * Device types
 */
#define TYPE_DISK           0x00
#define TYPE_TAPE           0x01
#define TYPE_PROCESSOR      0x03
#define TYPE_WORM           0x04
#define TYPE_ROM            0x05
#define TYPE_SCANNER        0x06
#define TYPE_MOD            0x07
#define TYPE_MEDIUM_CHANGER 0x08
#define TYPE_ENCLOSURE      0x0d
#define TYPE_NO_LUN         0x7f

/*
 * SCSI Architectual model (SAM) Status codes.
 */
#define SAM_STAT_GOOD			0x00
#define SAM_STAT_CHECK_CONDITION	0x02
#define SAM_STAT_CONDITION_MET		0x04
#define	SAM_STAT_BUSY			0x08
#define	SAM_STAT_INTERMEDIATE		0x10
#define	SAM_STAT_INTERMEDIATE_CONDITION_MET 0x14
#define SAM_STAT_RESERVATION_CONFLICT	0x18
#define SAM_STAT_COMMAND_TERMINATED	0x22
#define SAM_STAT_QUEUE_FULL		0x28

/*
 * SENSE keys
 */
#define NO_SENSE		0x00
#define RECOVERED_ERROR		0x01
#define NOT_READY		0x02
#define MEDIUM_ERROR		0x03
#define HARDWARE_ERROR		0x04
#define ILLEGAL_REQUEST		0x05
#define UNIT_ATTENTION		0x06
#define DATA_PROTECT		0x07
#define BLANK_CHECK		0x08
#define VOLUME_OVERFLOW		0x0d

/*
 * OP Codes
 */

#define	ALLOW_MEDIUM_REMOVAL	0x1e
#define ALLOW_OVERWRITE		0x82
#define ACCESS_CONTROL_IN	0x86
#define ACCESS_CONTROL_OUT	0x87
#define EXTENDED_COPY		0x83
#define A3_SA			0xa3
#define A4_SA			0xa4
#define ERASE_6			0x19
#define ERASE_16		0x93
#define FORMAT_UNIT		0x04
#define EXCHANGE_MEDIUM		0xa6
#define INITIALIZE_ELEMENT_STATUS	0x07
#define INITIALIZE_ELEMENT_STATUS_WITH_RANGE 0xE7
#define INQUIRY			0x12
#define LOAD_DISPLAY		0x06	/* STK T10000 specific */
#define LOCATE_10		0x2b
#define LOCATE_16		0x92
#define MODE_SENSE		0x1a
#define MODE_SENSE_10		0x5a
#define MODE_SELECT		0x15
#define	MODE_SELECT_10		0x55
#define	MOVE_MEDIUM		0xa5
#define	LOG_SELECT		0x4c
#define	LOG_SENSE		0x4d
#define PERSISTENT_RESERVE_IN	0x5e
#define PERSISTENT_RESERVE_OUT	0x5f
#define	READ_6			0x08
#define	READ_10			0x28
#define	READ_12			0xa8
#define	READ_16			0x88
#define READ_ATTRIBUTE		0x8c
#define READ_BLOCK_LIMITS	0x05
#define READ_BUFFER		0x3c
#define	READ_ELEMENT_STATUS	0xb8
#define READ_MEDIA_SERIAL_NUMBER	0xab
#define	READ_POSITION		0x34
#define	READ_REVERSE_6		0x0f
#define	READ_REVERSE_16		0x81
#define RECEIVE_DIAGNOSTIC	0x1c
#define RECOVER_BUFFERED_DATA	0x14
#define	RELEASE			0x17
#define	RELEASE_10		0x57
#define REPORT_DENSITY		0x44
#define REPORT_ELEMENT_INFORMATION 0x9e
#define	REPORT_LUNS		0xa0
#define	REQUEST_SENSE		0x03
#define	RESERVE			0x16
#define	RESERVE_10		0x56
#define	REZERO_UNIT		0x01
#define SECURITY_PROTOCOL_IN	0xa2
#define SECURITY_PROTOCOL_OUT	0xb5
#define SEND_DIAGNOSTIC		0x1d
#define SET_CAPACITY		0x0b
#define	SPACE			0x11
#define	SPACE_16		0x91
#define	START_STOP		0x1b
#define	TEST_UNIT_READY		0x00
#define VERIFY_6		0x13
#define VERIFY_16		0x8f
#define	WRITE_6			0x0a
#define	WRITE_10		0x2a
#define	WRITE_12		0xaa
#define	WRITE_16		0x8a
#define WRITE_ATTRIBUTE		0x8d
#define WRITE_BUFFER		0x3b
#define	WRITE_FILEMARKS		0x10
#define	WRITE_FILEMARKS_16	0x80

/* Service codes */
#define FORCED_EJECT		0x1f
#define MANAGEMENT_PROTOCOL_IN	0x10
#define MANAGEMENT_PROTOCOL_OUT	0x10
#define CHANGE_ALIASES		0x0b
#define REPORT_ALIASES		0x0b
#define REPORT_SUPPORTED_OPCODES 0x0c

/* No Sense Errors */
#define NO_ADDITIONAL_SENSE		0x0000
#define E_MARK				0x0001
#define E_EOM				0x0002
#define E_BOM				0x0004
#define E_END_OF_DATA			0x0005
#define E_PROGRAMMABLE_EARLY_WARNING	0x0007
#define E_OP_IN_PROGRESS		0x0016
#define E_DRIVE_REQUIRES_CLEANING	0x8282

/* Recovered Errors */
#define E_WRITE_ERROR			0x0c00
#define E_READ_ERROR			0x1100
#define E_RECOVERED_WITH_RETRYS		0x1701
#define E_MEDIA_LOAD_EJECT_ERROR	0x5300
#define E_FAILURE_PREDICTION		0x5d00

/* Not ready */
#define E_CAUSE_NOT_REPORTABLE		0x0400
#define E_BECOMING_READY		0x0401
#define E_INITIALIZING_REQUIRED		0x0402
#define E_OFFLINE			0x0412
#define E_EARLY_WARNING_EOM		0x2c98
#define E_CLEANING_CART_INSTALLED	0x3003
#define E_CLEANING_FAILURE		0x3007
#define E_MEDIUM_NOT_PRESENT		0x3a00
#define E_MAP_OPEN			0x3a02
#define E_LOGICAL_UNIT_NOT_CONFIG	0x3e00

/* Medium Errors */
#define E_WRITE_ERROR			0x0c00
#define E_UNRECOVERED_READ		0x1100
#define E_RECORDED_ENTITY_NOT_FOUND	0x1400
#define E_UNKNOWN_FORMAT		0x3001
#define E_IMCOMPATIBLE_FORMAT		0x3002
#define E_MEDIUM_FMT_CORRUPT		0x3100
#define E_SEQUENTIAL_POSITION_ERR	0x3b00
#define E_WRITE_APPEND_ERR		0x5000
#define E_CARTRIDGE_FAULT		0x5200

/* Hardware Failure */
#define E_COMPRESSION_CHECK		0x0c04
#define E_DECOMPRESSION_CRC		0x110d
#define E_MANUAL_INTERVENTION_REQ	0x0403
#define E_HARDWARE_FAILURE		0x4000
#define E_INTERNAL_TARGET_FAILURE	0x4400
#define E_ERASE_FAILURE			0x5100

/* Illegal Request */
#define E_PARAMETER_LIST_LENGTH_ERR	0x1a00
#define E_INVALID_OP_CODE		0x2000
#define E_ILLEGAL_COMMAND_WHEN_NOT_APPEND	0x200C
#define E_INVALID_ELEMENT_ADDR		0x2101
#define E_INVALID_FIELD_IN_CDB		0x2400
#define E_LUN_NOT_SUPPORTED		0x2500
#define E_INVALID_FIELD_IN_PARMS	0x2600
#define E_MEDIUM_INCOMPATIBLE		0x3000
#define E_SAVING_PARMS_UNSUP		0x3900
#define E_SEQUENTIAL_POSITIONING_ERROR	0x3b00
#define E_MEDIUM_DEST_FULL		0x3b0d
#define E_MEDIUM_SRC_EMPTY		0x3b0e
#define E_POSITION_PAST_BOM		0x3b0c
#define E_MEDIUM_REMOVAL_PREVENTED	0x5302
#define E_BAD_MICROCODE_DETECTED	0x8283

/* Unit Attention */
#define E_NOT_READY_TO_TRANSITION	0x2800
#define E_IMPORT_ELEMENT_ACCESSED	0x2801
#define E_POWERON_RESET			0x2900
#define E_I_T_NEXUS_LOSS		0x2907
#define E_MODE_PARAMETERS_CHANGED	0x2a01
#define E_MICROCODE_DOWNLOADED		0x3f01
#define E_FAILURE_PREDICTION_FALSE	0x5dff
#define E_MODE_PARAMETERS_CHANGED	0x2a01
#define E_INQUIRY_DATA_HAS_CHANGED	0x3f03

/* Data Protect */
#define E_WRITE_PROTECT			0x2700
#define E_MEDIUM_OVERWRITE_ATTEMPT	0x300c
#define E_UNABLE_TO_DECRYPT		0x7401
#define E_UNENCRYPTED_DATA		0x7402
#define E_INCORRECT_KEY			0x7403

/* Suppress Incorrect Length Indicator */
#define SILI		0x2
/* Fixed block format */
#define FIXED		0x1

/* Sense Data format bits & pieces */
/* Incorrect Length Indicator */
#define SD_CURRENT_INFORMATION_FIXED 0x70
#define SD_VALID	0x80
#define SD_FILEMARK	0x80
#define SD_EOM		0x40
#define SD_ILI		0x20

/* Sense - field pointer sense key specific data - SPC4 - 4.5.4.2 */
#define SKSV		0x80
#define CD		0x40
#define BPV		0x08

/* MODE PAGE */
#define MODE_RW_ERROR_RECOVER		0x01
#define MODE_DISCONNECT_RECONNECT	0x02
#define MODE_CONTROL			0x0a
#define MODE_DATA_COMPRESSION		0x0f
#define MODE_DEVICE_CONFIGURATION	0x10
#define MODE_MEDIUM_PARTITION		0x11
#define MODE_POWER_CONDITION		0x1a
#define MODE_INFORMATION_EXCEPTION	0x1c
#define MODE_MEDIUM_CONFIGURATION	0x1d
#define MODE_ELEMENT_ADDRESS		0x1d
#define MODE_TRANSPORT_GEOMETRY		0x1e
#define MODE_DEVICE_CAPABILITIES	0x1f
#define MODE_VENDOR_SPECIFIC_24H	0x24
#define MODE_VENDOR_SPECIFIC_25H	0x25
#define MODE_BEHAVIOR_CONFIGURATION	0x2f
#define MODE_ENCRYPTION_MODE		0x30
#define MODE_AIT_DEVICE_CONFIGURATION	0x31

#define TA_NONE				0x00
#define TA_READ				0x01
#define TA_WRITE			0x02
#define TA_HARD				0x04
#define TA_MEDIA			0x08
#define TA_READ_FAILURE			0x10
#define TA_WRITE_FAILURE		0X20
#define TA_MEDIA_EOL			0x40
#define TA_NOT_DATA_GRADE		0X80
#define TA_WRITE_PROTECT		0X100
#define TA_DRIVE_IN_USE			0x200
#define TA_CLEANING_MEDIA		0x400
#define TA_MEDIA_NOT_SUPPORTED		0x800
#define TA_RECOV_CART_FAILURE		0x1000
#define TA_UNRECOV_CART_FAILURE		0x2000
#define TA_MEM_CHIP_FAILURE		0x4000
#define TA_TAPE_UNMOUNTED		0x8000
#define TA_MEDIA_RO			0x10000
#define TA_MEDIA_DIR_CORRUPT		0x20000
#define TA_MEDIA_NEAR_EOL		0x40000
#define TA_DRIVE_NEEDS_CLEANING		0x80000
#define TA_CLEAN_PERIODIC		0x100000
#define TA_EXPIRED_CLEANING_CART	0x200000
#define TA_INVALID_CLEANING_CART	0x400000
#define TA_RETENTION_REQUESTED		0x800000
#define TA_INTERFACE_PORT_ERROR		0x1000000
#define TA_COOLING_FAN_FAILURE		0x2000000
#define TA_PSU_FAILURE			0x4000000
#define TA_PSU_POWER_CONSUMPTION	0x8000000
#define TA_PREVENTIVE_MAINTENANCE	0x10000000
#define TA_HARDWARE_A_FAULT		0x20000000
#define TA_HARDWARE_B_FAULT		0x40000000
#define TA_INTERFACE_FAULT		0x80000000
#define TA_MOUNT_FAILURE		0x100000000ull
#define TA_INCORRECT_FIRMWARE		0x200000000ull
#define TA_HUMIDITY_CONDITIONS		0x400000000ull
#define TA_TEMPERATURE_CONDITIONS	0x800000000ull
#define TA_DRIVE_VOLTAGE		0x1000000000ull
#define TA_PREDICTIVE_FAILURE		0x2000000000ull
#define TA_DIAGNOSTICS_REQUIRED		0x4000000000ull
#define TA_LOST_STATISTICS		0x2000000000000ull
#define TA_TAPE_DIR_INVALID		0x4000000000000ull
#define TA_SYSTEM_AREA_WRITE_FAILURE	0x8000000000000ull
#define TA_SYSTEM_AREA_READ_FAILURE	0x10000000000000ull
#define TA_NO_START_OF_DATA		0x20000000000000ull
#define TA_LOAD_FAILURE			0x40000000000000ull
#define TA_UNRECOV_UNLOAD_FAILURE	0x80000000000000ull
#define TA_AUTOMATION_FAILURE		0x100000000000000ull
#define TA_FIRMWARE_FAILURE		0x200000000000000ull
#define TA_WORM_INTEGRITY_FAILED	0x400000000000000ull
#define TA_WORM_OVERWRITE_ATTEMPTED	0x800000000000000ull

#endif
