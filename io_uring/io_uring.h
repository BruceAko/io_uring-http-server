#ifndef IO_URING_H
#define IO_URING_H

#include <ctype.h>
#include <liburing.h>
#include <sys/utsname.h>

#include <stdlib.h>
#include <stdio.h>

#define QUEUE_DEPTH 8192
#define EVENT_TYPE_ACCEPT 0
#define EVENT_TYPE_READ 1
#define EVENT_TYPE_WRITE 2

struct request
{
    int event_type;
    int iovec_count;
    int client_socket;
    struct iovec iov[];
};

// void fatal_error(const char *syscall)
// {
//     perror(syscall);
//     exit(1);
// }

#endif