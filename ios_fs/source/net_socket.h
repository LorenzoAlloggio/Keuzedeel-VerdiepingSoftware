#ifndef NET_SOCKET_H
#define NET_SOCKET_H

#include "types.h"

int net_socket_connect(u32 ipv4_address, u16 port);
int net_socket_send_all(int socket_fd, const void *data, u32 size);
int net_socket_close(int socket_fd);

#endif
