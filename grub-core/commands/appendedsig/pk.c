/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2020, 2022 Free Software Foundation, Inc.
 *  Copyright (C) 2020, 2022, 2025 IBM Corporation
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <grub/types.h>
#include <grub/err.h>
#include <grub/mm.h>
#include <grub/crypto.h>
#include <grub/misc.h>
#include <grub/gcrypt/gcrypt.h>

#include "pk.h"

/*
 * Prepare the hash and S-expressions (sexp), and perform the signature
 * verification using RSA
 */
grub_err_t
grub_pk_rsa_verify (const grub_uint8_t *data, const grub_size_t data_len,
                    const gcry_md_spec_t *hash, const grub_uint8_t *sig,
                    const grub_int32_t sig_len, const grub_pk_key_t *pk,
                    const char *algo)
{
  grub_size_t errof;
  gpg_error_t rc;
  grub_err_t ret = GRUB_ERR_BAD_SIGNATURE;
  gcry_sexp_t s_hdata = NULL, s_pubkey = NULL, s_sig = NULL;
  grub_uint8_t *hash_data;

  if (data == NULL || data_len == 0 || hash == NULL || sig == NULL ||
      pk == NULL || algo == NULL)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "bad input data");

  hash_data = grub_zalloc (hash->mdlen);
  if (hash_data == NULL)
    return grub_error (GRUB_ERR_OUT_OF_MEMORY,
                       "could not allocate memory for hash data");

  grub_crypto_hash (hash, hash_data, data, data_len);

  rc = _gcry_sexp_build (&s_hdata, &errof, "(data (flags %s) (hash %s %b))", "pkcs1",
                         hash->name, hash->mdlen, hash_data);
  if (rc != GPG_ERR_NO_ERROR)
    {
      grub_dprintf ("appendedsig", "building sexp for hash data failed: %s",
                    gpg_strerror (rc));
      goto exit;
    }

  rc = _gcry_sexp_build (&s_pubkey, &errof, "(public-key (dsa (n %M) (e %M)))",
                         pk->modulus, pk->exponent);
  if (rc != GPG_ERR_NO_ERROR)
    {
      grub_dprintf ("appendedsig", "building sexp for public key failed: %s",
                    gpg_strerror (rc));
      goto exit;
    }

  rc = _gcry_sexp_build (&s_sig, &errof, "(sig-val (%s (s %b)))", algo, sig_len, sig);
  if (rc != GPG_ERR_NO_ERROR)
    {
      grub_dprintf ("appendedsig", "building sexp for signature failed: %s",
                    gpg_strerror (rc));
      goto exit;
    }

  rc = (grub_crypto_pk_rsa != NULL && grub_crypto_pk_rsa->verify != NULL ?
        grub_crypto_pk_rsa->verify (s_sig, s_hdata, s_pubkey) : GPG_ERR_INV_OBJ);
  if (rc != GPG_ERR_NO_ERROR)
    grub_dprintf ("appendedsig", "RSA verify failed: %s", gpg_strerror (rc));
  else
    ret = GRUB_ERR_NONE;

 exit:
  grub_free (hash_data);
  _gcry_sexp_release (s_sig);
  _gcry_sexp_release (s_hdata);
  _gcry_sexp_release (s_pubkey);

  return ret;
}

/*
 * Prepare the S-expressions (sexp) for key and signature, and perform the signature
 * verification.
 *
 * Note: For now, only support pure ML-DSA.
 */
grub_err_t
grub_pk_mldsa_verify (const grub_uint8_t *data, const grub_size_t data_len,
                      const gcry_md_spec_t *hash, const grub_uint8_t *sig,
                      const grub_int32_t sig_len, const grub_pk_key_t *pk,
                      const char *algo)
{
  grub_size_t errof;
  gpg_error_t rc;
  grub_err_t ret = GRUB_ERR_BAD_SIGNATURE;
  gcry_sexp_t s_pubkey = NULL, s_sig = NULL;

  if (data == NULL || data_len == 0 || sig == NULL || pk == NULL || algo == NULL)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "bad input data");

  (void) hash; /* For now, only support pure ML-DSA. So, we can't use this now. */

  rc = _gcry_sexp_build (&s_pubkey, &errof, "(%s (p %b))", algo, pk->raw_len, pk->raw);
  if (rc != GPG_ERR_NO_ERROR)
    {
      grub_dprintf ("appendedsig", "building sexp for public key failed: %s",
                    gpg_strerror (rc));
      goto exit;
    }

  rc = _gcry_sexp_build (&s_sig, &errof, "(sig-val (%s (s %b)))", algo, sig_len, sig);
  if (rc != GPG_ERR_NO_ERROR)
    {
      grub_dprintf ("appendedsig", "building sexp for signature failed: %s",
                    gpg_strerror (rc));
      goto exit;
    }

  rc = (grub_crypto_pk_mldsa != NULL && grub_crypto_pk_mldsa->raw_verify != NULL ?
        grub_crypto_pk_mldsa->raw_verify (s_sig, s_pubkey, data, data_len, NULL, 0) :
        GPG_ERR_INV_OBJ);
  if (rc != GPG_ERR_NO_ERROR)
    grub_dprintf ("appendedsig", "ML-DSA verify failed: %s", gpg_strerror (rc));
  else
    ret = GRUB_ERR_NONE;

 exit:
  _gcry_sexp_release (s_sig);
  _gcry_sexp_release (s_pubkey);

  return ret;
}
