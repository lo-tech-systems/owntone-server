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

#ifndef SRC_LIBRARY_H_
#define SRC_LIBRARY_H_

#include <stdbool.h>

/*
 * Returns true if a library scan is in progress (always false — no scanner).
 */
bool
library_is_scanning(void);

/*
 * Trigger for sending the DATABASE event after a db change.
 * Thread-safe; may be called from any thread.
 */
void
library_update_trigger(short update_events);

int
library_init(void);

void
library_deinit(void);

#endif /* SRC_LIBRARY_H_ */
