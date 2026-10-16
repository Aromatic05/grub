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

#ifndef APPENDEDSIG_H
#define APPENDEDSIG_H

#include <grub/crypto.h>
#include "x509.h"
#include "pkcs7.h"

/* Max size of hash data. */
#define GRUB_MAX_HASH_LEN  64

/* Public key type. */
#define PKEY_ID_PKCS7      2

/* Appended signature magic string and size. */
#define SIG_MAGIC          "~Module signature appended~\n"
#define SIG_MAGIC_SIZE     ((sizeof(SIG_MAGIC) - 1))

#define OPTION_BINARY_HASH 0
#define OPTION_CERT_HASH   1

/*
 * This structure is extracted from scripts/sign-file.c in the linux kernel
 * source. It was licensed as LGPLv2.1+, which is GPLv3+ compatible.
 */
struct module_signature
{
  grub_uint8_t algo;       /* Public-key crypto algorithm [0]. */
  grub_uint8_t hash;       /* Digest algorithm [0]. */
  grub_uint8_t id_type;    /* Key identifier type [PKEY_ID_PKCS7]. */
  grub_uint8_t signer_len; /* Length of signer's name [0]. */
  grub_uint8_t key_id_len; /* Length of key identifier [0]. */
  grub_uint8_t __pad[3];
  grub_uint32_t sig_len;   /* Length of signature data. */
} GRUB_PACKED;
typedef struct module_signature grub_modsig_t;

#define SIG_METADATA_SIZE  (sizeof (grub_modsig_t))
#define APPENDED_SIG_SIZE(pkcs7_data_size) \
                           (pkcs7_data_size + SIG_MAGIC_SIZE + SIG_METADATA_SIZE)

/* This represents an entire, parsed, appended signature. */
struct append_sig
{
  grub_modsig_t sig_metadata;     /* Module signature metadata. */
  grub_size_t signature_len;      /* Length of PKCS#7 data + metadata + magic. */
  grub_pkcs7_signed_data_t pkcs7; /* Parsed PKCS#7 data. */
};
typedef struct append_sig grub_append_sig_t;

typedef struct hash_list grub_hash_t;

/* This represents Secure Boot Signature Databases db and dbx. */
struct sd
{
  grub_x509_cert_t *certs;    /* Certificate. */
  grub_uint32_t no_of_certs;  /* Sizes of certificate/binary hash. */
  grub_hash_t *hashes;        /* Certificate/binary hash. */
  grub_uint32_t no_of_hashes; /* Sizes of certificate/binary hash. */
  bool is_db;
};
typedef struct sd grub_append_sd_t;

#endif /* APPENDEDSIG_H */
