/*
 * $Id: dtls.h $
 *
 * Author: Markus Stenberg <markus stenberg@iki.fi>
 *
 * Copyright (c) 2014 cisco Systems, Inc.
 *
 * Created:       Thu Oct 16 10:50:18 2014 mstenber
 * Last modified: Thu Nov  6 11:09:22 2014 mstenber
 * Edit time:     18 min
 *
 */

#ifndef DTLS_H
#define DTLS_H

#include "hnetd.h"

#include <netinet/in.h>

/*
 * This is 'dtls' module, which hides most of the ugliness of OpenSSL
 * (DTLS) API with simple, socket-like API.
 *
 * It assumes uloop is available to juggle the various file
 * descriptors.
 *
 * The API is more or less same as what hncp_io provides; however,
 * underneath, a number of sockets are juggled.
 *
 * Basic usage:
 * 1. create
 * 2. .. configure things (psk/cert, callbacks, ..)
 * 3. start
 * 4. .. do things .. (send, or recvfrom as needed, possibly triggered by cb)
 * 5. destroy
 */

typedef struct dtls_struct *dtls;
typedef void (*dtls_readable_callback)(dtls d, void *context);
typedef bool (*dtls_unknown_callback)(dtls d, const char *pem_x509, void *context);

/* Create/destroy instance. */
dtls dtls_create(uint16_t port);
void dtls_start();
void dtls_destroy(dtls d);

/* Callback to call when dtls has new data. */
void dtls_set_readable_callback(dtls d, dtls_readable_callback cb, void *cb_context);

/* Authentication scheme 1 - PSK */

/* Set 'global' pre-shared key to use / expect other side to use. */
bool dtls_set_psk(dtls d, const char *psk, size_t psk_len);

/* Authentication scheme 2/3 shared requirement - local side setup */

/* Set local authentication information */
bool dtls_set_local_cert(dtls d, const char *certfile, const char *pkfile);

/* Authentication scheme 2 - PKI approach - provide either single file
 * with trusted cert(s), or directory with trusted certs. */
bool dtls_set_verify_locations(dtls d, const char *path, const char *dir);

/* Authentication scheme 3 - instead of using PKI, declare verdicts on
 * certificates on our own. The return value of 'true' from the
 * callback indicates we trust a certificate. */
void dtls_set_unknown_cert_callback(dtls d, dtls_unknown_callback cb, void *cb_context);



/* Send/receive data. */
ssize_t dtls_recvfrom(dtls d, void *buf, size_t len,
                      struct sockaddr_in6 *src);
ssize_t dtls_sendto(dtls o, void *buf, size_t len,
                    const struct sockaddr_in6 *dst);

#endif /* DTLS_H */
