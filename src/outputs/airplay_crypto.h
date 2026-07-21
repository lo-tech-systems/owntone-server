/*
 * Copyright (C) OwnTone contributors
 *
 * ChaCha20-Poly1305 helpers for AirPlay 2 audio-payload encryption. The
 * realtime and buffered senders use the same cipher, mode and session key, so
 * the low-level open/close/encrypt primitives live here and are shared; each
 * sender keeps its own nonce, associated-data and framing logic.
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

#ifndef __AIRPLAY_CRYPTO_H__
#define __AIRPLAY_CRYPTO_H__

#include <stddef.h>
#include <stdint.h>
#include <gcrypt.h>

// Open a ChaCha20-Poly1305 cipher handle keyed with the given key. Returns NULL
// on failure.
gcry_cipher_hd_t
airplay_crypto_open(const uint8_t *key, size_t key_len);

// Close a cipher handle. NULL-safe.
void
airplay_crypto_close(gcry_cipher_hd_t hd);

// Encrypt plain_len bytes with the given nonce and associated data, writing the
// ciphertext to cipher and the authentication tag to tag. Returns 0 on success,
// -1 on failure.
int
airplay_crypto_encrypt(uint8_t *cipher, uint8_t *plain, size_t plain_len, const void *ad, size_t ad_len, uint8_t *tag, size_t tag_len, uint8_t *nonce, size_t nonce_len, gcry_cipher_hd_t hd);

#endif /* !__AIRPLAY_CRYPTO_H__ */
