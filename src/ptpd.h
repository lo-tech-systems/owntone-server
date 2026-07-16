#ifndef __PTPD_H__
#define __PTPD_H__

uint64_t
ptpd_clock_id_get(void);

int
ptpd_slave_add(uint32_t *slave_id, const char *addr);

void
ptpd_slave_remove(uint32_t slave_id);

// Looks for a shared airptpd daemon. If not found, binds priviliged ports 319
// and 320, so must be called before the server drops priviliges.
int
ptpd_find_or_bind(void);

int
ptpd_init(uint64_t clock_id_seed);

void
ptpd_deinit(void);

// Reads CLOCK_MONOTONIC, since that is the timescale libairptp distributes
// as PTP grandmaster (see src/libairptp/src/ptp_msg_handle.c). *frac is the
// fraction of a second as a 2^64 fixed-point value.
int
ptpd_network_time_get(uint64_t *secs, uint64_t *frac);

#endif /* !__PTPD_H__ */
