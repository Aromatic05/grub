/*
 * GRUB-specific, client-only Mbed TLS configuration.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef GRUB_MBEDTLS_CONFIG_H
#define GRUB_MBEDTLS_CONFIG_H

/* Keep the TLS implementation independent of a hosted C library. */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_PLATFORM_NO_STD_FUNCTIONS
#define MBEDTLS_PLATFORM_CALLOC_MACRO grub_calloc
#define MBEDTLS_PLATFORM_FREE_MACRO grub_free
#define MBEDTLS_PLATFORM_SNPRINTF_MACRO grub_snprintf
#define MBEDTLS_PLATFORM_VSNPRINTF_MACRO grub_vsnprintf
#define MBEDTLS_NO_PLATFORM_ENTROPY

#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_TLS_C
/* Required by virtual-hosted HTTPS APIs: send SNI in ClientHello.
 * mbedtls_ssl_set_hostname() alone does not enable the extension. */
/* Explicitly disable DTLS CID compatibility in this TLS-only build. */
#define MBEDTLS_SSL_DTLS_CONNECTION_ID_COMPAT 0
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED

#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECP_WINDOW_SIZE 2
#define MBEDTLS_ECP_FIXED_POINT_OPTIM 0

#define MBEDTLS_AES_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_NO_UDBL_DIVISION
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_GCM_C
#define MBEDTLS_MD_C
#define MBEDTLS_OID_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_SHA256_C
/* SHA-384 is a separate feature in Mbed TLS 3.x; SHA512_C alone
 * does not provide ECDSA-SHA384 or RSA-SHA384 certificate OIDs. */
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_USE_C

#define MBEDTLS_SSL_IN_CONTENT_LEN 16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN 16384
#define MBEDTLS_ENTROPY_MAX_SOURCES 2
#define MBEDTLS_AES_ROM_TABLES

#endif
