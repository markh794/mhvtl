/*
 * Replay of the SPC-3 persistent reservation state that LIO saves for
 * registrations made with APTPL (Activate Persist Through Power Loss).
 *
 * LIO writes <dbroot>/pr/aptpl_<unit serial> on every PR OUT that changes
 * the state of such a device, but never reads it back: whoever configures
 * the backstore has to write each registration to its
 * pr/res_aptpl_metadata attribute, before any fabric exports it. targetcli
 * does this in rtslib (StorageObject._config_pr_aptpl()).
 */
#ifndef _TCMU_APTPL_H_
#define _TCMU_APTPL_H_

/* Largest file LIO writes (PR_APTPL_BUF_LEN, include/target/target_core_base.h) */
#define APTPL_FILE_MAX		262144

/*
 * Called once per registration with the line for res_aptpl_metadata,
 * "key=value" pairs separated by commas. Returns 0 if it was applied.
 */
typedef int (*aptpl_record_fn)(const char *record, void *arg);

/*
 * Split the contents of an aptpl_<serial> file into registrations and pass
 * each to fn. Returns the number of registrations fn applied, or -1 if buf
 * is not in the format LIO writes (registrations before the error have
 * been passed to fn). A file holding "No Registrations or Reservations"
 * returns 0.
 */
int aptpl_replay(const char *buf, aptpl_record_fn fn, void *arg);

#endif /* _TCMU_APTPL_H_ */
