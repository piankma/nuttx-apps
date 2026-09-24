/****************************************************************************
 * apps/system/adb/adb_auth.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/* adb authentication: which hosts may connect.
 *
 * A host proves it holds a private key by signing a random token; adbd
 * accepts the signature if it verifies with one of the public keys in
 * CONFIG_ADBD_AUTH_KEYS, one per line, as a host's ~/.android/adbkey.pub
 * has it: the key in base64, a space, and a comment (user@host).
 *
 * A host with no allowed key offers its public key.  With
 * CONFIG_ADBD_AUTH_PUBKEY the key is kept in CONFIG_ADBD_AUTH_KEYS ".new"
 * for the user to allow (move it into the keys file); the host connects
 * once it is allowed.
 *
 * The key is Android's RSAPublicKey, a 2048-bit RSA key laid out for
 * Montgomery arithmetic: the modulus n, -1/n[0] mod 2^32 and R^2 mod n
 * (R = 2^2048), as little-endian 32-bit words.  The host signs the token
 * as if it were a SHA-1 digest, with PKCS#1 v1.5 padding, so checking a
 * signature is one modular exponentiation and a comparison.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/stat.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "adb.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ADB_RSA_WORDS   64                      /* 2048 bits */
#define ADB_RSA_BYTES   (ADB_RSA_WORDS * 4)
#define ADB_KEY_BYTES   (4 + 4 + 2 * ADB_RSA_BYTES + 4)
#define ADB_KEY_B64MAX  (((ADB_KEY_BYTES + 2) / 3) * 4)
#define ADB_SHA1_BYTES  20

#define ADB_KEYS_NEW    CONFIG_ADBD_AUTH_KEYS ".new"

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Android's RSAPublicKey, as a host sends and stores it */

struct adb_rsa_key_s
{
  uint32_t len;                     /* Words in n: ADB_RSA_WORDS */
  uint32_t n0inv;                   /* -1 / n[0] mod 2^32 */
  uint32_t n[ADB_RSA_WORDS];        /* The modulus */
  uint32_t rr[ADB_RSA_WORDS];       /* R^2 mod n */
  uint32_t exponent;                /* 3 or 65537 */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The DER prefix of a SHA-1 DigestInfo, which PKCS#1 v1.5 puts before
 * the digest
 */

static const uint8_t g_sha1_prefix[] =
{
  0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e,
  0x03, 0x02, 0x1a, 0x05, 0x00, 0x04, 0x14
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int adb_b64_value(char c)
{
  if (c >= 'A' && c <= 'Z')
    {
      return c - 'A';
    }

  if (c >= 'a' && c <= 'z')
    {
      return c - 'a' + 26;
    }

  if (c >= '0' && c <= '9')
    {
      return c - '0' + 52;
    }

  return c == '+' ? 62 : c == '/' ? 63 : -1;
}

/* Decode base64 up to the first character that is not base64 ('=', a
 * space); returns the bytes written, or -1 if out is too small
 */

static int adb_b64_decode(const char *in, uint8_t *out, size_t size)
{
  uint32_t bits = 0;
  size_t len = 0;
  int nbits = 0;
  int v;

  while ((v = adb_b64_value(*in++)) >= 0)
    {
      bits   = (bits << 6) | v;
      nbits += 6;
      if (nbits >= 8)
        {
          nbits -= 8;
          if (len >= size)
            {
              return -1;
            }

          out[len++] = (uint8_t)(bits >> nbits);
        }
    }

  return len;
}

/* a -= n */

static void adb_rsa_sub_n(const struct adb_rsa_key_s *key, uint32_t *a)
{
  int64_t borrow = 0;
  int i;

  for (i = 0; i < ADB_RSA_WORDS; i++)
    {
      borrow += (uint64_t)a[i] - key->n[i];
      a[i]    = (uint32_t)borrow;
      borrow >>= 32;
    }
}

/* a >= n */

static int adb_rsa_ge_n(const struct adb_rsa_key_s *key, const uint32_t *a)
{
  int i;

  for (i = ADB_RSA_WORDS - 1; i >= 0; i--)
    {
      if (a[i] != key->n[i])
        {
          return a[i] > key->n[i];
        }
    }

  return 1;
}

/* c = (c + a * b) / 2^32 mod n, one word a at a time */

static void adb_rsa_mul_add(const struct adb_rsa_key_s *key, uint32_t *c,
                            uint32_t a, const uint32_t *b)
{
  uint64_t x = (uint64_t)a * b[0] + c[0];
  uint32_t d = (uint32_t)x * key->n0inv;
  uint64_t y = (uint64_t)d * key->n[0] + (uint32_t)x;
  int i;

  for (i = 1; i < ADB_RSA_WORDS; i++)
    {
      x = (x >> 32) + (uint64_t)a * b[i] + c[i];
      y = (y >> 32) + (uint64_t)d * key->n[i] + (uint32_t)x;
      c[i - 1] = (uint32_t)y;
    }

  x = (x >> 32) + (y >> 32);
  c[i - 1] = (uint32_t)x;
  if ((x >> 32) != 0)
    {
      adb_rsa_sub_n(key, c);
    }
}

/* c = a * b / R mod n (Montgomery) */

static void adb_rsa_mul(const struct adb_rsa_key_s *key, uint32_t *c,
                        const uint32_t *a, const uint32_t *b)
{
  int i;

  memset(c, 0, ADB_RSA_BYTES);
  for (i = 0; i < ADB_RSA_WORDS; i++)
    {
      adb_rsa_mul_add(key, c, a[i], b);
    }
}

/* in = in ^ e mod n, in place, as big-endian bytes */

static void adb_rsa_pow(const struct adb_rsa_key_s *key, uint8_t *in)
{
  uint32_t *a  = malloc(3 * ADB_RSA_BYTES);
  uint32_t *ar;
  uint32_t *aar;
  uint32_t *t;
  int i;

  if (a == NULL)
    {
      memset(in, 0, ADB_RSA_BYTES);
      return;
    }

  ar  = a + ADB_RSA_WORDS;
  aar = ar + ADB_RSA_WORDS;

  for (i = 0; i < ADB_RSA_WORDS; i++)
    {
      const uint8_t *p = in + ADB_RSA_BYTES - 4 * (i + 1);

      a[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
             ((uint32_t)p[2] << 8) | p[3];
    }

  /* aR = a R; square it 16 times (e = 65537) or once (e = 3) to a^(e-1)
   * R; one more multiplication by a gives a^e
   */

  adb_rsa_mul(key, ar, a, key->rr);
  for (i = 0; i < (key->exponent == 3 ? 1 : 16); i++)
    {
      adb_rsa_mul(key, aar, ar, ar);
      t = ar, ar = aar, aar = t;
    }

  adb_rsa_mul(key, aar, ar, a);
  if (adb_rsa_ge_n(key, aar))
    {
      adb_rsa_sub_n(key, aar);
    }

  for (i = 0; i < ADB_RSA_WORDS; i++)
    {
      uint8_t *p = in + ADB_RSA_BYTES - 4 * (i + 1);
      uint32_t w = aar[i];

      p[0] = w >> 24;
      p[1] = w >> 16;
      p[2] = w >> 8;
      p[3] = w;
    }

  free(a);
}

/* Whether sig is token signed with key */

static int adb_rsa_verify(const struct adb_rsa_key_s *key,
                          const uint8_t *token, const uint8_t *sig)
{
  uint8_t *m;
  size_t pad = ADB_RSA_BYTES - 3 - sizeof(g_sha1_prefix) - ADB_SHA1_BYTES;
  int ok;

  if (key->len != ADB_RSA_WORDS ||
      (key->exponent != 3 && key->exponent != 65537))
    {
      return 0;
    }

  m = malloc(ADB_RSA_BYTES);
  if (m == NULL)
    {
      return 0;
    }

  memcpy(m, sig, ADB_RSA_BYTES);
  adb_rsa_pow(key, m);

  /* 00 01 ff...ff 00, the DigestInfo prefix, then the token */

  ok = m[0] == 0x00 && m[1] == 0x01 && m[2 + pad] == 0x00 &&
       memcmp(&m[3 + pad], g_sha1_prefix, sizeof(g_sha1_prefix)) == 0 &&
       memcmp(&m[3 + pad + sizeof(g_sha1_prefix)], token,
              ADB_SHA1_BYTES) == 0;
  while (ok && pad-- > 0)
    {
      ok = m[2 + pad] == 0xff;
    }

  free(m);
  return ok;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int adb_auth_verify(const uint8_t *token, size_t tokenlen,
                    const uint8_t *sig, size_t siglen)
{
  struct adb_rsa_key_s *key;
  char *line;
  FILE *f;
  int ret = -EPERM;

  if (tokenlen != ADB_SHA1_BYTES || siglen != ADB_RSA_BYTES)
    {
      return -EINVAL;
    }

  f = fopen(CONFIG_ADBD_AUTH_KEYS, "r");
  if (f == NULL)
    {
      return -ENOENT;
    }

  key  = malloc(sizeof(*key));
  line = malloc(ADB_KEY_B64MAX + 128);
  if (key == NULL || line == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  while (fgets(line, ADB_KEY_B64MAX + 128, f) != NULL)
    {
      if (adb_b64_decode(line, (uint8_t *)key, sizeof(*key)) ==
          sizeof(*key) && adb_rsa_verify(key, token, sig))
        {
          syslog(LOG_NOTICE, "adbd: host allowed:%s",
                 strchr(line, ' ') != NULL ? strchr(line, ' ') : "\n");
          ret = 0;
          break;
        }
    }

out:
  free(line);
  free(key);
  fclose(f);
  return ret;
}

void adb_auth_offer(const char *key, size_t len)
{
#ifdef CONFIG_ADBD_AUTH_PUBKEY
  const char *comment;
  FILE *f;

  /* The key is base64, a space, a comment, then usually a NUL */

  while (len > 0 && (key[len - 1] == '\0' || key[len - 1] == '\n'))
    {
      len--;
    }

  if (len == 0 || len > ADB_KEY_B64MAX + 128 ||
      memchr(key, '\n', len) != NULL)
    {
      return;
    }

  f = fopen(ADB_KEYS_NEW, "w");
  if (f == NULL)
    {
      syslog(LOG_ERR, "adbd: %s: %d\n", ADB_KEYS_NEW, errno);
      return;
    }

  fwrite(key, 1, len, f);
  fputc('\n', f);
  fclose(f);

  comment = memchr(key, ' ', len);
  syslog(LOG_WARNING, "adbd: a new host asks to connect (%.*s): allow it "
         "with pnut adb allow\n",
         comment != NULL ? (int)(len - (comment + 1 - key)) : 0,
         comment != NULL ? comment + 1 : "");
#else
  syslog(LOG_WARNING, "adbd: a host with no allowed key\n");
#endif
}
