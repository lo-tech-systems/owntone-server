/*
 * Copyright (C) 2015 Christian Meffert <christian.meffert@googlemail.com>
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <event2/event.h>

#include "library.h"
#include "commands.h"
#include "db.h"
#include "logger.h"
#include "misc.h"
#include "listener.h"

static struct commands_base *cmdbase;
static pthread_t tid_library;

struct event_base *evbase_lib;

// After being told by db that the library was updated through
// library_update_trigger(), wait 5 seconds before notifying listeners
// of LISTENER_DATABASE.
static struct timeval library_update_wait = { 5, 0 };
static struct event *updateev;

// Counts the number of changes made to the database between two DATABASE
// event notifications
static unsigned int deferred_update_notifications;
static short deferred_update_events;


/* --------------------------------- HELPERS -------------------------------- */

static bool
handle_deferred_update_notifications(void)
{
  time_t update_time;
  bool ret = (deferred_update_notifications > 0);

  if (ret)
    {
      deferred_update_notifications = 0;
      update_time = time(NULL);
      db_admin_setint64(DB_ADMIN_DB_UPDATE, (int64_t) update_time);
      db_admin_setint64(DB_ADMIN_DB_MODIFIED, (int64_t) update_time);
    }

  return ret;
}

static void
update_trigger_cb(int fd, short what, void *arg)
{
  if (handle_deferred_update_notifications())
    {
      listener_notify(deferred_update_events);
      deferred_update_events = 0;
    }
}

static enum command_state
update_trigger(void *arg, int *retval)
{
  short *events = arg;

  ++deferred_update_notifications;
  deferred_update_events |= *events;

  evtimer_add(updateev, &library_update_wait);

  *retval = 0;
  return COMMAND_END;
}


/* ----------------------- LIBRARY EXTERNAL INTERFACE ---------------------- */

bool
library_is_scanning(void)
{
  return false;
}

void
library_update_trigger(short update_events)
{
  short *events;
  int ret;

  pthread_t current_thread = pthread_self();
  if (pthread_equal(current_thread, tid_library))
    {
      update_trigger(&update_events, &ret);
    }
  else
    {
      events = malloc(sizeof(short));
      *events = update_events;
      commands_exec_async(cmdbase, update_trigger, events);
    }
}

static void *
library(void *arg)
{
  int ret;

  thread_setname("library");

  ret = db_perthread_init();
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_LIB, "Error: DB init failed\n");
      pthread_exit(NULL);
    }

  event_base_dispatch(evbase_lib);

  db_perthread_deinit();

  pthread_exit(NULL);
}

/* Thread: main */
int
library_init(void)
{
  CHECK_NULL(L_LIB, evbase_lib = event_base_new());
  CHECK_NULL(L_LIB, updateev = evtimer_new(evbase_lib, update_trigger_cb, NULL));
  CHECK_NULL(L_LIB, cmdbase = commands_base_new(evbase_lib, NULL));

  CHECK_ERR(L_LIB, pthread_create(&tid_library, NULL, library, NULL));

  return 0;
}

/* Thread: main */
void
library_deinit(void)
{
  int ret;

  commands_base_destroy(cmdbase);

  ret = pthread_join(tid_library, NULL);
  if (ret != 0)
    {
      DPRINTF(E_FATAL, L_LIB, "Could not join library thread: %s\n", strerror(errno));
      return;
    }

  event_free(updateev);
  event_base_free(evbase_lib);
}
