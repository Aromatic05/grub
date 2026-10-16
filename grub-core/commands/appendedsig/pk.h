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

#ifndef PK_H
#define PK_H

#include <grub/types.h>
#include <grub/err.h>
#include <grub/crypto.h>
#include <grub/misc.h>
#include <grub/gcrypt/gcrypt.h>

struct pub_key
{
  gcry_mpi_t modulus;
  gcry_mpi_t exponent;
  grub_uint8_t *raw;
  grub_int32_t raw_len;
};
typedef struct pub_key grub_pk_key_t;

/* Type for the pk verify function.  */
typedef grub_err_t (*grub_pk_verify_t) (const grub_uint8_t *data,
                                        const grub_size_t data_len,
                                        const gcry_md_spec_t *hash,
                                        const grub_uint8_t *sig,
                                        const grub_int32_t sig_len,
                                        const grub_pk_key_t *pk,
                                        const char *algo);

/* Prepare the hash and S-expressions (sexp), and perform the signature verification. */
extern grub_err_t
grub_pk_rsa_verify (const grub_uint8_t *data, const grub_size_t data_len,
                    const gcry_md_spec_t *hash, const grub_uint8_t *sig,
                    const grub_int32_t sig_len, const grub_pk_key_t *pk,
                    const char *algo);

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
                      const char *algo);

#endif /* PK_H */
