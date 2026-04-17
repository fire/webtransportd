# Design: --cert=auto

## Goal
Allow the daemon to generate a self-signed cert+key in-memory via mbedtls, so it boots without a PEM pair on disk.

## Steps
- Use `mbedtls_x509write_crt_*` and `mbedtls_pk_write_key_der` to generate cert+key in memory.
- Integrate with `picoquic_set_tls_certificate_chain` and `picoquic_set_tls_key`.
- Add `--cert=auto` CLI flag.
- (Follow-up) Support persistence: save generated cert+key to survive restarts.
- Major UX win for first-time operators.
