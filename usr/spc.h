
#ifndef	SPC_H
#define	SPC_H

/* Variables for simple, single initiator, SCSI Reservation system */

extern uint64_t SPR_Reservation_Key;
extern uint32_t SPR_Reservation_Generation;
extern uint8_t SPR_Reservation_Type;


int resp_spc_pro(uint8_t *cdb, struct vtl_ds *dbuf_p);
int resp_spc_pri(uint8_t *cdb, struct vtl_ds *dbuf_p);
void spc_request_sense_old(uint8_t *cdb, struct vtl_ds *dbuf_p);

int spc_illegal_op(struct scsi_cmd *cmd);
int spc_inquiry(struct scsi_cmd *cmd);
int spc_log_select(struct scsi_cmd *cmd);
int spc_log_sense(struct scsi_cmd *cmd);
int spc_mode_select(struct scsi_cmd *cmd);
int spc_mode_sense(struct scsi_cmd *cmd);
int spc_recv_diagnostics(struct scsi_cmd *cmd);
int spc_release(struct scsi_cmd *cmd);
int spc_request_sense(struct scsi_cmd *cmd);
int spc_reserve(struct scsi_cmd *cmd);
int spc_send_diagnostics(struct scsi_cmd *cmd);
int spc_tur(struct scsi_cmd *cmd);

#endif	/* SPC_H */
