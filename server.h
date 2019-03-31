#ifndef SERVER_H
#define	SERVER_H
#include "config.h"
#define SERVER_STRING "Server:A Simple Http Server!\n"

bool server_init(struct Gthread_pool * pool);
bool server_close(struct Gthread_pool * pool);
void request_handle(struct Gthread_pool * pool);

#endif
