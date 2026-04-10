/*
 * db.h — compatibility shim for owntone-minimal
 *
 * The upstream airplay.c and raop.c include "db.h" and use three things:
 *
 *   1. struct db_queue_item + db_queue_fetch_byitemid + free_queue_item
 *      → satisfied by queue.h (our in-memory single-pipe queue)
 *
 *   2. db_speaker_save(device)
 *      → persists device->auth_key to owntone-settings.json so that AirPlay 2
 *        HomeKit pairing survives a server restart; implemented in db.c
 *
 *   3. CHAR_BIT (missing <limits.h> in upstream airplay.c)
 *      → pulled in here so the upstream file compiles unmodified
 *
 * No other db_* symbols are referenced by the compiled source files.
 */

#ifndef __DB_H__
#define __DB_H__

#include <limits.h>   /* CHAR_BIT */

#include "queue.h"    /* db_queue_item, db_queue_fetch_byitemid, free_queue_item */

/* Forward declaration — avoids pulling in outputs.h and creating a header
 * ordering problem for files that include db.h early (e.g. player.h). */
struct output_device;

/* Persist device->auth_key to the JSON config file so AirPlay pairing
 * survives a server restart.  Implemented in db.c. */
void db_speaker_save(struct output_device *device);

#endif /* !__DB_H__ */
