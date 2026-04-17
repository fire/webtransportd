#!/bin/bash
set -e

CFLAGS="-O0 -g -Wall -Wextra -std=c11 -D_GNU_SOURCE -fsanitize=address,undefined -fno-omit-frame-pointer"
INCLUDES="-isystem thirdparty/picoquic/picoquic -isystem thirdparty/picoquic/picohttp -isystem thirdparty/picotls/include -isystem thirdparty/mbedtls/include"
DEFINES="-DPICOQUIC_WITH_MBEDTLS=1 -DPTLS_WITHOUT_OPENSSL=1 -DPTLS_WITHOUT_FUSION=1 -DPICOQUIC_WITHOUT_SSLKEYLOG=1 -DDISABLE_DEBUG_PRINTF=1"

# Compile minimal HTTP/3 server
gcc $CFLAGS $INCLUDES $DEFINES -pthread \
  -o minimal_http3_server minimal_http3_server.c \
  thirdparty/picoquic/picoquic/*.o \
  thirdparty/picoquic/picohttp/*.o \
  thirdparty/picotls/lib/*.o \
  thirdparty/picotls/lib/cifra/*.o \
  thirdparty/picotls/deps/cifra/src/*.o \
  thirdparty/picotls/deps/micro-ecc/*.o \
  thirdparty/picoquic/picoquic_mbedtls/*.o \
  thirdparty/mbedtls/library/*.o \
  thirdparty/picoquic/loglib/*.o \
  -pthread -fsanitize=address,undefined

# Compile minimal HTTP/3 client  
gcc $CFLAGS $INCLUDES $DEFINES -pthread \
  -o minimal_http3_client minimal_http3_client.c \
  thirdparty/picoquic/picoquic/*.o \
  thirdparty/picoquic/picohttp/*.o \
  thirdparty/picotls/lib/*.o \
  thirdparty/picotls/lib/cifra/*.o \
  thirdparty/picotls/deps/cifra/src/*.o \
  thirdparty/picotls/deps/micro-ecc/*.o \
  thirdparty/picoquic/picoquic_mbedtls/*.o \
  thirdparty/mbedtls/library/*.o \
  thirdparty/picoquic/loglib/*.o \
  -pthread -fsanitize=address,undefined
  
echo "Compiled minimal HTTP/3 client and server"
