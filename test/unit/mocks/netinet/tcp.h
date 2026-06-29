/* Host test mock for netinet/tcp.h: forward to the system header.
 * (Quoted include in ssh_client.cpp is shadowed by the mocks dir.) */
#ifndef MOCK_NETINET_TCP_H
#define MOCK_NETINET_TCP_H

#include_next <netinet/tcp.h>

#endif
