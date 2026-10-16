#include "telnet_transport.h"
#include <libtelnet.h>
#include <stdlib.h>
#include <string.h>

struct TelnetTransport {
    SOCKET socket;
    const Options *options;
    const Device *device;
    telnet_t *parser;
    Buffer input,output;
    size_t input_at,output_at;
    int eof;
    char error[256];
};
static const telnet_telopt_t telopts[]={
    {TELNET_TELOPT_ECHO,TELNET_WONT,TELNET_DO},
    {TELNET_TELOPT_SGA,TELNET_WILL,TELNET_DO},
    {TELNET_TELOPT_TTYPE,TELNET_WILL,TELNET_DONT},
    {TELNET_TELOPT_NAWS,TELNET_WILL,TELNET_DONT},
    {TELNET_TELOPT_BINARY,TELNET_WILL,TELNET_DO},
    {-1,0,0}
};
static void event(telnet_t *parser,telnet_event_t *event,void *context) {
    TelnetTransport *t=context;if(*t->error) return;
    switch(event->type) {
    case TELNET_EV_DATA:
        if(t->input.len+event->data.size>1048576) snprintf(t->error,sizeof(t->error),"Telnet receive queue exceeds 1 MiB");
        else buf_add(&t->input,event->data.buffer,event->data.size);
        break;
    case TELNET_EV_SEND:
        if(t->output.len+event->data.size>1048576) snprintf(t->error,sizeof(t->error),"Telnet send queue exceeds 1 MiB");
        else buf_add(&t->output,event->data.buffer,event->data.size);
        break;
    case TELNET_EV_TTYPE:
        if(event->ttype.cmd==TELNET_TTYPE_SEND) telnet_ttype_is(parser,"VT100");
        break;
    case TELNET_EV_DO:
        if(event->neg.telopt==TELNET_TELOPT_NAWS) { const char dimensions[]={0,(char)240,3,(char)232};telnet_subnegotiation(parser,TELNET_TELOPT_NAWS,dimensions,4); }
        report_at(t->options,LOG_DEBUG,"[%s:%u] TELNET_NEGOTIATION DO option=%u",t->device->ip,t->device->port,event->neg.telopt);
        break;
    case TELNET_EV_WILL:
        report_at(t->options,LOG_DEBUG,"[%s:%u] TELNET_NEGOTIATION WILL option=%u",t->device->ip,t->device->port,event->neg.telopt);break;
    case TELNET_EV_WARNING:
        report_at(t->options,LOG_WARN,"[%s:%u] TELNET_PROTOCOL_WARNING %s",t->device->ip,t->device->port,event->error.msg);break;
    case TELNET_EV_ERROR:
        snprintf(t->error,sizeof(t->error),"Telnet protocol error: %.200s",event->error.msg);break;
    default:break;
    }
}
static int flush(TelnetTransport *t,uint64_t deadline) {
    while(t->output_at<t->output.len && !*t->error) {
        if(stopped(t->options)) { snprintf(t->error,sizeof(t->error),"Cancelled");return 0; }
        int n=send(t->socket,t->output.data+t->output_at,(int)(t->output.len-t->output_at),0);
        if(n>0) { t->output_at+=(size_t)n;continue; }
        int error=WSAGetLastError();
        if(n==SOCKET_ERROR && error==WSAEWOULDBLOCK) {
            if(!deadline) return 1;
            if(tick_ms()>=deadline) { snprintf(t->error,sizeof(t->error),"Telnet send timed out");return 0; }
            fd_set writable;FD_ZERO(&writable);FD_SET(t->socket,&writable);struct timeval tv={0,100000};
            if(select(0,NULL,&writable,NULL,&tv)<0) { snprintf(t->error,sizeof(t->error),"Telnet send wait failed (WinSock=%d)",WSAGetLastError());return 0; }
        } else { snprintf(t->error,sizeof(t->error),"Telnet send failed (WinSock=%d)",error);return 0; }
    }
    if(t->output_at==t->output.len) {
        if(t->output.data) { SecureZeroMemory(t->output.data,t->output.len);t->output.data[0]=0; }
        t->output.len=t->output_at=0;
    }
    return !*t->error;
}
TelnetTransport *telnet_transport_new(SOCKET socket,const Options *options,const Device *device) {
    TelnetTransport *t=mem_alloc(sizeof(*t));t->socket=socket;t->options=options;t->device=device;
    t->parser=telnet_init(telopts,event,TELNET_FLAG_NVT_EOL,t);
    if(!t->parser) { free(t);return NULL; }
    telnet_negotiate(t->parser,TELNET_DO,TELNET_TELOPT_SGA);
    telnet_negotiate(t->parser,TELNET_WILL,TELNET_TELOPT_TTYPE);
    telnet_negotiate(t->parser,TELNET_WILL,TELNET_TELOPT_NAWS);
    return t;
}
int telnet_transport_read(TelnetTransport *t,char *buffer,size_t size) {
    if(!flush(t,0)) return -1;
    if(t->input_at==t->input.len) {
        t->input_at=t->input.len=0;if(t->input.data) t->input.data[0]=0;
        if(t->eof) return 0;
        char block[32768];int n=recv(t->socket,block,sizeof(block),0);
        if(n==0) { t->eof=1;return 0; }
        if(n==SOCKET_ERROR) {
            int error=WSAGetLastError();if(error==WSAEWOULDBLOCK) return TELNET_READ_AGAIN;
            snprintf(t->error,sizeof(t->error),"Telnet receive failed (WinSock=%d)",error);return -1;
        }
        telnet_recv(t->parser,block,(size_t)n);
        if(!flush(t,0) || *t->error) return -1;
    }
    size_t available=t->input.len-t->input_at;if(!available) return TELNET_READ_AGAIN;
    if(available>size) available=size;
    memcpy(buffer,t->input.data+t->input_at,available);t->input_at+=available;return (int)available;
}
int telnet_transport_send(TelnetTransport *t,const char *data,size_t size,uint64_t deadline) {
    if(*t->error || t->eof) return 0;
    telnet_send(t->parser,data,size);return flush(t,deadline);
}
int telnet_transport_pending(TelnetTransport *t) { return t && t->output_at<t->output.len; }
const char *telnet_transport_error(TelnetTransport *t) { return t->error; }
void telnet_transport_free(TelnetTransport *t) {
    if(!t) return;telnet_free(t->parser);
    if(t->output.data) SecureZeroMemory(t->output.data,t->output.len);
    if(t->input.data) SecureZeroMemory(t->input.data,t->input.len);
    buf_free(&t->input);buf_free(&t->output);free(t);
}
