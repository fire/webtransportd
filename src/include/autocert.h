/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
/*
 * webtransportd — autocert.h
 *
 * Generate a self-signed TLS cert + key pair in memory. Lets the
 * daemon boot with `--cert=auto` instead of requiring a PEM pair
 * on disk — useful for dev loops, selftest, and CI. The caller
 * owns both buffers and must free them (both with free()).
 *
 * Cert is ECDSA over secp256r1, CN=webtransportd-autocert, validity
 * 30 days starting "now", SAN covers DNS:localhost + IP:127.0.0.1
 * + IP:::1 so a browser can connect to the loopback daemon without
 * hostname-verification gymnastics. Output is DER — picoquic's
 * `picoquic_set_tls_certificate_chain` / `picoquic_set_tls_key`
 * accept DER directly.
 *
 * Not a CA-signed cert — peers must either pin this cert or run
 * with certificate-verification disabled. For production the
 * operator supplies --cert= and --key= paths.
 */

#ifndef WEBTRANSPORTD_AUTOCERT_H
#define WEBTRANSPORTD_AUTOCERT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Allocate and fill two heap buffers with DER-encoded cert and
 * key. *p_cert_der must be free()'d by the caller on success;
 * likewise *p_key_der. Returns 0 on success, -1 on any failure
 * (entropy init, keygen, cert write). On failure the out-pointers
 * are left unchanged / NULL. */
int wtd_autocert_generate(uint8_t **p_cert_der, size_t *p_cert_len,
		uint8_t **p_key_der, size_t *p_key_len);

#ifdef __cplusplus
}
#endif

#endif /* WEBTRANSPORTD_AUTOCERT_H */
