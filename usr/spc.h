
#ifndef	SPC_H
#define	SPC_H

/* Variables for simple, single initiator, SCSI Reservation system */

extern uint64_t SPR_Reservation_Key;
extern uint32_t SPR_Reservation_Generation;
extern uint8_t SPR_Reservation_Type;


int spc_inquiry(uint8_t *cdb, struct vtl_ds *ds, struct lu_phy_attr *lu);
int resp_spc_pro(uint8_t *cdb, struct vtl_ds *dbuf_p);
int resp_spc_pri(uint8_t *cdb, struct vtl_ds *dbuf_p);
void spc_request_sense(uint8_t *cdb, struct vtl_ds *dbuf_p);

#endif	/* SPC_H */
