#include "memcached.h"

#ifdef WOLFSSL_MEMCACHED

#include "wolfssl.h"
#include <string.h>
#include <sysexits.h>
#include <sys/param.h>
#include <poll.h>
#ifndef WOLFSSL_USER_SETTINGS
#include <wolfssl/options.h>
#endif
#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/ssl.h>

#ifndef MAXPATHLEN
#define MAXPATHLEN 4096
#endif

static pthread_mutex_t wolfssl_ctx_lock = PTHREAD_MUTEX_INITIALIZER;

const unsigned ERROR_MSG_SIZE = 64;
const size_t SSL_ERROR_MSG_SIZE = 256;
const unsigned MAX_RETRY_COUNT = 5;

void SSL_LOCK() {
    pthread_mutex_lock(&(wolfssl_ctx_lock));
}

void SSL_UNLOCK(void) {
    pthread_mutex_unlock(&(wolfssl_ctx_lock));
}

/*
 * Reads decrypted data from the underlying BIO read buffers,
 * which reads from the socket.
 */
ssize_t ssl_read(conn *c, void *buf, size_t count) {
    int ret = -1, err = 0;
    struct pollfd to_poll[1];
    unsigned retry = 0;

    assert (c != NULL);
    /* TODO : document the state machine interactions for SSL_read with
        non-blocking sockets/ SSL re-negotiations
    */

    do {
        ret = wolfSSL_read(c->ssl, buf, count);
        err = wolfSSL_get_error(c->ssl, ret);
        if (err == WOLFSSL_ERROR_WANT_READ) {
            to_poll[0].fd = c->sfd;
            to_poll[0].events = POLLIN;
            poll(to_poll, 1, 500);
        }
        retry++;
    } while (err == WOLFSSL_ERROR_WANT_READ && retry < MAX_RETRY_COUNT);
    if (err == WOLFSSL_ERROR_WANT_READ) {
        ret = -1;
        errno = EWOULDBLOCK;
    }

    return ret;
}

/*
 * SSL sendmsg implementation. Perform a SSL_write.
 */
ssize_t ssl_sendmsg(conn *c, struct msghdr *msg, int flags) {
    assert (c != NULL);
    size_t buf_remain = settings.ssl_wbuf_size;
    size_t bytes = 0;
    size_t to_copy;
    int i;

    // ssl_wbuf is pointing to the buffer allocated in the worker thread.
    assert(c->ssl_wbuf);
    // TODO: allocate a fix buffer in crawler/logger if they start using
    // the sendmsg method. Also, set c->ssl_wbuf  when the side thread
    // start owning the connection and reset the pointer in
    // conn_worker_readd.
    // Currently this connection would not be served by a different thread
    // than the one it's assigned.
    assert(pthread_equal(c->thread->thread_id, pthread_self()) != 0);

    char *bp = c->ssl_wbuf;
    for (i = 0; i < msg->msg_iovlen; i++) {
        size_t len = msg->msg_iov[i].iov_len;
        to_copy = len < buf_remain ? len : buf_remain;

        memcpy(bp + bytes, (void*)msg->msg_iov[i].iov_base, to_copy);
        buf_remain -= to_copy;
        bytes += to_copy;
        if (buf_remain == 0)
            break;
    }
    /* TODO : document the state machine interactions for SSL_write with
        non-blocking sockets/ SSL re-negotiations
    */
    return ssl_write(c, c->ssl_wbuf, bytes);
}

/*
 * Writes data to the underlying BIO write buffers,
 * which encrypt and write them to the socket.
 */
ssize_t ssl_write(conn *c, void *buf, size_t count) {
    int ret = -1, err = 0;
    unsigned retry = 0;

    assert (c != NULL);

    do {
        ret = wolfSSL_write(c->ssl, buf, count);
        err = wolfSSL_get_error(c->ssl, ret);
        if (err == WOLFSSL_ERROR_WANT_WRITE) {
            usleep(500);
        }
        retry++;
    } while (err == WOLFSSL_ERROR_WANT_WRITE && retry < MAX_RETRY_COUNT);
    if (err == WOLFSSL_ERROR_WANT_WRITE) {
        errno = EWOULDBLOCK;
        ret = -1;
    }

    return ret;
}

/*
 * Prints an SSL error into the buff, if there's any.
 */
static void print_ssl_error(char *buff, size_t len) {
    unsigned long err;
    if ((err = wolfSSL_ERR_get_error()) != 0) {
        wolfSSL_ERR_error_string_n(err, buff, len);
    }
}

/*
 * Loads server certificates to the SSL context and validate them.
 * @return whether certificates are successfully loaded and verified or not.
 * @param error_msg contains the error when unsuccessful.
 */
static bool load_server_certificates(char **errmsg) {
    bool success = false;

    const size_t CRLF_NULLCHAR_LEN = 3;
    char *error_msg = malloc(MAXPATHLEN + ERROR_MSG_SIZE +
        SSL_ERROR_MSG_SIZE);
    size_t errmax = MAXPATHLEN + ERROR_MSG_SIZE + SSL_ERROR_MSG_SIZE -
        CRLF_NULLCHAR_LEN;

    if (error_msg == NULL) {
        *errmsg = NULL;
        return false;
    }

    if (settings.ssl_ctx == NULL) {
        snprintf(error_msg, errmax, "Error TLS not enabled\r\n");
        *errmsg = error_msg;
        return false;
    }

    char *ssl_err_msg = malloc(SSL_ERROR_MSG_SIZE);
    if (ssl_err_msg == NULL) {
        free(error_msg);
        *errmsg = NULL;
        return false;
    }
    bzero(ssl_err_msg, SSL_ERROR_MSG_SIZE);
    size_t err_msg_size = 0;

    SSL_LOCK();
    if (!wolfSSL_CTX_use_certificate_chain_file(settings.ssl_ctx,
        settings.ssl_chain_cert)) {
        print_ssl_error(ssl_err_msg, SSL_ERROR_MSG_SIZE);
        err_msg_size = snprintf(error_msg, errmax, "Error loading the certificate chain: "
            "%s : %s", settings.ssl_chain_cert, ssl_err_msg);
    } else if (!wolfSSL_CTX_use_PrivateKey_file(settings.ssl_ctx, settings.ssl_key,
                                        settings.ssl_keyformat)) {
        print_ssl_error(ssl_err_msg, SSL_ERROR_MSG_SIZE);
        err_msg_size = snprintf(error_msg, errmax, "Error loading the key: %s : %s",
            settings.ssl_key, ssl_err_msg);
    } else if (!wolfSSL_CTX_check_private_key(settings.ssl_ctx)) {
        print_ssl_error(ssl_err_msg, SSL_ERROR_MSG_SIZE);
        err_msg_size = snprintf(error_msg, errmax, "Error validating the certificate: %s",
            ssl_err_msg);
    } else if (settings.ssl_ca_cert) {
        if (!wolfSSL_CTX_load_verify_locations(settings.ssl_ctx,
          settings.ssl_ca_cert, NULL)) {
            print_ssl_error(ssl_err_msg, SSL_ERROR_MSG_SIZE);
            err_msg_size = snprintf(error_msg, errmax,
              "Error loading the CA certificate: %s : %s",
              settings.ssl_ca_cert, ssl_err_msg);
        } else {
            /*wolfSSL_CTX_set_client_CA_list(settings.ssl_ctx,
              wolfSSL_load_client_CA_file(settings.ssl_ca_cert));*/
            fprintf(stderr, "Warning: wolfSSL does not currently support setting client CA list.\n");
            success = true;
        }
    } else {
        success = true;
    }
    SSL_UNLOCK();
    free(ssl_err_msg);
    if (success) {
        settings.ssl_last_cert_refresh_time = current_time;
        free(error_msg);
    } else {
        *errmsg = error_msg;
        error_msg += (err_msg_size >= errmax ? errmax - 1: err_msg_size);
        snprintf(error_msg, CRLF_NULLCHAR_LEN, "\r\n");
        // Print if there are more errors and drain the queue.
        wc_ERR_print_errors_fp(stderr);
    }
    return success;
}

/*
 * Verify SSL settings and initiates the SSL context.
 */
int ssl_init(void) {
    assert(settings.ssl_enabled);

    if (wolfSSL_Init() != WOLFSSL_SUCCESS) {
        fprintf(stderr, "Failed to initialize wolfSSL.");
        exit(EX_SOFTWARE);
    }

    // SSL context for the process. All connections will share one
    // process level context.
    settings.ssl_ctx = wolfSSL_CTX_new(wolfTLS_server_method());

    wolfSSL_CTX_SetMinVersion(settings.ssl_ctx, settings.ssl_min_version);

    // The server certificate, private key and validations.
    char *error_msg;
    if (!load_server_certificates(&error_msg)) {
        fprintf(stderr, "%s", error_msg);
        free(error_msg);
        exit(EX_USAGE);
    }

    // The verification mode of client certificate, default is SSL_VERIFY_PEER.
    wolfSSL_CTX_set_verify(settings.ssl_ctx, settings.ssl_verify_mode, NULL);
    if (settings.ssl_ciphers && !wolfSSL_CTX_set_cipher_list(settings.ssl_ctx,
                                                    settings.ssl_ciphers)) {
        fprintf(stderr, "Error setting the provided cipher(s): %s\n",
                settings.ssl_ciphers);
        exit(EX_USAGE);
    }

    // Optional session caching; default disabled.
    if (settings.ssl_session_cache) {
        wolfSSL_CTX_sess_set_new_cb(settings.ssl_ctx, ssl_new_session_callback);
        wolfSSL_CTX_set_session_cache_mode(settings.ssl_ctx, WOLFSSL_SESS_CACHE_SERVER);
        wolfSSL_CTX_set_session_id_context(settings.ssl_ctx,
                                           (const unsigned char *) SESSION_ID_CONTEXT,
                                           strlen(SESSION_ID_CONTEXT));
    } else {
        wolfSSL_CTX_set_session_cache_mode(settings.ssl_ctx, SSL_SESS_CACHE_OFF);
    }

    // wolfSSL does not support kTLS.

    // wolfSSL does not support disabling renegotiation at runtime.
    // It is off by default, and must be enabled by the calling application
    // or it is not used.

    // Release TLS read/write buffers of idle connections
    // wolfSSL does not currently support SSL_MODE_RELEASE_BUFFERS.
    /*wolfSSL_CTX_set_mode(settings.ssl_ctx, SSL_MODE_RELEASE_BUFFERS);*/

    return 0;
}

/*
 * This method is invoked with every new successfully negotiated SSL session,
 * when server-side session caching is enabled. Note that this method is not
 * invoked when a session is reused.
 */
int ssl_new_session_callback(WOLFSSL *s, WOLFSSL_SESSION *sess) {
    STATS_LOCK();
    stats.ssl_new_sessions++;
    STATS_UNLOCK();

    return 0;
}

bool refresh_certs(char **errmsg) {
    int ret = -1;

    ret = wolfSSL_CTX_UnloadCAs(settings.ssl_ctx);
    if (ret != WOLFSSL_SUCCESS) {
        fprintf(stderr, "Error unloading CA certs: %s.\n",
                wolfSSL_ERR_reason_error_string(ret));
        return false;
    }

    return load_server_certificates(errmsg);
}

const char *ssl_proto_text(int version) {
    switch (version) {
        case WOLFSSL_TLSV1:
            return "tlsv1.0";
        case WOLFSSL_TLSV1_1:
            return "tlsv1.1";
        case WOLFSSL_TLSV1_2:
            return "tlsv1.2";
#if defined(TLS1_3_VERSION)
        case WOLFSSL_TLSV1_3:
            return "tlsv1.3";
#endif
        default:
            return "unknown";
    }
}
#endif
