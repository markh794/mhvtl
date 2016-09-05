/*
 * vtl_u.h
 */
#define NETLINK_VTL	22

struct vtl_event {
	u32 tid;
	aligned_u64 sid;
	aligned_u64 serial_no;
	u32 cid;
	u32 state;
}

