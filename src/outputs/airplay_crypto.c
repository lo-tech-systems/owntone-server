/*
 * Copyright (C) OwnTone contributors
 *
 * ChaCha20-Poly1305 helpers for AirPlay 2 audio-payload encryption, factored
 * out of airplay.c so the realtime and buffered senders share one
 * implementation of the low-level cipher primitives.
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

#include <gcrypt.h>

#include "airplay_crypto.h"

void
airplay_crypto_close(gcry_cipher_hd_t hd)
{
  if (!hd)
    return;

  gcry_cipher_close(hd);
}

gcry_cipher_hd_t
airplay_crypto_open(const uint8_t *key, size_t key_len)
{
  gcry_cipher_hd_t hd;

  if (gcry_cipher_open(&hd, GCRY_CIPHER_CHACHA20, GCRY_CIPHER_MODE_POLY1305, 0) != GPG_ERR_NO_ERROR)
    goto error;

  if (gcry_cipher_setkey(hd, key, key_len) != GPG_ERR_NO_ERROR)
    goto error;

  return hd;

 error:
  airplay_crypto_close(hd);
  return NULL;
}

int
airplay_crypto_encrypt(uint8_t *cipher, uint8_t *plain, size_t plain_len, const void *ad, size_t ad_len, uint8_t *tag, size_t tag_len, uint8_t *nonce, size_t nonce_len, gcry_cipher_hd_t hd)
{
  if (gcry_cipher_setiv(hd, nonce, nonce_len) != GPG_ERR_NO_ERROR)
    return -1;

  if (gcry_cipher_authenticate(hd, ad, ad_len) != GPG_ERR_NO_ERROR)
    return -1;

  if (gcry_cipher_encrypt(hd, cipher, plain_len, plain, plain_len) != GPG_ERR_NO_ERROR)
    return -1;

  if (gcry_cipher_gettag(hd, tag, tag_len) != GPG_ERR_NO_ERROR)
    return -1;

  return 0;
}
