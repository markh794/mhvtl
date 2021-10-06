
#ifndef	SPC_H
#define	SPC_H

/* Variables for simple, single initiator, SCSI Reservation system */

extern uint64_t SPR_Reservation_Key;
extern uint32_t SPR_Reservation_Generation;
extern uint8_t SPR_Reservation_Type;


uint8_t resp_spc_pro(uint8_t *cdb, struct mhvtl_ds *dbuf_p);
uint8_t resp_spc_pri(uint8_t *cdb, struct mhvtl_ds *dbuf_p);

uint8_t spc_illegal_op(struct scsi_cmd *cmd);
uint8_t spc_inquiry(struct scsi_cmd *cmd);
uint8_t spc_log_select(struct scsi_cmd *cmd);
uint8_t spc_log_sense(struct scsi_cmd *cmd);
uint8_t spc_mode_select(struct scsi_cmd *cmd);
uint8_t spc_mode_sense(struct scsi_cmd *cmd);
uint8_t spc_recv_diagnostics(struct scsi_cmd *cmd);
uint8_t spc_release(struct scsi_cmd *cmd);
uint8_t spc_request_sense(struct scsi_cmd *cmd);
uint8_t spc_reserve(struct scsi_cmd *cmd);
uint8_t spc_send_diagnostics(struct scsi_cmd *cmd);
uint8_t spc_tur(struct scsi_cmd *cmd);

#endif	/* SPC_H */
