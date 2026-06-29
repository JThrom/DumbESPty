/* Host test mock of lwip/sockets.h.
 *
 * ssh_client.cpp writes `struct fd_set` (lwip's fd_set is a real struct tag,
 * unlike glibc's typedef). To reconcile, we rename the token `fd_set` to
 * `lwip_fd_set` for the translation units that include this mock, then define
 * `struct lwip_fd_set` + FD_* macros ourselves. The socket/select paths are
 * not exercised by the unit tests; this only needs to compile and link. */
#ifndef MOCK_LWIP_SOCKETS_H
#define MOCK_LWIP_SOCKETS_H

/* Pull in real host socket types BEFORE the rename so sockaddr_in etc. exist
 * with their normal definitions. */
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>

/* From here on, any `fd_set` token (including `struct fd_set` in firmware
 * source) maps to our lwip-style struct tag. */
#define fd_set lwip_fd_set

#ifdef __cplusplus
extern "C" {
#endif

#ifndef LWIP_MOCK_FD_SETSIZE
#define LWIP_MOCK_FD_SETSIZE 64
#endif

struct lwip_fd_set {
    unsigned char fds_bits[(LWIP_MOCK_FD_SETSIZE + 7) / 8];
};

#undef FD_ZERO
#undef FD_SET
#undef FD_CLR
#undef FD_ISSET
#define FD_ZERO(s)      memset((s), 0, sizeof(*(s)))
#define FD_SET(n, s)    ((s)->fds_bits[(n) / 8] |= (unsigned char)(1u << ((n) % 8)))
#define FD_CLR(n, s)    ((s)->fds_bits[(n) / 8] &= (unsigned char)~(1u << ((n) % 8)))
#define FD_ISSET(n, s)  (((s)->fds_bits[(n) / 8] & (1u << ((n) % 8))) != 0)

int lwip_select(int maxfdp1, struct lwip_fd_set *readset, struct lwip_fd_set *writeset,
                struct lwip_fd_set *exceptset, struct timeval *timeout);
#define select(n, r, w, e, t) lwip_select((n), (r), (w), (e), (t))

#ifdef __cplusplus
}
#endif

#endif
