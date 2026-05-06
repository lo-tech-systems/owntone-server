/*
 * Copyright (C) 2016 Espen Jürgensen <espenjurgensen@gmail.com>
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

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <inttypes.h>

#include <event2/event.h>

#include "logger.h"
#include "misc.h"
#include "transcode.h"
#include "worker.h"
#include "outputs.h"
#include "owntone_config.h"

extern struct output_definition output_raop;
extern struct output_definition output_airplay;

/* From player.c */
extern struct event_base *evbase_player;

// Must be in sync with enum output_types
static struct output_definition *outputs[] = {
    &output_raop,
    &output_airplay,
    NULL
};

// Default volume (must be from 0 - 100)
#define OUTPUTS_DEFAULT_VOLUME 50

// When we stop, we keep the outputs open for a while, just in case we are
// actually just restarting. This timeout determines how long we wait before
// full stop.
// (value is in seconds)
#define OUTPUTS_STOP_TIMEOUT 10

#define OUTPUTS_MAX_CALLBACKS 64

struct outputs_callback_register
{
  output_status_cb cb;
  struct output_device *device;

  // We have received the callback with the result from the backend
  bool ready;

  // We store a device_id to avoid the risk of dangling device pointer
  uint64_t device_id;
  enum output_device_state state;
};

struct output_quality_subscription
{
  int count;
  struct media_quality quality;
  struct encode_ctx *encode_ctx;
};

// Buffer used to pass data to the backends
static struct output_buffer output_buffer;

static struct output_device *outputs_device_list;
static int outputs_master_volume;
static uint64_t outputs_buffer_duration_ms;

static bool outputs_exclusive_mode;

static struct outputs_callback_register outputs_cb_register[OUTPUTS_MAX_CALLBACKS];
static struct event *outputs_deferredev;
static struct timeval outputs_stop_timeout = { OUTPUTS_STOP_TIMEOUT, 0 };

// Last element is a zero terminator
static struct output_quality_subscription output_quality_subscriptions[OUTPUTS_MAX_QUALITY_SUBSCRIPTIONS + 1];
static bool outputs_got_new_subscription;

struct output_group
{
  char *id;
  char *name;
  uint64_t leader_id;
  uint64_t *member_ids;
  size_t members_count;
  bool leader_known;
  struct output_group *next;
};

static struct output_group *outputs_group_list;


/* ----------------------------- Mode helpers ------------------------------- */

const char *
output_mode_to_string(enum output_mode mode)
{
  switch (mode)
    {
      case OUTPUT_MODE_RAOP:     return "raop";
      case OUTPUT_MODE_AIRPLAY2: return "airplay2";
      default:                   return "auto";
    }
}

enum output_mode
output_mode_from_string(const char *str)
{
  if (!str)
    return OUTPUT_MODE_AUTO;
  if (strcmp(str, "raop") == 0)
    return OUTPUT_MODE_RAOP;
  if (strcmp(str, "airplay2") == 0)
    return OUTPUT_MODE_AIRPLAY2;
  return OUTPUT_MODE_AUTO;
}

/* ------------------------------- MISC HELPERS ----------------------------- */

static void
output_group_free_all(void)
{
  struct output_group *group;
  struct output_group *next;

  for (group = outputs_group_list; group; group = next)
    {
      next = group->next;
      free(group->id);
      free(group->name);
      free(group->member_ids);
      free(group);
    }

  outputs_group_list = NULL;
}

static struct output_group *
output_group_find_or_add(const char *group_id)
{
  struct output_group *group;

  for (group = outputs_group_list; group; group = group->next)
    {
      if (strcmp(group->id, group_id) == 0)
        return group;
    }

  CHECK_NULL(L_PLAYER, group = calloc(1, sizeof(struct output_group)));
  group->id = safe_strdup(group_id);
  group->next = outputs_group_list;
  outputs_group_list = group;

  return group;
}

static void
output_group_member_add(struct output_group *group, uint64_t device_id)
{
  uint64_t *member_ids;

  CHECK_NULL(L_PLAYER, member_ids = realloc(group->member_ids, (group->members_count + 1) * sizeof(uint64_t)));
  group->member_ids = member_ids;
  group->member_ids[group->members_count++] = device_id;
}

static void
device_group_fields_clear(struct output_device *device)
{
  free(device->display_name);
  device->display_name = NULL;

  free(device->group_id);
  device->group_id = NULL;

  free(device->group_name);
  device->group_name = NULL;

  free(device->raw_gid);
  device->raw_gid = NULL;

  device->group_leader_id = 0;
  device->is_grouped = 0;
  device->is_group_leader = 0;
  device->is_group_hidden = 0;
  device->is_tv_proxy_group = 0;
  device->leader_hint_gcgl = 0;
  device->leader_hint_igl = 0;
}

static void
device_group_fields_copy(struct output_device *dst, struct output_device *src)
{
  device_group_fields_clear(dst);

  dst->display_name = safe_strdup(src->display_name);
  dst->group_id = safe_strdup(src->group_id);
  dst->group_name = safe_strdup(src->group_name);
  dst->raw_gid = safe_strdup(src->raw_gid);
  dst->group_leader_id = src->group_leader_id;
  dst->is_grouped = src->is_grouped;
  dst->is_group_leader = src->is_group_leader;
  dst->is_group_hidden = src->is_group_hidden;
  dst->is_tv_proxy_group = src->is_tv_proxy_group;
  dst->leader_hint_gcgl = src->leader_hint_gcgl;
  dst->leader_hint_igl = src->leader_hint_igl;
}

static void
output_group_refresh(void)
{
  struct output_device *device;
  struct output_device *candidate;
  struct output_device *tv_device;
  struct output_device *tv_candidate;
  struct output_group *group;
  const char *log_name;
  const char *shared_raw_gid;
  int leaders;

  // Reset any TV proxy state set synthetically during a previous refresh.
  // The Apple TV proxy leader's group fields are computed (not from mDNS), so
  // they must be cleared before rebuilding. HomePod members only need their
  // is_tv_proxy_group flag cleared; their group_id comes from mDNS and stays.
  for (device = outputs_device_list; device; device = device->next)
    {
      candidate = device->candidate_airplay2;
      if (!candidate)
        continue;

      if (candidate->is_tv_proxy_group && candidate->is_group_leader)
        {
          DPRINTF(E_DBG, L_PLAYER,
                  "output_group_refresh: clearing proxy leader candidate=%p canonical id=%" PRIu64 " name='%s' group_id=%s group_name='%s' raw_gid=%s\n",
                  candidate, device->id, device->name ? device->name : "(null)",
                  candidate->group_id ? candidate->group_id : "(null)",
                  candidate->group_name ? candidate->group_name : "(null)",
                  candidate->raw_gid ? candidate->raw_gid : "(null)");
          // This candidate was promoted as Apple TV proxy leader; clear the
          // synthetically assigned group fields but preserve raw_gid.
          free(candidate->group_id);
          candidate->group_id = NULL;
          free(candidate->group_name);
          candidate->group_name = NULL;
          candidate->group_leader_id = 0;
          candidate->is_grouped = 0;
          candidate->is_group_leader = 0;
          candidate->is_group_hidden = 0;
        }
      candidate->is_tv_proxy_group = 0;
    }

  output_group_free_all();

  for (device = outputs_device_list; device; device = device->next)
    {
      candidate = device->candidate_airplay2;

      // Stereo/group discovery is only tracked for AirPlay 2-capable devices.
      if (!candidate || !candidate->group_id)
        continue;

      DPRINTF(E_DBG, L_PLAYER,
              "Considering AirPlay group candidate: canonical id=%" PRIu64 ", canonical='%s', candidate='%s', type=%s, gid=%s, name='%s', leader=%u, hidden=%u, leader_hint_gcgl=%u, leader_hint_igl=%u\n",
              device->id, device->name ? device->name : "(null)",
              candidate->name ? candidate->name : "(null)",
              candidate->type_name ? candidate->type_name : "(unknown)",
              candidate->group_id, candidate->group_name ? candidate->group_name : "(null)",
              candidate->is_group_leader, candidate->is_group_hidden,
              candidate->leader_hint_gcgl, candidate->leader_hint_igl);

      group = output_group_find_or_add(candidate->group_id);
      if (!group->name && candidate->group_name)
        group->name = safe_strdup(candidate->group_name);
      if (candidate->is_group_leader && !group->leader_known)
        {
          group->leader_id = device->id;
          group->leader_known = true;
        }

      output_group_member_add(group, device->id);
    }

  // For each HomePod group with no mDNS leader (both HomePods behind an Apple
  // TV report igl=0), look for an Apple TV whose raw mDNS gid matches the
  // members' shared raw gid and which carries igl=1. Promote it as the visible
  // proxy leader for the group.
  for (group = outputs_group_list; group; group = group->next)
    {
      if (group->leader_known)
        continue;

      // Find the raw_gid shared by members of this leaderless group.
      shared_raw_gid = NULL;
      for (device = outputs_device_list; device; device = device->next)
        {
          candidate = device->candidate_airplay2;
          if (!candidate || !candidate->group_id || strcmp(candidate->group_id, group->id) != 0)
            continue;
          if (candidate->raw_gid)
            {
              shared_raw_gid = candidate->raw_gid;
              break;
            }
        }

      if (!shared_raw_gid)
        continue;

      // Find an Apple TV whose raw_gid matches and which has the leader hint.
      tv_device = NULL;
      for (device = outputs_device_list; device; device = device->next)
        {
          candidate = device->candidate_airplay2;
          if (!candidate)
            continue;
          // Exclude devices already in this group.
          if (candidate->group_id && strcmp(candidate->group_id, group->id) == 0)
            continue;
          if (!candidate->raw_gid || strcmp(candidate->raw_gid, shared_raw_gid) != 0)
            continue;
          if (!candidate->leader_hint_igl)
            continue;
          tv_device = device;
          break;
        }

      if (!tv_device)
        continue;

      tv_candidate = tv_device->candidate_airplay2;

      // Promote the Apple TV as the proxy leader for this HomePod group.
      DPRINTF(E_DBG, L_PLAYER,
              "output_group_refresh: promoting proxy leader tv_device=%p id=%" PRIu64 " name='%s' candidate=%p raw_gid=%s -> group_id=%s group_name='%s'\n",
              tv_device, tv_device->id, tv_device->name ? tv_device->name : "(null)",
              tv_candidate, tv_candidate->raw_gid ? tv_candidate->raw_gid : "(null)",
              group->id, group->name ? group->name : "(unknown)");
      tv_candidate->group_id       = safe_strdup(group->id);
      tv_candidate->group_name     = safe_strdup(group->name);
      tv_candidate->group_leader_id = tv_device->id;
      tv_candidate->is_grouped     = 1;
      tv_candidate->is_group_leader = 1;
      tv_candidate->is_group_hidden = 0;
      tv_candidate->is_tv_proxy_group = 1;

      group->leader_id    = tv_device->id;
      group->leader_known = true;
      output_group_member_add(group, tv_device->id);

      // Tag HomePod members so the player can apply connection mode gating.
      for (device = outputs_device_list; device; device = device->next)
        {
          candidate = device->candidate_airplay2;
          if (!candidate || !candidate->group_id || strcmp(candidate->group_id, group->id) != 0)
            continue;
          if (device->id != tv_device->id)
            candidate->is_tv_proxy_group = 1;
        }

      DPRINTF(E_INFO, L_PLAYER,
              "AirPlay TV proxy group: Apple TV '%s' (id=%" PRIu64 ") linked as leader for HomePod group gid=%s, name='%s'\n",
              tv_device->name, tv_device->id, group->id, group->name ? group->name : "(unknown)");
    }

  for (group = outputs_group_list; group; group = group->next)
    {
      leaders = 0;
      for (device = outputs_device_list; device; device = device->next)
        {
          candidate = device->candidate_airplay2;
          if (!candidate || !candidate->group_id || strcmp(candidate->group_id, group->id) != 0)
            continue;
          if (candidate->is_group_leader)
            leaders++;
        }

      if (leaders > 1)
        DPRINTF(E_WARN, L_PLAYER, "AirPlay stereo group gid=%s has multiple leader candidates (%d)\n",
                group->id, leaders);
      else if (!group->leader_known)
        DPRINTF(E_WARN, L_PLAYER, "AirPlay stereo group gid=%s has no leader candidate\n", group->id);

      if (group->members_count < 2)
        DPRINTF(E_WARN, L_PLAYER, "AirPlay stereo group gid=%s is incomplete: %zu member(s) currently present\n",
                group->id, group->members_count);

      log_name = group->name ? group->name : "(unknown)";
      DPRINTF(E_INFO, L_PLAYER, "AirPlay stereo group state updated: gid=%s, name='%s', members=%zu, leader_id=%" PRIu64 "\n",
              group->id, log_name, group->members_count, group->leader_known ? group->leader_id : 0);
    }
}

static struct output_device *
group_meta_device(struct output_device *device)
{
  // Stereo grouping is derived from the AirPlay 2 candidate even when another
  // backend is currently active, so prefer that candidate as the metadata
  // source when it is available.
  if (device && device->candidate_airplay2 && device->candidate_airplay2->group_id)
    return device->candidate_airplay2;

  return device;
}

bool
outputs_device_is_hidden(struct output_device *device)
{
  struct output_device *meta = group_meta_device(device);

  return meta && meta->is_group_hidden;
}

bool
outputs_device_is_stereo_leader(struct output_device *device)
{
  struct output_device *meta = group_meta_device(device);

  return meta && meta->is_grouped && meta->is_group_leader;
}

bool
outputs_device_is_tv_proxy_group(struct output_device *device)
{
  struct output_device *meta = group_meta_device(device);

  return meta && meta->is_tv_proxy_group;
}

const char *
outputs_device_display_name(struct output_device *device)
{
  struct output_device *meta = group_meta_device(device);

  if (meta && meta->display_name)
    return meta->display_name;

  return device ? device->name : NULL;
}

const char *
outputs_device_group_id(struct output_device *device)
{
  struct output_device *meta = group_meta_device(device);

  return meta ? meta->group_id : NULL;
}

uint32_t
outputs_device_supported_modes(struct output_device *device)
{
  // Grouped stereo outputs are intentionally exposed as AirPlay 2-only, even
  // though the underlying HomePods may still advertise RAOP candidates.
  if (outputs_device_is_stereo_leader(device))
    return OUTPUT_MODE_AIRPLAY2;

  return device ? device->supported_modes : 0;
}

enum output_mode
outputs_device_display_mode(struct output_device *device)
{
  if (outputs_device_is_stereo_leader(device))
    return OUTPUT_MODE_AIRPLAY2;

  return device ? device->preferred_mode : OUTPUT_MODE_AUTO;
}

// Frees a candidate device struct and its backend-specific data. Candidates
// own their extra_device_info; canonical devices borrow it.
static void
candidate_free(struct output_device *candidate)
{
  if (!candidate)
    return;

  DPRINTF(E_DBG, L_PLAYER,
          "candidate_free: candidate=%p id=%" PRIu64 " type=%s name='%s' display='%s' group_id=%s group_name='%s' raw_gid=%s extra=%p\n",
          candidate, candidate->id, candidate->type_name ? candidate->type_name : "(null)",
          candidate->name ? candidate->name : "(null)",
          candidate->display_name ? candidate->display_name : "(null)",
          candidate->group_id ? candidate->group_id : "(null)",
          candidate->group_name ? candidate->group_name : "(null)",
          candidate->raw_gid ? candidate->raw_gid : "(null)",
          candidate->extra_device_info);

  if (candidate->extra_device_info && outputs[candidate->type]->device_free_extra)
    outputs[candidate->type]->device_free_extra(candidate);

  free(candidate->name);
  free(candidate->display_name);
  free(candidate->group_id);
  free(candidate->group_name);
  free(candidate->raw_gid);
  free(candidate->auth_key);
  free(candidate->v4_address);
  free(candidate->v6_address);
  free(candidate);
}

// Returns a pointer to the candidate slot for the given output type, or NULL
// for types that have no dedicated slot.
static struct output_device **
candidate_slot(struct output_device *canonical, enum output_types type)
{
  if (type == OUTPUT_TYPE_RAOP)
    return &canonical->candidate_raop;
  if (type == OUTPUT_TYPE_AIRPLAY)
    return &canonical->candidate_airplay2;
  return NULL;
}

// Recomputes supported_modes from whichever candidate slots are populated.
static void
supported_modes_recompute(struct output_device *device)
{
  device->supported_modes = 0;
  if (device->candidate_raop)
    device->supported_modes |= OUTPUT_MODE_RAOP;
  if (device->candidate_airplay2)
    device->supported_modes |= OUTPUT_MODE_AIRPLAY2;
}

// Resolves the effective output type from preferred_mode and available
// candidates, falling back to priority-based selection for AUTO.
static enum output_types
effective_type_resolve(struct output_device *device)
{
  if (outputs_device_is_stereo_leader(device) && device->candidate_airplay2)
    return OUTPUT_TYPE_AIRPLAY;

  if (device->preferred_mode == OUTPUT_MODE_RAOP && device->candidate_raop)
    return OUTPUT_TYPE_RAOP;
  if (device->preferred_mode == OUTPUT_MODE_AIRPLAY2 && device->candidate_airplay2)
    return OUTPUT_TYPE_AIRPLAY;

  // AUTO or preferred protocol unavailable: highest priority wins
  if (device->candidate_airplay2 &&
      (!device->candidate_raop ||
       outputs[OUTPUT_TYPE_AIRPLAY]->priority <= outputs[OUTPUT_TYPE_RAOP]->priority))
    return OUTPUT_TYPE_AIRPLAY;

  return OUTPUT_TYPE_RAOP;
}

// Copies the active candidate's transport fields into the canonical device.
// extra_device_info is borrowed (not owned) by the canonical device and is
// only updated when there is no active session.
static void
effective_candidate_apply(struct output_device *canonical)
{
  struct output_device *candidate;
  enum output_types etype;

  etype = effective_type_resolve(canonical);
  candidate = (etype == OUTPUT_TYPE_RAOP) ? canonical->candidate_raop
                                          : canonical->candidate_airplay2;
  if (!candidate)
    return;

  // Log backend switches before overwriting canonical->type (skip on first apply
  // where canonical->name is NULL because it has not been set yet)
  if (canonical->name && canonical->type != etype)
    DPRINTF(E_INFO, L_PLAYER, "Switching '%s' output backend: %s -> %s\n",
            canonical->name, outputs_name(canonical->type), candidate->type_name);

  // Log when the user's preferred protocol is unavailable and we fall back
  if (canonical->preferred_mode != OUTPUT_MODE_AUTO)
    {
      bool preferred_unavailable =
        (canonical->preferred_mode == OUTPUT_MODE_RAOP    && !canonical->candidate_raop) ||
        (canonical->preferred_mode == OUTPUT_MODE_AIRPLAY2 && !canonical->candidate_airplay2);
      if (preferred_unavailable)
        DPRINTF(E_DBG, L_PLAYER, "Preferred protocol for '%s' unavailable, using %s\n",
                candidate->name, candidate->type_name);
    }

  canonical->type      = candidate->type;
  canonical->type_name = candidate->type_name;

  canonical->has_password  = candidate->has_password;
  canonical->password      = candidate->password;
  canonical->has_video     = candidate->has_video;
  canonical->requires_auth = candidate->requires_auth;
  canonical->v6_disabled   = candidate->v6_disabled;
  canonical->quality       = candidate->quality;
  canonical->selected_format = candidate->selected_format;
  canonical->default_format  = candidate->default_format;
  canonical->supported_formats = candidate->supported_formats;
  canonical->max_volume    = candidate->max_volume;
  canonical->relvol        = candidate->relvol;
  canonical->resurrect     = candidate->resurrect;

  // Borrow extra_device_info only when the session is gone (safe to switch)
  if (!canonical->session)
    canonical->extra_device_info = candidate->extra_device_info;

  // Update canonical's own address copies
  free(canonical->v4_address);
  canonical->v4_address = candidate->v4_address ? strdup(candidate->v4_address) : NULL;
  canonical->v4_port    = candidate->v4_port;

  free(canonical->v6_address);
  canonical->v6_address = candidate->v6_address ? strdup(candidate->v6_address) : NULL;
  canonical->v6_port    = candidate->v6_port;

  // Keep name in sync with active candidate
  free(canonical->name);
  canonical->name = strdup(candidate->name);
  device_group_fields_copy(canonical, candidate);
}

// Stores a new candidate in the canonical device's appropriate slot. For a
// same-type re-advertisement while a session is active the existing candidate
// is updated in-place (so extra_device_info remains valid for the session);
// otherwise the old candidate is replaced.
static void
candidate_store(struct output_device *canonical, struct output_device *new_cand)
{
  struct output_device **slot = candidate_slot(canonical, new_cand->type);
  if (!slot)
    {
      DPRINTF(E_LOG, L_PLAYER, "Bug! candidate_store called for unsupported type %d\n", new_cand->type);
      candidate_free(new_cand);
      return;
    }

  if (*slot && canonical->session && canonical->type == new_cand->type)
    {
      // Active session is using this candidate's extra_device_info. Update
      // the existing candidate in-place and discard the new one.
      struct output_device *existing = *slot;

      if (new_cand->v4_address)
        {
          free(existing->v4_address);
          existing->v4_address = strdup(new_cand->v4_address);
          existing->v4_port    = new_cand->v4_port;
        }
      if (new_cand->v6_address)
        {
          free(existing->v6_address);
          existing->v6_address = strdup(new_cand->v6_address);
          existing->v6_port    = new_cand->v6_port;
        }
      existing->has_password       = new_cand->has_password;
      existing->password           = new_cand->password;
      existing->supported_formats  = new_cand->supported_formats;
      device_group_fields_copy(existing, new_cand);

      candidate_free(new_cand);
      return;
    }

  candidate_free(*slot); // Free old candidate if any (no-op when NULL)
  *slot = new_cand;
}

static output_status_cb
callback_get(struct output_device *device)
{
  int callback_id;

  for (callback_id = 0; callback_id < ARRAY_SIZE(outputs_cb_register); callback_id++)
    {
      if (outputs_cb_register[callback_id].device == device)
	return outputs_cb_register[callback_id].cb;
    }

  return NULL;
}

static void
callback_remove(struct output_device *device)
{
  int callback_id;

  if (!device)
    return;

  for (callback_id = 0; callback_id < ARRAY_SIZE(outputs_cb_register); callback_id++)
    {
      if (outputs_cb_register[callback_id].device == device)
	{
	  DPRINTF(E_DBG, L_PLAYER, "Removing callback to %p, id %d\n", outputs_cb_register[callback_id].cb, callback_id);
	  memset(&outputs_cb_register[callback_id], 0, sizeof(struct outputs_callback_register));
	}
    }
}

static int
callback_add(struct output_device *device, output_status_cb cb)
{
  int callback_id;

  if (!cb)
    return -1;

  // We will replace any previously registered callbacks, since that's what the
  // player expects
  callback_remove(device);

  // Find a free slot in the queue
  for (callback_id = 0; callback_id < ARRAY_SIZE(outputs_cb_register); callback_id++)
    {
      if (outputs_cb_register[callback_id].cb == NULL)
	break;
    }

  if (callback_id == ARRAY_SIZE(outputs_cb_register))
    {
      DPRINTF(E_LOG, L_PLAYER, "Output callback queue is full! (size is %d)\n", OUTPUTS_MAX_CALLBACKS);
      return -1;
    }

  outputs_cb_register[callback_id].cb = cb;
  outputs_cb_register[callback_id].device = device; // Don't dereference this later, it might become invalid!

  DPRINTF(E_DBG, L_PLAYER, "Registered callback to %p with id %d (device %p, %s)\n", cb, callback_id, device, device->name);

  int active = 0;
  for (int i = 0; i < ARRAY_SIZE(outputs_cb_register); i++)
    if (outputs_cb_register[i].cb)
      active++;

  DPRINTF(E_DBG, L_PLAYER, "Number of active callbacks: %d\n", active);

  return callback_id;
};

static void
deferred_cb(int fd, short what, void *arg)
{
  struct output_device *device;
  output_status_cb cb;
  enum output_device_state state;
  int callback_id;

  for (callback_id = 0; callback_id < ARRAY_SIZE(outputs_cb_register); callback_id++)
    {
      if (outputs_cb_register[callback_id].ready)
	{
	  // Must copy before making callback, since you never know what the
	  // callback might result in (could call back in)
	  cb = outputs_cb_register[callback_id].cb;
	  state = outputs_cb_register[callback_id].state;

	  // Will be NULL if the device has disappeared
	  device = outputs_device_get(outputs_cb_register[callback_id].device_id);

	  memset(&outputs_cb_register[callback_id], 0, sizeof(struct outputs_callback_register));

	  // The device has left the building (stopped/failed), and the backend
	  // is not using it any more
	  if (device && !device->advertised && !device->session)
	    {
	      outputs_device_remove(device);
	      device = NULL;
	    }
	  else if (device)
	    {
	      device->state = state;

	      // Handle deferred candidate cleanup after a session ends.
	      if (!device->session)
		{
		  struct output_device **active_slot =
		    (device->type == OUTPUT_TYPE_RAOP) ? &device->candidate_raop
		                                       : &device->candidate_airplay2;
		  struct output_device *active_cand = *active_slot;

		  // Case 1: The active candidate disappeared while a session was
		  // running. Its slot is still set (kept alive for extra_device_info)
		  // but the candidate has no addresses. Free it and switch now.
		  if (active_cand && !active_cand->v4_address && !active_cand->v6_address)
		    {
		      DPRINTF(E_INFO, L_PLAYER, "Freeing gone %s candidate for '%s' after session end\n",
			      active_cand->type_name, device->name);
		      device->extra_device_info = NULL; // borrowed reference; candidate owns it
		      candidate_free(active_cand);
		      *active_slot = NULL;
		      supported_modes_recompute(device);
		      if (device->candidate_raop || device->candidate_airplay2)
			effective_candidate_apply(device);
		      else
			device->advertised = 0;
		    }
		  // Case 2: The active slot was already cleared through another path.
		  else if (!active_cand && (device->candidate_raop || device->candidate_airplay2))
		    {
		      DPRINTF(E_INFO, L_PLAYER, "Switching '%s' to available protocol after previous backend disappeared\n", device->name);
		      effective_candidate_apply(device);
		    }
		}
	    }

	  DPRINTF(E_DBG, L_PLAYER, "Making deferred callback to %p, id was %d\n", cb, callback_id);

	  cb(device, state);
	}
    }

  for (int i = 0; i < ARRAY_SIZE(outputs_cb_register); i++)
    {
      if (outputs_cb_register[i].cb)
	DPRINTF(E_DBG, L_PLAYER, "%d. Active callback: %p\n", i, outputs_cb_register[i].cb);
    }
}

static void
stop_timer_cb(int fd, short what, void *arg)
{
  struct output_device *device = arg;
  output_status_cb cb = callback_get(device);

  outputs_device_stop(device, cb);
}

static void
device_stop_cb(struct output_device *device, enum output_device_state state)
{
  if (device)
    device->state = state;

  if (state == OUTPUT_STATE_FAILED)
    DPRINTF(E_WARN, L_PLAYER, "Failed to stop device\n");
  else
    DPRINTF(E_INFO, L_PLAYER, "Device stopped properly\n");
}

static enum transcode_profile
quality_to_xcode(struct media_quality *quality)
{
  /* Pipe input is always 16-bit PCM; other depths are not supported. */
  if (quality->bits_per_sample != 16)
    DPRINTF(E_WARN, L_PLAYER, "Unsupported bit depth %d, falling back to 16-bit encoding\n", quality->bits_per_sample);

  return XCODE_PCM16;
}

static int
encoding_reset(struct media_quality *quality)
{
  struct transcode_encode_setup_args encode_args = { 0 };
  struct output_quality_subscription *subscription;
  enum transcode_profile profile;
  int i;

  profile = quality_to_xcode(quality);
  encode_args.src_ctx = transcode_decode_setup_raw(profile, quality);
  if (!encode_args.src_ctx)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not create subscription decoding context (profile %d)\n", profile);
      return -1;
    }

  for (i = 0; output_quality_subscriptions[i].count > 0; i++)
    {
      subscription = &output_quality_subscriptions[i]; // Just for short-hand

      transcode_encode_cleanup(&subscription->encode_ctx); // Will also point the ctx to NULL

      if (quality_is_equal(quality, &subscription->quality))
	continue; // No resampling required

      encode_args.profile = quality_to_xcode(&subscription->quality);
      encode_args.quality = &subscription->quality;
      subscription->encode_ctx = transcode_encode_setup(encode_args);
    }

  transcode_decode_cleanup(&encode_args.src_ctx);

  return 0;
}

static void
buffer_fill(struct output_buffer *obuf, void *buf, size_t bufsize, struct media_quality *quality, int nsamples, struct timespec *pts)
{
  transcode_frame *frame;
  int ret;
  int i;
  int n;

  obuf->pts = *pts;

  // The resampling/encoding (transcode) contexts work for a given input quality,
  // so if the quality changes we need to reset the contexts. We also do that if
  // we have received a subscription for a new quality.
  if (!quality_is_equal(quality, &obuf->data[0].quality) || outputs_got_new_subscription)
    {
      encoding_reset(quality);
      outputs_got_new_subscription = false;
    }

  // The first element of the output_buffer is always just the raw input data
  // TODO can we avoid the copy below? we can't use evbuffer_add_buffer_reference,
  // because then the outputs can't use it and we would need to copy there instead
  evbuffer_add(obuf->data[0].evbuf, buf, bufsize);
  obuf->data[0].buffer = buf;
  obuf->data[0].bufsize = bufsize;
  obuf->data[0].quality = *quality;
  obuf->data[0].samples = nsamples;

  for (i = 0, n = 1; output_quality_subscriptions[i].count > 0; i++)
    {
      if (quality_is_equal(&output_quality_subscriptions[i].quality, quality))
	continue; // Skip, no resampling required and we have the data in element 0

      if (!output_quality_subscriptions[i].encode_ctx)
	continue;

      frame = transcode_frame_new(buf, bufsize, nsamples, quality);
      if (!frame)
	continue;

      ret = transcode_encode(obuf->data[n].evbuf, output_quality_subscriptions[i].encode_ctx, frame, 0);
      transcode_frame_free(frame);
      if (ret < 0)
	continue;

      obuf->data[n].buffer  = evbuffer_pullup(obuf->data[n].evbuf, -1);
      obuf->data[n].bufsize = evbuffer_get_length(obuf->data[n].evbuf);
      obuf->data[n].quality = output_quality_subscriptions[i].quality;
      obuf->data[n].samples = BTOS(obuf->data[n].bufsize, obuf->data[n].quality.bits_per_sample, obuf->data[n].quality.channels);
      n++;
    }
}

static void
buffer_drain(struct output_buffer *obuf)
{
  int i;

  for (i = 0; obuf->data[i].buffer; i++)
    {
      evbuffer_drain(obuf->data[i].evbuf, obuf->data[i].bufsize);
      obuf->data[i].buffer  = NULL;
      obuf->data[i].bufsize = 0;
      // We don't reset quality and samples, would be a waste of time
    }
}

static struct output_buffer *
buffer_copy(struct output_buffer *obuf)
{
  struct output_buffer *copy;
  int i;

  if (!obuf)
    return NULL;

  CHECK_NULL(L_PLAYER, copy = malloc(sizeof(struct output_buffer)));

  memcpy(copy, obuf, sizeof(struct output_buffer));

  for (i = 0; obuf->data[i].buffer; i++)
    {
      CHECK_NULL(L_PLAYER, copy->data[i].evbuf = evbuffer_new());
      evbuffer_add(copy->data[i].evbuf, obuf->data[i].buffer, obuf->data[i].bufsize);
      copy->data[i].buffer = evbuffer_pullup(copy->data[i].evbuf, -1);
    }

  return copy;
}

static void
buffer_free(struct output_buffer *obuf)
{
  int i;

  if (!obuf)
    return;

  for (i = 0; obuf->data[i].buffer; i++)
    evbuffer_free(obuf->data[i].evbuf);

  free(obuf);
}

static void
device_list_sort(void)
{
  struct output_device *device;
  struct output_device *next;
  struct output_device *prev;
  int swaps;

  // Swap sorting since even the most inefficient sorting should do fine here
  do
    {
      swaps = 0;
      prev = NULL;
      for (device = outputs_device_list; device && device->next; device = device->next)
	{
	  next = device->next;
	  if ( (outputs_priority(device) > outputs_priority(next)) ||
	       (outputs_priority(device) == outputs_priority(next) && strcasecmp(device->name, next->name) > 0) )
	    {
	      if (device == outputs_device_list)
		outputs_device_list = next;
	      if (prev)
		prev->next = next;

	      device->next = next->next;
	      next->next = device;
	      swaps++;
	    }
	  prev = device;
	}
    }
  while (swaps > 0);
}

// Convenience function
static inline int
device_state_update(struct output_device *device, int ret)
{
  if (ret < 0)
    device->state = OUTPUT_STATE_FAILED;

  return ret;
}

static void
metadata_cb_send(int fd, short what, void *arg)
{
  struct output_metadata *metadata = arg;
  int ret;

  event_free(metadata->ev);
  metadata->ev = NULL;

  ret = metadata->finalize_cb(metadata);
  if (ret < 0)
    return;

  outputs[metadata->type]->metadata_send(metadata);
}

// *** Worker thread ***
static void
metadata_cb_prepare(void *arg)
{
  struct output_metadata *metadata = *((struct output_metadata **)arg);

  metadata->priv = outputs[metadata->type]->metadata_prepare(metadata);
  if (!metadata->priv)
    {
      event_free(metadata->ev);
      free(metadata);
      return;
    }

  // Metadata is prepared, let the player thread do the actual sending
  event_active(metadata->ev, 0, 0);
}

static void
metadata_free(struct output_metadata *metadata)
{
  if (!metadata)
    return;
  if (metadata->ev)
    event_free(metadata->ev);
  free(metadata);
}

static void
metadata_send(enum output_types type, uint32_t item_id, bool startup, output_metadata_finalize_cb cb)
{
  struct output_metadata *metadata;

  CHECK_NULL(L_PLAYER, metadata = calloc(1, sizeof(struct output_metadata)));

  metadata->type        = type;
  metadata->item_id     = item_id;
  metadata->startup     = startup;
  metadata->finalize_cb = cb;

  metadata->ev = event_new(evbase_player, -1, 0, metadata_cb_send, metadata);

  if (outputs[type]->metadata_prepare)
    worker_execute(metadata_cb_prepare, &metadata, sizeof(struct output_metadata *), 0);
  else
    outputs[type]->metadata_send(metadata);
}


/* ----------------------------- Volume helpers ----------------------------- */

static int
rel_to_vol(int relvol, int master_volume)
{
  float vol;

  if (relvol == 100)
    return master_volume;

  vol = ((float)relvol * (float)master_volume) / 100.0;

  return (int)vol;
}

static int
vol_to_rel(int volume, int master_volume)
{
  float rel;

  if (volume == master_volume)
    return 100;

  rel = ((float)volume / (float)master_volume) * 100.0;

  return (int)rel;
}

static void
vol_adjust(void)
{
  struct output_device *device;
  int selected_highest = -1;
  int all_highest = -1;

  for (device = outputs_device_list; device; device = device->next)
    {
      if (OUTPUTS_DEVICE_DISPLAY_SELECTED(device) && (device->volume > selected_highest))
	selected_highest = device->volume;

      if (device->volume > all_highest)
	all_highest = device->volume;
    }

  outputs_master_volume = (selected_highest >= 0) ? selected_highest : all_highest;

  for (device = outputs_device_list; device; device = device->next)
    {
      device->relvol = vol_to_rel(device->volume, outputs_master_volume);
    }

#ifdef DEBUG_VOLUME
  DPRINTF(E_DBG, L_PLAYER, "*** Master: %d\n", outputs_master_volume);

  for (device = outputs_device_list; device; device = device->next)
    {
      DPRINTF(E_DBG, L_PLAYER, "*** %s: abs %d rel %d selected %d\n", device->name, device->volume, device->relvol, OUTPUTS_DEVICE_DISPLAY_SELECTED(device));
    }
#endif
}

/* ----------------------------------- API ---------------------------------- */

struct output_device *
outputs_device_get(uint64_t device_id)
{
  struct output_device *device;

  for (device = outputs_device_list; device; device = device->next)
    {
      if (device_id == device->id)
	return device;
    }

  DPRINTF(E_WARN, L_PLAYER, "Output device with id %" PRIu64 " is not in our list\n", device_id);
  return NULL;
}

uint64_t
outputs_buffer_duration_ms_get(void)
{
  return outputs_buffer_duration_ms;
}

bool
outputs_exclusive_mode_get(void)
{
  return outputs_exclusive_mode;
}

/* ----------------------- Called by backend modules ------------------------ */

// Sessions free their sessions themselves, but should not touch the device,
// since they can't know for sure that it is still valid in memory
int
outputs_device_session_add(uint64_t device_id, void *session)
{
  struct output_device *device;

  device = outputs_device_get(device_id);
  if (!device)
    return -1;

  device->session = session;
  return 0;
}

void
outputs_device_session_remove(uint64_t device_id)
{
  struct output_device *device;

  device = outputs_device_get(device_id);
  if (device)
    device->session = NULL;

  return;
}

int
outputs_quality_subscribe(struct media_quality *quality)
{
  int i;

  // If someone else is already subscribing to this quality we just increase the
  // reference count.
  for (i = 0; output_quality_subscriptions[i].count > 0; i++)
    {
      if (!quality_is_equal(quality, &output_quality_subscriptions[i].quality))
	continue;

      output_quality_subscriptions[i].count++;

      DPRINTF(E_DBG, L_PLAYER, "Subscription request for quality %d/%d/%d (now %d subscribers)\n",
	quality->sample_rate, quality->bits_per_sample, quality->channels, output_quality_subscriptions[i].count);

      return 0;
    }

  if (i >= (ARRAY_SIZE(output_quality_subscriptions) - 1))
    {
      DPRINTF(E_LOG, L_PLAYER, "Bug! The number of different quality levels requested by outputs is too high\n");
      return -1;
    }

  output_quality_subscriptions[i].quality = *quality;
  output_quality_subscriptions[i].count++;

  DPRINTF(E_DBG, L_PLAYER, "Subscription request for quality %d/%d/%d (now %d subscribers)\n",
    quality->sample_rate, quality->bits_per_sample, quality->channels, output_quality_subscriptions[i].count);

  // Better way of signaling this?
  outputs_got_new_subscription = true;

  return 0;
}

void
outputs_quality_unsubscribe(struct media_quality *quality)
{
  int i;

  // Find subscription
  for (i = 0; output_quality_subscriptions[i].count > 0; i++)
    {
      if (quality_is_equal(quality, &output_quality_subscriptions[i].quality))
	break;
    }

  if (output_quality_subscriptions[i].count == 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Bug! Unsubscription request for a quality level that there is no subscription for\n");
      return;
    }

  output_quality_subscriptions[i].count--;

  DPRINTF(E_DBG, L_PLAYER, "Unsubscription request for quality %d/%d/%d (now %d subscribers)\n",
    quality->sample_rate, quality->bits_per_sample, quality->channels, output_quality_subscriptions[i].count);

  if (output_quality_subscriptions[i].count > 0)
    return;

  transcode_encode_cleanup(&output_quality_subscriptions[i].encode_ctx);

  // Shift elements
  for (; i < ARRAY_SIZE(output_quality_subscriptions) - 1; i++)
    output_quality_subscriptions[i] = output_quality_subscriptions[i + 1];
}

// Output backends call back through the below wrapper to make sure that:
// 1. Callbacks are always deferred
// 2. The callback never has a dangling pointer to a device (a device that has been removed from our list)
void
outputs_cb(int callback_id, uint64_t device_id, enum output_device_state state)
{
  if (callback_id < 0)
    return;

  if (!(callback_id < ARRAY_SIZE(outputs_cb_register)) || !outputs_cb_register[callback_id].cb)
    {
      DPRINTF(E_LOG, L_PLAYER, "Bug! Output backend called us with an illegal callback id (%d)\n", callback_id);
      return;
    }

  DPRINTF(E_DBG, L_PLAYER, "Callback request received, id is %i\n", callback_id);

  outputs_cb_register[callback_id].ready = true;
  outputs_cb_register[callback_id].device_id = device_id;
  outputs_cb_register[callback_id].state = state;
  event_active(outputs_deferredev, 0, 0);
}

void
outputs_metadata_free(struct output_metadata *metadata)
{
  metadata_free(metadata);
}

struct output_buffer *
outputs_buffer_copy(struct output_buffer *buffer)
{
  return buffer_copy(buffer);
}

void
outputs_buffer_free(struct output_buffer *buffer)
{
  buffer_free(buffer);
}

/* ---------------------------- Called by player ---------------------------- */

struct output_device *
outputs_device_add(struct output_device *add, bool new_deselect)
{
  struct output_device *device;
  int keep_offset_ms;

  for (device = outputs_device_list; device; device = device->next)
    {
      if (device->id == add->id)
	break;
    }

  if (device)
    {
      // Known device: store or refresh the arriving protocol candidate.
      // candidate_store() handles in-place update if a session is active.
      const char *add_type_name = add->type_name; // save before ownership transfer
      candidate_store(device, add); // takes ownership of add; add may be freed
      supported_modes_recompute(device);
      effective_candidate_apply(device);

      DPRINTF(E_DBG, L_PLAYER, "Updated %s candidate for device '%s', supported modes: 0x%x\n",
              add_type_name, device->name, device->supported_modes);
    }
  else
    {
      // New device: allocate a canonical shell and store add as first candidate.
      // The canonical struct holds stable user-facing state (volume, selection,
      // preferred_mode, auth_key); transport fields are populated by
      // effective_candidate_apply() below.
      CHECK_NULL(L_PLAYER, device = calloc(1, sizeof(struct output_device)));

      device->id         = add->id;
      device->stop_timer = evtimer_new(evbase_player, stop_timer_cb, device);

      keep_offset_ms = add->offset_ms; // For legacy local audio and Chromecast where offset could come from config file

      device->selected       = 0;
      device->preferred_mode = OUTPUT_MODE_AUTO;
      device->volume         = (outputs_master_volume >= 0) ? outputs_master_volume : OUTPUTS_DEFAULT_VOLUME;

      /* Restore persisted AirPlay auth_key (HomeKit pairing) if available */
      {
        const char *saved_key = config_get_device_str(add->name, "auth_key", NULL);
        if (saved_key)
          device->auth_key = strdup(saved_key);
      }

      if (keep_offset_ms != 0)
	device->offset_ms = keep_offset_ms;

      if (new_deselect)
	device->selected = 0;

      candidate_store(device, add); // takes ownership of add
      supported_modes_recompute(device);
      effective_candidate_apply(device); // populates name, type, addresses, etc.

      device->next = outputs_device_list;
      outputs_device_list = device;
    }

  device_list_sort();

  vol_adjust();

  device->advertised = 1;
  output_group_refresh();

  return device;
}

void
outputs_device_remove(struct output_device *remove)
{
  struct output_device *device;
  struct output_device *prev;

  // Device stop should be able to handle that we invalidate the device, even
  // if it is an async stop. It might call outputs_device_session_remove(), but
  // that just won't do anything since the id will be unknown.
  if (remove->session)
    outputs_device_stop(remove, device_stop_cb);

  prev = NULL;
  for (device = outputs_device_list; device; device = device->next)
    {
      if (device == remove)
	break;

      prev = device;
    }

  if (!device)
    return;

  DPRINTF(E_INFO, L_PLAYER, "Removing %s device '%s'\n", remove->type_name, remove->name);

  if (!prev)
    outputs_device_list = remove->next;
  else
    prev->next = remove->next;

  outputs_device_free(remove);

  vol_adjust();
  output_group_refresh();
}

void
outputs_device_select(struct output_device *device, int max_volume)
{
  device->selected = 1;
  device->prevent_playback = 0;
  device->busy = 0;

  // The purpose of this is to cap the volume for a newly selected device. It is
  // used by the player to avoid this scenario:
  // 1 Play on two speakers, say Kitchen (100) and Office (75), master is 100
  // 2 Disable Office, reduce master to 25, Kitchen is now 25, Office is still 75
  // 3 Turn on Office, it now blasts at 75
  // We could avoid this by reducing the unselected Office in step 2, but that
  // leads to issue #1077, where volumes of unselected devices go to 0 (e.g. by
  // reducing master to 0 and then increasing again -> unselected stays at 0).
  if (max_volume >= 0 && device->volume > max_volume)
    device->volume = max_volume;

  vol_adjust();
}

void
outputs_device_deselect(struct output_device *device)
{
  device->selected = 0;

  vol_adjust();
}

int
outputs_device_start(struct output_device *device, output_status_cb cb, bool only_probe)
{
  int ret;

  if (outputs[device->type]->disabled || !outputs[device->type]->device_start || !outputs[device->type]->device_probe)
    return -1;

  if (device->session)
    return 0; // Device is already running, nothing to do

  if (only_probe)
    ret = outputs[device->type]->device_probe(device, callback_add(device, cb));
  else
    ret = outputs[device->type]->device_start(device, callback_add(device, cb));

  return device_state_update(device, ret);;
}

int
outputs_device_stop(struct output_device *device, output_status_cb cb)
{
  int ret;

  if (outputs[device->type]->disabled || !outputs[device->type]->device_stop)
    return -1;

  if (!device->session)
    return 0; // Device is already stopped, nothing to do

  ret = outputs[device->type]->device_stop(device, callback_add(device, cb));

  return device_state_update(device, ret);
}

int
outputs_device_stop_delayed(struct output_device *device, output_status_cb cb)
{
  if (outputs[device->type]->disabled || !outputs[device->type]->device_stop)
    return -1;

  if (!device->session)
    return 0; // Device is already stopped, nothing to do

  outputs[device->type]->device_cb_set(device, callback_add(device, cb));

  event_add(device->stop_timer, &outputs_stop_timeout);

  return 1;
}

int
outputs_device_flush(struct output_device *device, output_status_cb cb)
{
  int ret;

  if (outputs[device->type]->disabled || !outputs[device->type]->device_flush)
    return -1;

  if (!device->session)
    return 0; // Nothing to flush

  ret = outputs[device->type]->device_flush(device, callback_add(device, cb));

  return ret; // We don't change device state just because of a failed flush
}

void
outputs_device_volume_register(struct output_device *device, int absvol, int relvol)
{
  if (absvol > -1)
    device->volume = absvol;
  else if (relvol > -1)
    device->volume = rel_to_vol(relvol, outputs_master_volume);

  vol_adjust();
}

int
outputs_device_volume_set(struct output_device *device, output_status_cb cb)
{
  int ret;

  if (outputs[device->type]->disabled || !outputs[device->type]->device_volume_set)
    return -1;

  if (!device->session)
    return 0; // Device isn't active

  ret = outputs[device->type]->device_volume_set(device, callback_add(device, cb));

  return ret; // We don't change device state just because of a failed volume change
}

int
outputs_device_volume_to_pct(struct output_device *device, const char *volume)
{
  if (outputs[device->type]->disabled || !outputs[device->type]->device_volume_to_pct)
    return -1;

  return outputs[device->type]->device_volume_to_pct(device, volume);
}

int
outputs_device_quality_set(struct output_device *device, struct media_quality *quality, output_status_cb cb)
{
  int ret;

  if (outputs[device->type]->disabled || !outputs[device->type]->device_quality_set)
    return -1;

  ret = outputs[device->type]->device_quality_set(device, quality, callback_add(device, cb));

  return device_state_update(device, ret);
}

int
outputs_device_authorize(struct output_device *device, const char *pin, output_status_cb cb)
{
  int ret;

  if (outputs[device->type]->disabled || !outputs[device->type]->device_authorize)
    return -1;

  if (device->session)
    return 0; // We are already connected to the device - no auth required

  ret = outputs[device->type]->device_authorize(device, pin, callback_add(device, cb));

  return device_state_update(device, ret); // If ret < 0 then we couldn't reach the speaker
}

void
outputs_device_cb_set(struct output_device *device, output_status_cb cb)
{
  if (outputs[device->type]->disabled || !outputs[device->type]->device_cb_set)
    return;

  if (!device->session)
    return;

  outputs[device->type]->device_cb_set(device, callback_add(device, cb));
}

void
outputs_device_free(struct output_device *device)
{
  if (!device)
    return;

  DPRINTF(E_DBG, L_PLAYER,
          "outputs_device_free: device=%p id=%" PRIu64 " type=%s name='%s' display='%s' session=%p cand_raop=%p cand_airplay2=%p group_id=%s group_name='%s' raw_gid=%s extra=%p\n",
          device, device->id, device->type_name ? device->type_name : "(null)",
          device->name ? device->name : "(null)",
          device->display_name ? device->display_name : "(null)",
          device->session, device->candidate_raop, device->candidate_airplay2,
          device->group_id ? device->group_id : "(null)",
          device->group_name ? device->group_name : "(null)",
          device->raw_gid ? device->raw_gid : "(null)",
          device->extra_device_info);

  if (outputs[device->type]->disabled)
    DPRINTF(E_LOG, L_PLAYER, "BUG! Freeing device from a disabled output?\n");

  if (device->session)
    DPRINTF(E_LOG, L_PLAYER, "BUG! Freeing device with active session?\n");

  // Canonical devices borrow extra_device_info from their active candidate.
  // Null it out so device_free_extra is a no-op on the canonical; the
  // real cleanup happens via candidate_free() on each candidate slot below.
  if (device->candidate_raop || device->candidate_airplay2)
    device->extra_device_info = NULL;

  if (device->extra_device_info && outputs[device->type]->device_free_extra)
    outputs[device->type]->device_free_extra(device);

  if (device->stop_timer)
    event_free(device->stop_timer);

  free(device->name);
  free(device->display_name);
  free(device->group_id);
  free(device->group_name);
  free(device->raw_gid);
  free(device->auth_key);
  free(device->v4_address);
  free(device->v6_address);

  // Free protocol candidates (each owns its own extra_device_info)
  candidate_free(device->candidate_raop);
  candidate_free(device->candidate_airplay2);

  free(device);
}

void
outputs_device_candidate_apply(struct output_device *device)
{
  effective_candidate_apply(device);
}

int
outputs_device_mode_set(struct output_device *device, enum output_mode mode)
{
  uint32_t mode_bit;

  // Validate: only enumerated values are accepted
  if (mode != OUTPUT_MODE_AUTO && mode != OUTPUT_MODE_RAOP && mode != OUTPUT_MODE_AIRPLAY2)
    return -1;

  if (outputs_device_is_stereo_leader(device) && mode == OUTPUT_MODE_RAOP)
    {
      DPRINTF(E_WARN, L_PLAYER, "Ignoring requested mode '%s' for stereo output '%s': stereo pairs are AirPlay 2-only\n",
              output_mode_to_string(mode), outputs_device_display_name(device));
      return 0;
    }

  // For a concrete protocol check that the device actually supports it
  if (mode != OUTPUT_MODE_AUTO)
    {
      mode_bit = (uint32_t)mode;
      if (!(outputs_device_supported_modes(device) & mode_bit))
        {
          DPRINTF(E_WARN, L_PLAYER, "Ignoring requested mode '%s' for output '%s': protocol not available\n",
                  output_mode_to_string(mode), outputs_device_display_name(device));
          return 0; // Warn and ignore; not an error from the API's perspective
        }
    }

  device->preferred_mode = mode;

  // If no session is active apply immediately; otherwise the caller must stop
  // the session first and call outputs_device_candidate_apply() in the callback.
  if (!device->session)
    {
      effective_candidate_apply(device);
      return 0;
    }

  // Return 1 only when the effective backend would actually change
  return (effective_type_resolve(device) != device->type) ? 1 : 0;
}

int
outputs_device_protocol_remove(struct output_device *canonical, struct output_device *remove)
{
  struct output_device **slot;
  struct output_device *candidate;

  slot = candidate_slot(canonical, remove->type);
  if (!slot)
    return -1; // Not a known multi-protocol type; caller uses legacy path

  candidate = *slot;
  if (!candidate)
    return 0; // Already gone

  DPRINTF(E_DBG, L_PLAYER,
          "outputs_device_protocol_remove: canonical=%p id=%" PRIu64 " name='%s' type=%s session=%p remove=%p remove_type=%s candidate=%p candidate_type=%s candidate_name='%s' v4=%s:%d v6=%s:%d group_id=%s group_name='%s' raw_gid=%s\n",
          canonical, canonical->id, canonical->name ? canonical->name : "(null)",
          canonical->type_name ? canonical->type_name : "(null)", canonical->session,
          remove, remove->type_name ? remove->type_name : "(null)",
          candidate, candidate->type_name ? candidate->type_name : "(null)",
          candidate->name ? candidate->name : "(null)",
          candidate->v4_address ? candidate->v4_address : "(null)", candidate->v4_port,
          candidate->v6_address ? candidate->v6_address : "(null)", candidate->v6_port,
          candidate->group_id ? candidate->group_id : "(null)",
          candidate->group_name ? candidate->group_name : "(null)",
          candidate->raw_gid ? candidate->raw_gid : "(null)");

  // Remove the departing address family from the candidate
  if (remove->v4_port && candidate->v4_address)
    {
      DPRINTF(E_DBG, L_PLAYER, "outputs_device_protocol_remove: clearing IPv4 for candidate=%p address=%s port=%d\n",
              candidate, candidate->v4_address, candidate->v4_port);
      free(candidate->v4_address);
      candidate->v4_address = NULL;
      candidate->v4_port    = 0;
    }
  if (remove->v6_port && candidate->v6_address)
    {
      DPRINTF(E_DBG, L_PLAYER, "outputs_device_protocol_remove: clearing IPv6 for candidate=%p address=%s port=%d\n",
              candidate, candidate->v6_address, candidate->v6_port);
      free(candidate->v6_address);
      candidate->v6_address = NULL;
      candidate->v6_port    = 0;
    }

  // If the candidate still has at least one address it is still reachable
  if (candidate->v4_address || candidate->v6_address)
    {
      DPRINTF(E_DBG, L_PLAYER,
              "outputs_device_protocol_remove: candidate=%p still reachable after family removal, remaining v4=%s:%d v6=%s:%d\n",
              candidate,
              candidate->v4_address ? candidate->v4_address : "(null)", candidate->v4_port,
              candidate->v6_address ? candidate->v6_address : "(null)", candidate->v6_port);
      // If this is the active backend, refresh canonical's address fields
      if (canonical->type == candidate->type)
        effective_candidate_apply(canonical);
      return 0;
    }

  // Candidate has no addresses left
  DPRINTF(E_INFO, L_PLAYER, "Protocol %s no longer available for device '%s'\n",
          candidate->type_name, canonical->name);

  if (remove->type == OUTPUT_TYPE_AIRPLAY && candidate->group_id)
    DPRINTF(E_INFO, L_PLAYER, "AirPlay stereo member disappeared: device '%s', gid=%s, leader=%u\n",
            canonical->name, candidate->group_id, candidate->is_group_leader);

  if (canonical->session && canonical->type == candidate->type)
    {
      DPRINTF(E_DBG, L_PLAYER,
              "outputs_device_protocol_remove: preserving active candidate=%p for canonical id=%" PRIu64 " because session=%p is still using backend %s\n",
              candidate, canonical->id, canonical->session, candidate->type_name ? candidate->type_name : "(null)");
      // Active session is using this candidate's extra_device_info. Keep the
      // candidate alive so the session can finish cleanly; the session will
      // fail, and deferred_cb will switch to the remaining protocol after
      // device->session is cleared.
      uint32_t gone_bit = (remove->type == OUTPUT_TYPE_RAOP) ? OUTPUT_MODE_RAOP
                                                              : OUTPUT_MODE_AIRPLAY2;
      canonical->supported_modes &= ~gone_bit;
      // Mark as unadvertised only when no other protocol remains
      if (!canonical->supported_modes)
        canonical->advertised = 0;
      output_group_refresh();
      return 0;
    }

  // Safe to free the candidate now
  DPRINTF(E_DBG, L_PLAYER,
          "outputs_device_protocol_remove: freeing exhausted candidate=%p for canonical id=%" PRIu64 " name='%s'\n",
          candidate, canonical->id, canonical->name ? canonical->name : "(null)");
  candidate_free(*slot);
  *slot = NULL;
  // The canonical borrows extra_device_info from the active candidate.
  // Clear the borrowed pointer now that the candidate (and its owned extra)
  // have been freed, so outputs_device_free() doesn't double-free it.
  if (canonical->type == candidate->type)
    canonical->extra_device_info = NULL;
  supported_modes_recompute(canonical);

  if (!canonical->candidate_raop && !canonical->candidate_airplay2)
    {
      DPRINTF(E_DBG, L_PLAYER,
              "outputs_device_protocol_remove: canonical id=%" PRIu64 " has no remaining candidates, session=%p\n",
              canonical->id, canonical->session);
      canonical->advertised = 0;
      output_group_refresh();
      return canonical->session ? 0 : 1; // 1 = caller should remove canonical
    }

  // Another protocol is available: switch to it
  DPRINTF(E_DBG, L_PLAYER,
          "outputs_device_protocol_remove: canonical id=%" PRIu64 " switching to remaining backend, cand_raop=%p cand_airplay2=%p\n",
          canonical->id, canonical->candidate_raop, canonical->candidate_airplay2);
  effective_candidate_apply(canonical);
  output_group_refresh();
  return 0;
}

// The return value will be the number of devices we need to wait for, either
// because they are starting or shutting down. The return value is only negative
// if we don't have to wait, i.e. all the selected devices failed immediately.
int
outputs_start(output_status_cb started_cb, output_status_cb stopped_cb, bool only_probe)
{
  struct output_device *device;
  int pending = 0;
  int ret;

  for (device = outputs_device_list; device; device = device->next)
    {
      if (device->selected)
	continue;

      ret = outputs_device_stop(device, stopped_cb);
      if (ret > 0)
	pending += ret;
    }

  ret = 0; // We don't care about devices that returned an error on stop
  for (device = outputs_device_list; device; device = device->next)
    {
      if (!device->selected)
	continue;

      ret = outputs_device_start(device, started_cb, only_probe);
      if (ret > 0)
	pending += ret;
    }

  return (pending > 0) ? pending : ret;
}

int
outputs_stop(output_status_cb cb)
{
  struct output_device *device;
  int pending = 0;
  int ret;

  for (device = outputs_device_list; device; device = device->next)
    {
      ret = outputs_device_stop(device, cb);
      if (ret < 0)
	continue;

      pending += ret;
    }

  return pending;
}

int
outputs_stop_delayed_cancel(void)
{
  struct output_device *device;

  for (device = outputs_device_list; device; device = device->next)
    event_del(device->stop_timer);

  return 0;
}

int
outputs_flush(output_status_cb cb)
{
  struct output_device *device;
  int pending = 0;
  int ret;

  for (device = outputs_device_list; device; device = device->next)
    {
      ret = outputs_device_flush(device, cb);
      if (ret < 0)
	continue;

      pending += ret;
    }

  return pending;
}

int
outputs_volume_get(void)
{
  return outputs_master_volume;
}

int
outputs_volume_set(int volume, output_status_cb cb)
{
  struct output_device *device;
  int pending = 0;
  int ret;

  if (outputs_master_volume == volume)
    return 0;

  outputs_master_volume = volume;

  for (device = outputs_device_list; device; device = device->next)
    {
      if (!device->selected)
	continue;

      device->volume = rel_to_vol(device->relvol, outputs_master_volume);

      ret = outputs_device_volume_set(device, cb);
      if (ret < 0)
	continue;

      pending += ret;
    }

  return pending;
}

int
outputs_sessions_count(void)
{
  struct output_device *device;
  int count = 0;

  for (device = outputs_device_list; device; device = device->next)
    if (device->session)
      count++;

  return count;
}

void
outputs_write(void *buf, size_t bufsize, int nsamples, struct media_quality *quality, struct timespec *pts)
{
  int i;

  buffer_fill(&output_buffer, buf, bufsize, quality, nsamples, pts);

  for (i = 0; outputs[i]; i++)
    {
      if (outputs[i]->disabled)
	continue;

      if (outputs[i]->write)
	outputs[i]->write(&output_buffer);
    }

  buffer_drain(&output_buffer);
}

void
outputs_metadata_send(uint32_t item_id, bool startup, output_metadata_finalize_cb cb)
{
  int i;

  for (i = 0; outputs[i]; i++)
    {
      if (outputs[i]->disabled || !outputs[i]->metadata_send)
	continue;

      metadata_send(i, item_id, startup, cb);
    }
}

void
outputs_metadata_purge(void)
{
  int i;

  for (i = 0; outputs[i]; i++)
    {
      if (outputs[i]->disabled || !outputs[i]->metadata_purge)
	continue;

      outputs[i]->metadata_purge();
    }
}

int
outputs_priority(struct output_device *device)
{
  return outputs[device->type]->priority;
}

const char *
outputs_name(enum output_types type)
{
  return outputs[type]->name;
}

struct output_device *
outputs_list(void)
{
  return outputs_device_list;
}

int
outputs_init(void)
{
  int no_output;
  int ret;
  int i;

  outputs_master_volume = -1;

  // Number of milliseconds the outputs should buffer before starting playback.
  // The default of 2250 comes from RAOP, where iTunes and Apple Music will send
  // a sync packet with a 2000 ms/88200 delay, and the speaker will add it's own
  // audio latency of 250 ms/11025. See e.g. shairport-sync's rtp.c. While a
  // shorter delay would seem desirable, some Airplay devices ignore the values
  // we give and stick to 2.25 seconds.
  outputs_buffer_duration_ms = config_get_int("start_buffer_ms", 2250);
  if (outputs_buffer_duration_ms != 2250)
    DPRINTF(E_WARN, L_PLAYER, "Output buffer duration is configured to a non-standard value %" PRIu64 "\n", outputs_buffer_duration_ms);

  CHECK_NULL(L_PLAYER, outputs_deferredev = evtimer_new(evbase_player, deferred_cb, NULL));

  no_output = 1;
  for (i = 0; outputs[i]; i++)
    {
      if (outputs[i]->type != i)
	{
	  DPRINTF(E_FATAL, L_PLAYER, "BUG! Output definitions are misaligned with output enum\n");
	  return -1;
	}

      if (outputs[i]->disabled)
	{
	  continue;
	}

      if (!outputs[i]->init)
	{
	  no_output = 0;
	  continue;
	}

      ret = outputs[i]->init();
      if (ret < 0)
	outputs[i]->disabled = 1;
      else
	no_output = 0;
    }

  if (no_output)
    return -1;

  for (i = 0; i < ARRAY_SIZE(output_buffer.data); i++)
    output_buffer.data[i].evbuf = evbuffer_new();

  return 0;
}

void
outputs_deinit(void)
{
  int i;

  event_free(outputs_deferredev);

  for (i = 0; outputs[i]; i++)
    {
      if (outputs[i]->disabled)
	continue;

      if (outputs[i]->deinit)
        outputs[i]->deinit();
    }

  // In case some outputs forgot to unsubscribe
  for (i = 0; i < ARRAY_SIZE(output_quality_subscriptions); i++)
    if (output_quality_subscriptions[i].count > 0)
      {
	transcode_encode_cleanup(&output_quality_subscriptions[i].encode_ctx);
	memset(&output_quality_subscriptions[i], 0, sizeof(struct output_quality_subscription));
      }

  for (i = 0; i < ARRAY_SIZE(output_buffer.data); i++)
    evbuffer_free(output_buffer.data[i].evbuf);
}
