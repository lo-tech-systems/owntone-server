
#ifndef __LISTENER_H__
#define __LISTENER_H__

enum listener_event_type
{
  /* The player has been started or stopped */
  LISTENER_PLAYER    = (1 << 0),
  /* The volume has been changed */
  LISTENER_VOLUME    = (1 << 2),
  /* Speaker status changes (enabled/disabled or verification status) */
  LISTENER_SPEAKER   = (1 << 3),
};

typedef void (*notify)(short event_mask, void *ctx);

/*
 * Registers the given callback function to the given event types.
 * This function is not thread safe. Listeners must be added once at startup.
 *
 * @param notify_cb Callback function (should be a non-blocking function,
 *        especially when the event is from the player)
 * @param event_mask Event mask, one or more of LISTENER_*
 * @param ctx Context will be passed to the notify callback
 * @return 0 on success, -1 on failure
 */
int
listener_add(notify notify_cb, short event_mask, void *ctx);

/*
 * Removes the given callback function
 * This function is not thread safe. Listeners must be removed once at shutdown.
 *
 * @param notify_cb Callback function
 * @return 0 on success, -1 if the callback was not registered
 */
int
listener_remove(notify notify_cb);

/*
 * Calls the callback function of the registered listeners listening for the
 * given type of event.
 *
 * @param event_mask Event mask, one or more of LISTENER_*
 *
 */
void
listener_notify(short event_mask);

#endif /* !__LISTENER_H__ */
