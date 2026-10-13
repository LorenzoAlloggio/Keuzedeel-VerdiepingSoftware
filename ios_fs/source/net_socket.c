#include <stddef.h>

#include "imports.h"
#include "net_socket.h"
#include "svc.h"

#define SOCKET_IOCTL_CLOSE   0x03
#define SOCKET_IOCTL_CONNECT 0x04
#define SOCKET_IOCTL_SEND    0x0E
#define SOCKET_IOCTL_SOCKET  0x11
#define MAX_SOCKET_SEND_SIZE 0x8000

static int socket_service;
static const char socket_path[] = "/dev/socket";

static void *alloc_socket_buffer(u32 size)
{
    void *buffer = svcAlloc(0xCAFF, size);
    if (buffer)
    {
        FS_MEMSET(buffer, 0, size);
    }
    return buffer;
}

int net_socket_connect(u32 ipv4_address, u16 port)
{
    socket_service = svcOpen((char *)socket_path, 0);
    if (socket_service <= 0)
    {
        return socket_service;
    }

    u32 socket_args[3];
    socket_args[0] = 2;
    socket_args[1] = 1;
    socket_args[2] = 6;
    int socket_fd = svcIoctl(
        socket_service, SOCKET_IOCTL_SOCKET, socket_args, sizeof(socket_args), NULL, 0);
    if (socket_fd < 0)
    {
        svcClose(socket_service);
        socket_service = 0;
        return socket_fd;
    }

    u32 connect_args[6];
    connect_args[0] = (u32)socket_fd;
    connect_args[1] = (2u << 16) | port;
    connect_args[2] = ipv4_address;
    connect_args[3] = 0;
    connect_args[4] = 0;
    connect_args[5] = 0x10;
    int result = svcIoctl(
        socket_service, SOCKET_IOCTL_CONNECT, connect_args, sizeof(connect_args), NULL, 0);
    if (result < 0)
    {
        u32 close_arg = (u32)socket_fd;
        svcIoctl(socket_service, SOCKET_IOCTL_CLOSE, &close_arg, sizeof(close_arg), NULL, 0);
        svcClose(socket_service);
        socket_service = 0;
        return result;
    }

    return socket_fd;
}

static int net_socket_send(int socket_fd, const void *data, u32 size)
{
    void *aligned_data = svcAllocAlign(0xCAFF, size, 0x40);
    if (!aligned_data)
    {
        return -1;
    }
    FS_MEMCPY(aligned_data, data, size);

    u8 *io_buffer = alloc_socket_buffer(0x38);
    if (!io_buffer)
    {
        svcFree(0xCAFF, aligned_data);
        return -1;
    }

    iovec_s *vectors = (iovec_s *)io_buffer;
    u32 *send_args = (u32 *)(io_buffer + 0x30);
    send_args[0] = (u32)socket_fd;
    send_args[1] = 0;
    vectors[0].ptr = send_args;
    vectors[0].len = 0x8;
    vectors[1].ptr = aligned_data;
    vectors[1].len = size;

    int result = svcIoctlv(socket_service, SOCKET_IOCTL_SEND, 4, 0, vectors);
    svcFree(0xCAFF, io_buffer);
    svcFree(0xCAFF, aligned_data);
    return result;
}

int net_socket_send_all(int socket_fd, const void *data, u32 size)
{
    const u8 *bytes = (const u8 *)data;
    u32 sent = 0;

    while (sent < size)
    {
        u32 chunk_size = size - sent;
        if (chunk_size > MAX_SOCKET_SEND_SIZE)
        {
            chunk_size = MAX_SOCKET_SEND_SIZE;
        }

        int result = net_socket_send(socket_fd, bytes + sent, chunk_size);
        if (result <= 0)
        {
            return result < 0 ? result : -1;
        }
        sent += (u32)result;
    }
    return 0;
}

int net_socket_close(int socket_fd)
{
    int result = 0;
    if (socket_service > 0)
    {
        u32 close_arg = (u32)socket_fd;
        result = svcIoctl(
            socket_service, SOCKET_IOCTL_CLOSE, &close_arg, sizeof(close_arg), NULL, 0);
        int service_result = svcClose(socket_service);
        socket_service = 0;
        if (result >= 0 && service_result < 0)
        {
            result = service_result;
        }
    }
    return result;
}
