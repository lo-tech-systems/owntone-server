
#ifndef __PLAYER_H__
#define __PLAYER_H__

#include <stdbool.h>
#include <stdint.h>

#include "db.h"
#include "misc.h" // for struct media_quality

enum play_status {
  PLAY_STOPPED = 2,
  PLAY_PAUSED  = 3,
  PLAY_PLAYING = 4,
};


struct player_speaker_info {
  uint64_t id;
  uint32_t index;
  uint32_t active_remote;
  char name[255];
  char output_type[50];
  int relvol;
  int absvol;
  int offset_ms;

  enum media_format format;
  uint32_t supported_formats;

  bool selected;
  bool has_password;
  bool requires_auth;
  bool needs_auth_key;

  bool prevent_playback;
  bool busy;

  bool has_video;
};

struct player_status {
  enum play_status status;
  char consume;

  int volume;

  /* Id of the playing file/item in the files database */
  uint32_t id;
  /* Item-Id of the playing file/item in the queue */
  uint32_t item_id;
  /* Elapsed time in ms of playing item */
  uint32_t pos_ms;
  /* Length in ms of playing item */
  uint32_t len_ms;
};

typedef void (*spk_enum_cb)(struct player_speaker_info *spk, void *arg);

int
player_get_status(struct player_status *status);

void
player_speaker_enumerate(spk_enum_cb cb, void *arg);

int
player_speaker_set(uint64_t *ids);

int
player_speaker_get_byid(struct player_speaker_info *spk, uint64_t id);

int
player_speaker_get_byactiveremote(struct player_speaker_info *spk, uint32_t active_remote);

int
player_speaker_get_byaddress(struct player_speaker_info *spk, const char *address);

int
player_speaker_get_byindex(struct player_speaker_info *spk, uint32_t index);

int
player_speaker_enable(uint64_t id);

int
player_speaker_disable(uint64_t id);

void
player_speaker_resurrect(void *arg);

int
player_speaker_authorize(uint64_t id, const char *pin);

int
player_speaker_format_set(uint64_t id, enum media_format format);

int
player_speaker_offset_ms_set(uint64_t id, int offset_ms);


int
player_playback_start(void);

int
player_playback_start_byitem(struct db_queue_item *queue_item);

int
player_playback_start_byid(uint32_t id);

int
player_playback_stop(void);

int
player_playback_pause(void);

int
player_playback_flush(void);

int
player_volume_set(int vol);

int
player_volume_setrel_speaker(uint64_t id, int relvol);

int
player_volume_setabs_speaker(uint64_t id, int vol);

int
player_volume_setraw_speaker(uint64_t id, const char *volstr);

int
player_device_add(void *device);

int
player_device_remove(void *device);

int
player_init(void);

void
player_deinit(void);

#endif /* !__PLAYER_H__ */
