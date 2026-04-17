/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
/*
 * webtransportd — autocert.c
 *
 * Self-signed cert generator. See header for contract. Implemented
 * with mbedtls's x509write + ECP APIs; the whole dance stays on the
 * heap / stack — no /tmp, no OS keystore. The PRNG seed is pulled
 * from mbedtls_entropy_context which calls into the OS entropy
 * source (getentropy on POSIX, BCryptGenRandom on Windows via
 * mbedtls's Windows backend).
 */

#include "autocert.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecp.h"
#include "mbedtls/entropy.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"

/* Cert field constants — tuned for loopback dev use. Operators who
 * need a different subject / validity should ship their own PEM
 * pair instead of using --cert=auto. */
#define AUTOCERT_CN              "CN=webtransportd-autocert"
#define AUTOCERT_VALID_DAYS      30
#define AUTOCERT_DER_CAP         (4 * 1024)  /* DER fits well under 1 KiB */
#define AUTOCERT_KEY_DER_CAP     (1 * 1024)

static void format_time(time_t t, char out[16]) {
	/* mbedtls wants "YYYYMMDDHHMMSS" (14 chars) + NUL. */
	struct tm tm_utc;
#ifdef _WIN32
	gmtime_s(&tm_utc, &t);
#else
	gmtime_r(&t, &tm_utc);
#endif
	strftime(out, 16, "%Y%m%d%H%M%S", &tm_utc);
}

int wtd_autocert_generate(uint8_t **p_cert_der, size_t *p_cert_len,
		uint8_t **p_key_der, size_t *p_key_len) {
	if (p_cert_der == NULL || p_cert_len == NULL
			|| p_key_der == NULL || p_key_len == NULL) {
		return -1;
	}
	*p_cert_der = NULL;
	*p_cert_len = 0;
	*p_key_der = NULL;
	*p_key_len = 0;

	int rc = -1;
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context drbg;
	mbedtls_pk_context key;
	mbedtls_x509write_cert crt;
	uint8_t cert_buf[AUTOCERT_DER_CAP];
	uint8_t key_buf[AUTOCERT_KEY_DER_CAP];
	uint8_t *cert_heap = NULL;
	uint8_t *key_heap = NULL;

	mbedtls_entropy_init(&entropy);
	mbedtls_ctr_drbg_init(&drbg);
	mbedtls_pk_init(&key);
	mbedtls_x509write_crt_init(&crt);

	const char *seed = "webtransportd-autocert";
	if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
			(const unsigned char *)seed, strlen(seed)) != 0) {
		goto out;
	}

	/* EC P-256 (secp256r1) keypair. picoquic's mbedtls bridge
	 * handles ECDSA certs without extra configuration. */
	if (mbedtls_pk_setup(&key,
			mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0) {
		goto out;
	}
	if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,
			mbedtls_pk_ec(key),
			mbedtls_ctr_drbg_random, &drbg) != 0) {
		goto out;
	}

	/* Cert body. Self-signed: issuer == subject, issuer key == subject key. */
	if (mbedtls_x509write_crt_set_subject_name(&crt, AUTOCERT_CN) != 0) {
		goto out;
	}
	if (mbedtls_x509write_crt_set_issuer_name(&crt, AUTOCERT_CN) != 0) {
		goto out;
	}
	mbedtls_x509write_crt_set_subject_key(&crt, &key);
	mbedtls_x509write_crt_set_issuer_key(&crt, &key);
	mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);

	/* Serial: a single "1" byte. mbedtls requires at least 1 byte
	 * of raw serial; unique-per-cert doesn't matter for a
	 * dev-loopback keypair. */
	const unsigned char serial[] = { 0x01 };
	if (mbedtls_x509write_crt_set_serial_raw(&crt,
			(unsigned char *)serial, sizeof(serial)) != 0) {
		goto out;
	}

	/* Validity: now → now + 30 days. */
	time_t now = time(NULL);
	time_t end = now + (time_t)AUTOCERT_VALID_DAYS * 24 * 60 * 60;
	char nb[16] = { 0 };
	char na[16] = { 0 };
	format_time(now, nb);
	format_time(end, na);
	if (mbedtls_x509write_crt_set_validity(&crt, nb, na) != 0) {
		goto out;
	}

	/* Basic constraints: not a CA. */
	if (mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1) != 0) {
		goto out;
	}

	/* Subject Alternative Names: DNS:localhost + IP:127.0.0.1 +
	 * IP:::1. Browser cert-matching since Chrome 58 ignores CN in
	 * favor of SAN — without these, loopback connections reject. */
	static const uint8_t loopback4[4] = { 127, 0, 0, 1 };
	static const uint8_t loopback6[16] = { 0, 0, 0, 0, 0, 0, 0, 0,
			0, 0, 0, 0, 0, 0, 0, 1 };
	mbedtls_x509_san_list san_ip4 = {
		.node = { .type = MBEDTLS_X509_SAN_IP_ADDRESS,
			.san = { .unstructured_name = { .p = (unsigned char *)loopback4,
					.len = sizeof(loopback4) } } },
		.next = NULL,
	};
	mbedtls_x509_san_list san_ip6 = {
		.node = { .type = MBEDTLS_X509_SAN_IP_ADDRESS,
			.san = { .unstructured_name = { .p = (unsigned char *)loopback6,
					.len = sizeof(loopback6) } } },
		.next = &san_ip4,
	};
	mbedtls_x509_san_list san_dns = {
		.node = { .type = MBEDTLS_X509_SAN_DNS_NAME,
			.san = { .unstructured_name = { .p = (unsigned char *)"localhost",
					.len = strlen("localhost") } } },
		.next = &san_ip6,
	};
	if (mbedtls_x509write_crt_set_subject_alternative_name(&crt,
			&san_dns) != 0) {
		goto out;
	}

	/* Serialize the cert to DER. mbedtls_x509write_crt_der fills
	 * from the END of the buffer and returns the written length
	 * (or negative on failure). */
	int cert_written = mbedtls_x509write_crt_der(&crt, cert_buf,
			sizeof(cert_buf), mbedtls_ctr_drbg_random, &drbg);
	if (cert_written < 0) {
		goto out;
	}
	size_t cert_len = (size_t)cert_written;
	/* +1 / NUL-terminate for the same PEM-sniffer bounds reason
	 * noted below for the key buffer. */
	cert_heap = (uint8_t *)malloc(cert_len + 1);
	if (cert_heap == NULL) {
		goto out;
	}
	memcpy(cert_heap, cert_buf + sizeof(cert_buf) - cert_len, cert_len);
	cert_heap[cert_len] = '\0';

	/* Serialize the key to DER. mbedtls_pk_write_key_der is the
	 * same fill-from-end convention. */
	/* picoquic's mbedtls-backed private-key loader
	 * (ptls_mbedtls_load_private_key_from_buffer) expects PEM
	 * format — it calls mbedtls_pem_read_buffer looking for the
	 * "-----BEGIN ... PRIVATE KEY-----" / "-----END ... PRIVATE
	 * KEY-----" wrappers. Raw DER returns
	 * MBEDTLS_ERR_PK_KEY_INVALID_FORMAT. Write PEM here; the cert
	 * stays DER because picoquic_set_tls_certificate_chain passes
	 * it straight to picotls which serializes it on-wire (TLS
	 * certificates are DER-encoded on the wire). */
	int key_written = mbedtls_pk_write_key_pem(&key, key_buf,
			sizeof(key_buf));
	if (key_written != 0) {
		goto out;
	}
	/* PEM is a NUL-terminated ASCII blob; mbedtls_pk_write_key_pem
	 * returns 0 and fills from the START of the buffer. Length is
	 * the strlen to the first NUL. */
	size_t key_len = strlen((const char *)key_buf);
	/* Allocate +1 and copy the trailing NUL too — picotls's PEM
	 * sniffer reads `buf[len]` for a NUL presence check. */
	key_heap = (uint8_t *)malloc(key_len + 1);
	if (key_heap == NULL) {
		goto out;
	}
	memcpy(key_heap, key_buf, key_len + 1);

	*p_cert_der = cert_heap;
	*p_cert_len = cert_len;
	*p_key_der = key_heap;
	*p_key_len = key_len;
	cert_heap = NULL; /* ownership transferred */
	key_heap = NULL;
	rc = 0;

out:
	free(cert_heap);
	free(key_heap);
	mbedtls_x509write_crt_free(&crt);
	mbedtls_pk_free(&key);
	mbedtls_ctr_drbg_free(&drbg);
	mbedtls_entropy_free(&entropy);
	return rc;
}
