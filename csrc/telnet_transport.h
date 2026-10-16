#ifndef TELNET_TRANSPORT_H
#define TELNET_TRANSPORT_H
#include "collector.h"

typedef struct TelnetTransport TelnetTransport;
#define TELNET_READ_AGAIN (-2)
TelnetTransport *telnet_transport_new(SOCKET socket,const Options *options,const Device *device);
int telnet_transport_read(TelnetTransport *transport,char *buffer,size_t size);
int telnet_transport_send(TelnetTransport *transport,const char *data,size_t size,uint64_t deadline);
int telnet_transport_pending(TelnetTransport *transport);
const char *telnet_transport_error(TelnetTransport *transport);
void telnet_transport_free(TelnetTransport *transport);
#endif
