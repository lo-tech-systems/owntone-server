/*
 * Minimal db.h — types-only stub for the pipe-only OwnTone build.
 *
 * All queue operations are now in queue.h / queue.c.
 * The SQLite db.c module has been removed.
 */

#ifndef __DB_H__
#define __DB_H__

#include "queue.h"   /* enum data_kind, enum media_kind, struct db_queue_item, queue API */

#endif /* !__DB_H__ */
