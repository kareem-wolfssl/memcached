#ifndef WOLFSSL_H
#define WOLFSSL_H

/* constant session ID context for application-level SSL session scoping.
 * used in server-side SSL session caching, when enabled. */
#define SESSION_ID_CONTEXT "memcached"
#define SSL_TYPE WOLFSSL
#define SSL_CTX_TYPE WOLFSSL_CTX

void SSL_LOCK(void);
void SSL_UNLOCK(void);
ssize_t ssl_read(conn *c, void *buf, size_t count);
ssize_t ssl_sendmsg(conn *c, struct msghdr *msg, int flags);
ssize_t ssl_write(conn *c, void *buf, size_t count);

int ssl_init(void);
bool refresh_certs(char **errmsg);
void ssl_callback(const WOLFSSL *s, int where, int ret);
int ssl_new_session_callback(WOLFSSL *s, WOLFSSL_SESSION *sess);
const char *ssl_proto_text(int version);

#endif
