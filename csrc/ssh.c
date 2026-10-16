#include "collector.h"
#include "telnet_transport.h"
#include <libssh2.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>
#include <ctype.h>

typedef struct { int status; size_t completed; char *name; char error[512]; } Result;
typedef struct {
    const Options *o; Inventory *inv; Result *results;
    volatile LONG next;
    CRITICAL_SECTION host_lock;
} Pool;
typedef struct {
    SOCKET socket;LIBSSH2_SESSION *session;LIBSSH2_CHANNEL *channel;TelnetTransport *telnet;
    const Options *o;const Device *device;int network_error,user_sent,password_sent,continue_sent;
} Connection;
static int socket_wait(Connection *c,uint64_t deadline,int writing) {
    while(tick_ms()<deadline && !stopped(c->o)) {
        fd_set readfds,writefds; FD_ZERO(&readfds);FD_ZERO(&writefds);
        int direction=c->session?libssh2_session_block_directions(c->session):0;
        if(writing || (direction & LIBSSH2_SESSION_BLOCK_OUTBOUND)) FD_SET(c->socket,&writefds);
        if(!writing || (direction & LIBSSH2_SESSION_BLOCK_INBOUND)) FD_SET(c->socket,&readfds);
        struct timeval tv={0,100000}; int rc=select(0,&readfds,&writefds,NULL,&tv);
        if(rc>0) return 1;if(rc<0) { c->network_error=WSAGetLastError();return 0; }
    }
    c->network_error=stopped(c->o)?WSAEINTR:WSAETIMEDOUT;
    return 0;
}
static int connect_tcp(Connection *c) {
    char port[12];snprintf(port,sizeof(port),"%u",c->device->port);
    struct addrinfo hints={0},*list=NULL; hints.ai_socktype=SOCK_STREAM;hints.ai_family=AF_UNSPEC;
    c->network_error=getaddrinfo(c->device->ip,port,&hints,&list);if(c->network_error) return 0;
    uint64_t deadline=tick_ms()+12000; int ok=0;
    for(struct addrinfo *a=list;a && tick_ms()<deadline && !stopped(c->o);a=a->ai_next) {
        c->socket=socket(a->ai_family,a->ai_socktype,a->ai_protocol);
        if(c->socket==INVALID_SOCKET) { c->network_error=WSAGetLastError();continue; }
        u_long nonblocking=1;ioctlsocket(c->socket,FIONBIO,&nonblocking);
        int rc=connect(c->socket,a->ai_addr,(int)a->ai_addrlen);
        c->network_error=rc?WSAGetLastError():0;
        if(!rc || c->network_error==WSAEWOULDBLOCK) {
            if(!rc || socket_wait(c,deadline,1)) {
                int error=0,len=sizeof(error);
                if(getsockopt(c->socket,SOL_SOCKET,SO_ERROR,(char*)&error,&len)) c->network_error=WSAGetLastError();
                else { c->network_error=error;if(!error) { ok=1;break; } }
            }
        }
        closesocket(c->socket);c->socket=INVALID_SOCKET;
    }
    freeaddrinfo(list); if(ok) { BOOL yes=TRUE;setsockopt(c->socket,IPPROTO_TCP,TCP_NODELAY,(char*)&yes,sizeof(yes)); } return ok;
}
/* Every EAGAIN retry shares one deadline; no busy polling or unbounded SSH waits. */
#define SSH_RETRY(target, expression, conn, deadline) do { \
    while(((target)=(expression))==LIBSSH2_ERROR_EAGAIN) \
        if(!socket_wait((conn),(deadline),0)) { (target)=LIBSSH2_ERROR_TIMEOUT;break; } \
} while(0)

static void keyboard(const char *name,int name_len,const char *instruction,int instruction_len,int count,
                     const LIBSSH2_USERAUTH_KBDINT_PROMPT *prompts,LIBSSH2_USERAUTH_KBDINT_RESPONSE *responses,void **abstract) {
    const Device *d=*abstract;
    if(count==1 && !prompts[0].echo) { responses[0].text=str_copy(d->password);responses[0].length=(unsigned)strlen(d->password); }
}
static int host_check(Pool *pool,Connection *c,char *error,size_t error_size) {
    size_t len;int type;const char *key=libssh2_session_hostkey(c->session,&len,&type);
    int kind=0;
    switch(type) {
    case LIBSSH2_HOSTKEY_TYPE_RSA:kind=LIBSSH2_KNOWNHOST_KEY_SSHRSA;break;
    case LIBSSH2_HOSTKEY_TYPE_DSS:kind=LIBSSH2_KNOWNHOST_KEY_SSHDSS;break;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256:kind=LIBSSH2_KNOWNHOST_KEY_ECDSA_256;break;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384:kind=LIBSSH2_KNOWNHOST_KEY_ECDSA_384;break;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521:kind=LIBSSH2_KNOWNHOST_KEY_ECDSA_521;break;
    case LIBSSH2_HOSTKEY_TYPE_ED25519:kind=LIBSSH2_KNOWNHOST_KEY_ED25519;break;
    }
    if(!key || !kind) { snprintf(error,error_size,"Unsupported host key type");return 0; }
    wchar_t *path=join_path(pool->o->output,"_known_hosts");
    int ok=0;EnterCriticalSection(&pool->host_lock);
    LIBSSH2_KNOWNHOSTS *hosts=libssh2_knownhost_init(c->session);
    if(!hosts) { LeaveCriticalSection(&pool->host_lock);free(path);snprintf(error,error_size,"Cannot initialize known hosts");return 0; }
    FILE *f=_wfopen(path,L"rb");
    if(f) {
        char line[16384];
        while(fgets(line,sizeof(line),f)) {
            if(libssh2_knownhost_readline(hosts,line,strlen(line),LIBSSH2_KNOWNHOST_FILE_OPENSSH)) { fclose(f);f=NULL;snprintf(error,error_size,"Invalid _known_hosts file");goto finish; }
        }
        fclose(f);
    } else if(GetFileAttributesW(path)!=INVALID_FILE_ATTRIBUTES) { snprintf(error,error_size,"Cannot read _known_hosts");goto finish; }
    struct libssh2_knownhost *match=NULL;
    int mask=LIBSSH2_KNOWNHOST_TYPE_PLAIN|LIBSSH2_KNOWNHOST_KEYENC_RAW|kind;
    int rc=libssh2_knownhost_checkp(hosts,c->device->ip,(int)c->device->port,key,len,mask,&match);
    if(rc==LIBSSH2_KNOWNHOST_CHECK_MATCH) ok=1;
    else if(rc==LIBSSH2_KNOWNHOST_CHECK_NOTFOUND) {
        char endpoint[1024]; if(c->device->port==22) snprintf(endpoint,sizeof(endpoint),"%s",c->device->ip);
        else snprintf(endpoint,sizeof(endpoint),"[%s]:%u",c->device->ip,c->device->port);
        struct libssh2_knownhost *entry=NULL;
        if(libssh2_knownhost_addc(hosts,endpoint,NULL,key,len,"netcollect",10,mask,&entry)) { snprintf(error,error_size,"Cannot store host key");goto finish; }
        char line[16384];size_t written=0;
        if(libssh2_knownhost_writeline(hosts,entry,line,sizeof(line),&written,LIBSSH2_KNOWNHOST_FILE_OPENSSH)) { snprintf(error,error_size,"Cannot serialize host key");goto finish; }
        f=_wfopen(path,L"ab"); if(!f) { snprintf(error,error_size,"Cannot save _known_hosts");goto finish; }
        ok=fwrite(line,1,written,f)==written; if(fclose(f)) ok=0;
        if(!ok) snprintf(error,error_size,"Cannot save host key");
        else report(pool->o,"[%s:%u] 已记录首次连接的 SSH 主机密钥",c->device->ip,c->device->port);
    } else snprintf(error,error_size,"SSH host key changed: verify device identity before updating _known_hosts");
finish:
    libssh2_knownhost_free(hosts);LeaveCriticalSection(&pool->host_lock);free(path);return ok;
}
static int send_bytes(Connection *c,const char *data,size_t len,uint64_t deadline) {
    if(c->telnet) return telnet_transport_send(c->telnet,data,len,deadline);
    while(len && !stopped(c->o) && tick_ms()<deadline) {
        ssize_t n=libssh2_channel_write(c->channel,data,len);
        if(n==LIBSSH2_ERROR_EAGAIN) { if(!socket_wait(c,deadline,0)) return 0; }
        else if(n<=0) return 0;
        else { data+=n;len-=(size_t)n; }
    }
    return !len;
}
static int send_line(Connection *c,const char *text,uint64_t deadline) {
    char *bytes=c->telnet?encode_input(text,c->device->encoding):str_copy(text);
    if(!bytes) return 0;
    Buffer line={0};buf_text(&line,bytes);buf_text(&line,c->telnet?"\r\n":"\n");
    SecureZeroMemory(bytes,strlen(bytes));free(bytes);
    int ok=send_bytes(c,line.data,line.len,deadline);SecureZeroMemory(line.data,line.len);buf_free(&line);return ok;
}
static int suffix(const char *text,const char *ending) {
    size_t a=strlen(text),b=strlen(ending);return a>=b && !strcmp(text+a-b,ending);
}
/* Credential prompts may arrive across arbitrary TCP and Telnet packet boundaries. */
static int login_action(Connection *c,Buffer *raw,const char *clean,uint64_t deadline,char *error,size_t size) {
    char *lower=str_copy(clean);_strlwr(lower);
    const char *failure[]={"login incorrect","authentication failed","access denied","invalid password","login failed","incorrect password","认证失败","登录失败","用户名或密码错误"};
    if(c->user_sent || c->password_sent) {
        for(size_t i=0;i<sizeof(failure)/sizeof(*failure);i++) if(strstr(lower,failure[i])) {
            free(lower);snprintf(error,size,"Telnet authentication failed");return -1;
        }
    }
    size_t n=strlen(lower);while(n && isspace((unsigned char)lower[n-1])) lower[--n]=0;
    const char *last=strrchr(lower,'\n');last=last?last+1:lower;
    Buffer compact={0};for(const char *p=last;*p;p++) if(*p!=' ' && *p!='\t' && *p!='\r') buf_add(&compact,p,1);
    const char *line=compact.data?compact.data:"";
    int user=suffix(line,"username:") || suffix(line,"login:") || suffix(line,"loginas:") || suffix(line,"user:") || suffix(line,"用户名:") || suffix(line,"用户名：") || suffix(line,"登录名:") || suffix(line,"登录名：");
    int password=suffix(line,"password:") || suffix(line,"password：") || suffix(line,"密码:") || suffix(line,"密码：");
    int enter=!c->continue_sent && (strstr(line,"pressenter") || strstr(line,"pressreturn") || strstr(line,"pressanykey"));
    int action=user || password || enter;
    if(password && (strstr(line,"newpassword") || strstr(line,"retype") || strstr(line,"新密码"))) {
        snprintf(error,size,"Telnet password change is required; interactive password changes are unsupported");action=-1;
    } else if((user && c->user_sent) || (password && c->password_sent) || (user && c->password_sent)) {
        snprintf(error,size,"Telnet authentication failed (repeated credential prompt)");action=-1;
    } else if(user && !*c->device->username) {
        snprintf(error,size,"Telnet device requested a username, but the Excel username is empty");action=-1;
    } else if(action) {
        const char *value=user?c->device->username:password?c->device->password:"";
        if(!send_line(c,value,deadline)) { snprintf(error,size,"Telnet login send failed, cancelled or credential encoding unsupported");action=-1; }
        else {
            report_at(c->o,LOG_DEBUG,"[%s:%u] TELNET_LOGIN_SENT kind=%s",c->device->ip,c->device->port,user?"username":password?"password":"enter");
            if(user) c->user_sent=1;else if(password) c->password_sent=1;else c->continue_sent=1;
            if(raw->data) { SecureZeroMemory(raw->data,raw->len);raw->data[0]=0; }raw->len=0;
        }
    }
    buf_free(&compact);free(lower);return action;
}
static int pager(const char *text) {
    size_t n=strlen(text);const char *tail=text+(n>1024?n-1024:0);
    char buf[1025];snprintf(buf,sizeof(buf),"%s",tail);_strlwr(buf);
    char *p=strstr(buf,"--more--");if(!p) p=strstr(buf,"---- more ----");
    if(!p) p=strstr(buf,"more:");if(!p) p=strstr(buf,"press any key");
    if(!p) p=strstr(buf,"press <space>");
    if(!p) return 0;
    /* A pager must be on the current terminal line, not in prior command output. */
    return !strchr(p,'\n');
}
static int receive_prompt(Connection *c,const char *expected,char **name,Buffer *raw,char *error,size_t error_size) {
    uint64_t login_ms=c->telnet && c->device->timeout<15?(uint64_t)c->device->timeout*1000:15000;
    uint64_t deadline=tick_ms()+(expected?(uint64_t)c->device->timeout*1000:login_ms),last_data=tick_ms(),stable=0;
    size_t pager_at=0;char *candidate=NULL;
    for(;;) {
        if(stopped(c->o)) { snprintf(error,error_size,"Cancelled");free(candidate);return 0; }
        char block[32768];ssize_t n;
        if(c->telnet) { n=telnet_transport_read(c->telnet,block,sizeof(block));if(n==TELNET_READ_AGAIN) n=LIBSSH2_ERROR_EAGAIN; }
        else n=libssh2_channel_read(c->channel,block,sizeof(block));
        if(n>0) {
            if(raw->len+(size_t)n>MAX_COMMAND_BYTES) { snprintf(error,error_size,"Output exceeds 32 MiB limit");free(candidate);return 0; }
            buf_add(raw,block,(size_t)n);last_data=tick_ms();stable=0;free(candidate);candidate=NULL;
            size_t offset=raw->len>8192?raw->len-8192:0;
            char *decoded=decode_output(raw->data+offset,raw->len-offset,c->device->encoding,NULL);
            char *clean=terminal_clean(decoded);free(decoded);
            if(c->telnet && !expected) {
                int action=login_action(c,raw,clean,deadline,error,error_size);
                if(action) { free(clean);if(action<0) return 0;continue; }
            }
            if(pager(clean) && raw->len!=pager_at) {
                report_at(c->o,LOG_DEBUG,"[%s:%u] PAGER_ADVANCE received_bytes=%zu",c->device->ip,c->device->port,raw->len);
                pager_at=raw->len;
                if(!send_bytes(c," ",1,deadline)) { free(clean);snprintf(error,error_size,"Cannot advance pager");return 0; }
            }
            candidate=prompt_name(clean);
            if(candidate && (!expected || !strcmp(candidate,expected))) stable=last_data;
            free(clean);
        } else if(n!=LIBSSH2_ERROR_EAGAIN && n<0) {
            if(c->telnet) snprintf(error,error_size,"%s",telnet_transport_error(c->telnet));
            else snprintf(error,error_size,"SSH channel read failed (%lld)",(long long)n);
            free(candidate);return 0;
        }
        else {
            if(stable && tick_ms()-stable>=120) { if(name) *name=candidate;else free(candidate);return 1; }
            if(c->telnet?n==0:libssh2_channel_eof(c->channel)) { snprintf(error,error_size,"Device closed channel before completion prompt");free(candidate);return 0; }
            /* select() wakes on data; a short timeout also permits prompt stabilization/cancellation. */
            fd_set fds,writable;FD_ZERO(&fds);FD_SET(c->socket,&fds);FD_ZERO(&writable);
            if(telnet_transport_pending(c->telnet)) FD_SET(c->socket,&writable);
            struct timeval tv={0,50000};select(0,&fds,&writable,NULL,&tv);
        }
        if(tick_ms()>=deadline) { snprintf(error,error_size,"Timed out waiting for device completion prompt");free(candidate);return 0; }
    }
}
static void close_connection(Connection *c) {
    telnet_transport_free(c->telnet);
    if(c->session) {
        libssh2_session_set_blocking(c->session,0);
        if(c->channel) libssh2_channel_free(c->channel);
        libssh2_session_disconnect(c->session,"collection finished");
        if(c->socket!=INVALID_SOCKET) { shutdown(c->socket,SD_BOTH);closesocket(c->socket);c->socket=INVALID_SOCKET; }
        libssh2_session_free(c->session);
    }
    if(c->socket!=INVALID_SOCKET) closesocket(c->socket);
}
static void device_collect(Pool *pool,size_t index) {
    const Device *d=&pool->inv->devices[index];Result *result=&pool->results[index];
    Options device_options=*pool->o;device_options.secret=d->password;const Options *o=&device_options;
    uint64_t device_started=tick_ms(),phase_started=device_started;
    Connection c={0};c.socket=INVALID_SOCKET;c.o=o;c.device=d;
    wchar_t *directory=NULL,*log_path=NULL;Buffer session_log={0};
    char *hostname=NULL; int rc=0;uint64_t deadline=tick_ms()+15000;
    report(o,"[%s:%u] DEVICE_START 正在连接 protocol=%s OS=%s encoding=%s command_timeout_s=%u",d->ip,d->port,protocol_name(d->protocol),d->os,d->encoding,d->timeout);
    if(!connect_tcp(&c)) { snprintf(result->error,sizeof(result->error),"TCP connection failed or timed out (WinSock=%d)",c.network_error);goto done; }
    report(o,"[%s:%u] TCP_CONNECTED elapsed_ms=%llu",d->ip,d->port,(unsigned long long)(tick_ms()-phase_started));phase_started=tick_ms();
    if(d->protocol==PROTOCOL_TELNET) {
        c.telnet=telnet_transport_new(c.socket,o,d);
        if(!c.telnet) { snprintf(result->error,sizeof(result->error),"Cannot allocate Telnet session");goto done; }
        report(o,"[%s:%u] TELNET_LOGIN_WAIT",d->ip,d->port);
    } else {
    c.session=libssh2_session_init_ex(NULL,NULL,NULL,(void*)d);
    if(!c.session) { snprintf(result->error,sizeof(result->error),"Cannot allocate SSH session");goto done; }
    libssh2_session_set_blocking(c.session,0);
    deadline=tick_ms()+15000;
    SSH_RETRY(rc,libssh2_session_handshake(c.session,c.socket),&c,deadline);
    if(rc) { char *detail=NULL;libssh2_session_last_error(c.session,&detail,NULL,0);snprintf(result->error,sizeof(result->error),"SSH handshake failed (%d): %.350s",rc,detail?detail:"");goto done; }
    report(o,"[%s:%u] SSH_HANDSHAKE_OK elapsed_ms=%llu kex=%s hostkey=%s cipher=%s",d->ip,d->port,(unsigned long long)(tick_ms()-phase_started),libssh2_session_methods(c.session,LIBSSH2_METHOD_KEX),libssh2_session_methods(c.session,LIBSSH2_METHOD_HOSTKEY),libssh2_session_methods(c.session,LIBSSH2_METHOD_CRYPT_CS));phase_started=tick_ms();
    if(!host_check(pool,&c,result->error,sizeof(result->error))) goto done;
    report_at(o,LOG_DEBUG,"[%s:%u] HOST_KEY_VERIFIED",d->ip,d->port);
    SSH_RETRY(rc,libssh2_userauth_password(c.session,d->username,d->password),&c,deadline);
    if(rc && !stopped(o)) {
        report_at(o,LOG_DEBUG,"[%s:%u] PASSWORD_AUTH_REJECTED code=%d; trying keyboard-interactive",d->ip,d->port,rc);
        SSH_RETRY(rc,libssh2_userauth_keyboard_interactive(c.session,d->username,keyboard),&c,deadline);
    }
    if(rc) { snprintf(result->error,sizeof(result->error),"SSH authentication failed (%d)",rc);goto done; }
    report(o,"[%s:%u] SSH_AUTH_OK elapsed_ms=%llu",d->ip,d->port,(unsigned long long)(tick_ms()-phase_started));
    while(!(c.channel=libssh2_channel_open_session(c.session))) {
        if(libssh2_session_last_errno(c.session)!=LIBSSH2_ERROR_EAGAIN || !socket_wait(&c,deadline,0)) { snprintf(result->error,sizeof(result->error),"Cannot open SSH channel");goto done; }
    }
    SSH_RETRY(rc,libssh2_channel_handle_extended_data2(c.channel,LIBSSH2_CHANNEL_EXTENDED_DATA_MERGE),&c,deadline);
    if(rc) { snprintf(result->error,sizeof(result->error),"Cannot merge SSH diagnostic stream");goto done; }
    SSH_RETRY(rc,libssh2_channel_request_pty_ex(c.channel,"vt100",5,NULL,0,240,1000,0,0),&c,deadline);
    if(rc) { snprintf(result->error,sizeof(result->error),"PTY request rejected");goto done; }
    SSH_RETRY(rc,libssh2_channel_shell(c.channel),&c,deadline);
    if(rc) { snprintf(result->error,sizeof(result->error),"Shell request rejected");goto done; }
    }
    Buffer banner={0};
    int ready=receive_prompt(&c,NULL,&hostname,&banner,result->error,sizeof(result->error));buf_free(&banner);
    if(!ready) goto done;
    if(c.telnet) report(o,"[%s:%u] TELNET_AUTH_OK elapsed_ms=%llu",d->ip,d->port,(unsigned long long)(tick_ms()-phase_started));
    result->name=str_copy(hostname);
    Buffer folder={0};buf_text(&folder,hostname);buf_text(&folder,"_");buf_text(&folder,d->ip);
    if(d->protocol==PROTOCOL_TELNET || d->port!=22) { char p[16];snprintf(p,sizeof(p),"_%u",d->port);buf_text(&folder,p); }
    char *safe=safe_filename(folder.data);directory=join_path(o->output,safe);free(safe);buf_free(&folder);
    if(!make_dirs(directory)) { snprintf(result->error,sizeof(result->error),"Cannot create device output directory");goto done; }
    log_path=join_path(directory,"_session.log");
    char *folder_path=utf8(directory);report(o,"[%s:%u] SHELL_READY 设备名: %s output=%s",d->ip,d->port,hostname,folder_path);free(folder_path);
    CommandSet *set=&pool->inv->sets[d->set];
    char **filenames=set->files;
    for(size_t i=0;i<set->count;i++) {
        if(stopped(o)) { snprintf(result->error,sizeof(result->error),"Cancelled");break; }
        uint64_t command_started=tick_ms();
        report(o,"[%s:%u] COMMAND_START %zu/%zu %s",d->ip,d->port,i+1,set->count,set->commands[i]);
        Buffer raw={0};ready=send_line(&c,set->commands[i],tick_ms()+15000);
        if(!ready) snprintf(result->error,sizeof(result->error),"Command send failed or cancelled");
        else ready=receive_prompt(&c,hostname,NULL,&raw,result->error,sizeof(result->error));
        int lossy=0;char *decoded=decode_output(raw.data?raw.data:"",raw.len,d->encoding,&lossy);
        char *clean=terminal_clean(decoded);free(decoded);
        Buffer text={0};buf_text(&text,clean);free(clean);buf_text(&text,"\n");
        if(!ready) { buf_text(&text,"\n[INCOMPLETE: ");buf_text(&text,result->error);buf_text(&text,"]\n"); }
        wchar_t *path=join_path(directory,filenames[i]);
        if(!write_utf8(path,text.data)) { ready=0;snprintf(result->error,sizeof(result->error),"Cannot write command output"); }
        if(lossy) {
            report_at(o,LOG_WARN,"[%s:%u] 编码转换有损，请在 Excel 编码列指定编码；保留原始字节到 .txt.raw",d->ip,d->port);
            Buffer rawname={0};buf_text(&rawname,filenames[i]);buf_text(&rawname,".raw");wchar_t *rp=join_path(directory,rawname.data);
            FILE *rf=_wfopen(rp,L"wb");int raw_ok=0;
            if(rf) { raw_ok=fwrite(raw.data,1,raw.len,rf)==raw.len;if(fclose(rf)) raw_ok=0; }
            if(!raw_ok) { ready=0;snprintf(result->error,sizeof(result->error),"Cannot preserve raw output bytes"); }
            free(rp);buf_free(&rawname);
        }
        report_at(o,ready?LOG_INFO:LOG_ERROR,"[%s:%u] COMMAND_%s %zu/%zu file=%s received_bytes=%zu elapsed_ms=%llu%s%s",d->ip,d->port,ready?"OK":"FAILED",i+1,set->count,filenames[i],raw.len,(unsigned long long)(tick_ms()-command_started),ready?"":" error=",ready?"":result->error);
        free(path);buf_free(&text);buf_free(&raw);
        buf_text(&session_log,ready?"OK\t":"FAILED\t");buf_text(&session_log,filenames[i]);buf_text(&session_log,"\t");buf_text(&session_log,set->commands[i]);buf_text(&session_log,"\n");
        if(!ready) break;result->completed++;
    }
    if(result->completed==set->count) result->status=1;
done:
    if(stopped(o) && !result->status) { result->status=2;snprintf(result->error,sizeof(result->error),"Cancelled"); }
    if(log_path) {
        buf_text(&session_log,result->status==1?"SUCCESS\n":result->error);buf_text(&session_log,"\n");
        char *safe_log=redact_secret(session_log.data,d->password);
        if(!write_utf8(log_path,safe_log)) { result->status=0;snprintf(result->error,sizeof(result->error),"Cannot write session log"); }free(safe_log);
    }
    char *safe_error=redact_secret(result->error,d->password);snprintf(result->error,sizeof(result->error),"%s",safe_error);free(safe_error);
    report_at(o,result->status==1?LOG_INFO:result->status==2?LOG_WARN:LOG_ERROR,"[%s:%u] DEVICE_END %s (%zu 条命令) elapsed_ms=%llu%s%s",d->ip,d->port,result->status==1?"成功":result->status==2?"已停止":"失败",result->completed,(unsigned long long)(tick_ms()-device_started),*result->error?": ":"",result->error);
    close_connection(&c);free(hostname);free(directory);free(log_path);buf_free(&session_log);
}
static unsigned __stdcall worker(void *context) {
    Pool *p=context;
    while(!stopped(p->o)) { LONG i=InterlockedIncrement(&p->next)-1;if((size_t)i>=p->inv->count) break;device_collect(p,(size_t)i); }
    return 0;
}
static void csv(Buffer *b,const char *s) { buf_text(b,"\"");for(;*s;s++) { if(*s=='\"') buf_text(b,"\"");buf_add(b,s,1); }buf_text(b,"\""); }
static int collect_internal(const Options *o,Summary *summary) {
    memset(summary,0,sizeof(*summary));Inventory inv;char error[512];
    report(o,"TEMPLATE_READ 开始读取模板");
    if(!inventory_load(o->input,&inv,error,sizeof(error))) { report_at(o,LOG_ERROR,"TEMPLATE_FAILED 模板错误: %s",error);return 2; }
    if(!make_dirs(o->output)) { inventory_free(&inv);report(o,"无法创建输出目录");return 2; }
    WSADATA wsa;int init_error=WSAStartup(MAKEWORD(2,2),&wsa);
    if(init_error) { inventory_free(&inv);report_at(o,LOG_ERROR,"WinSock 初始化失败 code=%d",init_error);return 2; }
    init_error=libssh2_init(0);
    if(init_error) { WSACleanup();inventory_free(&inv);report_at(o,LOG_ERROR,"SSH 网络库初始化失败 code=%d",init_error);return 2; }
    Pool pool={0};pool.o=o;pool.inv=&inv;pool.results=mem_alloc(inv.count*sizeof(Result));InitializeCriticalSection(&pool.host_lock);
    unsigned count=o->workers?o->workers:4;if(count>32) count=32;if(count>inv.count) count=(unsigned)inv.count;
    report(o,"已读取 %zu 台设备，%u 个并发线程",inv.count,count);
    HANDLE threads[32];unsigned started=0;
    for(unsigned i=0;i<count;i++) { HANDLE h=(HANDLE)_beginthreadex(NULL,0,worker,&pool,0,NULL);if(!h) { report_at(o,LOG_WARN,"WORKER_CREATE_FAILED Win32=%lu; continuing with available workers",(unsigned long)GetLastError());break; }threads[started++]=h; }
    if(!started) worker(&pool);
    else { WaitForMultipleObjects(started,threads,TRUE,INFINITE);for(unsigned i=0;i<started;i++) CloseHandle(threads[i]); }
    Buffer out={0};buf_text(&out,"管理地址,端口,设备名,状态,完成命令数,错误,登录方式\r\n");
    for(size_t i=0;i<inv.count;i++) {
        Result *r=&pool.results[i];if(!r->status && !*r->error) { r->status=2;snprintf(r->error,sizeof(r->error),"Not started (cancelled)"); }
        if(r->status==1) summary->succeeded++;else if(r->status==2) summary->cancelled++;else summary->failed++;
        csv(&out,inv.devices[i].ip);char n[64];snprintf(n,sizeof(n),",%u,",inv.devices[i].port);buf_text(&out,n);
        csv(&out,r->name?r->name:"");buf_text(&out,",");csv(&out,r->status==1?"SUCCESS":r->status==2?"CANCELLED":"FAILED");
        snprintf(n,sizeof(n),",%zu,",r->completed);buf_text(&out,n);csv(&out,r->error);buf_text(&out,",");csv(&out,protocol_name(inv.devices[i].protocol));buf_text(&out,"\r\n");free(r->name);
    }
    wchar_t *path=join_path(o->output,"_summary.csv");int ok=write_utf8(path,out.data);free(path);buf_free(&out);
    if(!ok) report_at(o,LOG_ERROR,"SUMMARY_WRITE_FAILED 无法写入 _summary.csv");
    report(o,"采集结束：成功 %zu，失败 %zu，停止 %zu",summary->succeeded,summary->failed,summary->cancelled);
    free(pool.results);DeleteCriticalSection(&pool.host_lock);inventory_free(&inv);libssh2_exit();WSACleanup();
    return !ok?2:summary->cancelled?3:summary->failed?1:0;
}
int collect(const Options *o,Summary *summary) {
    memset(summary,0,sizeof(*summary));uint64_t started=tick_ms();
    wchar_t *output=full_path(o->output);
    if(!output) { report_at(o,LOG_ERROR,"无效的输出目录");return 2; }
    if(!make_dirs(output)) { report_at(o,LOG_ERROR,"无法创建输出目录 Win32=%lu",(unsigned long)GetLastError());free(output);return 2; }
    DWORD error;FileLogger *logger=logger_open(output,"collection",&error);
    if(!logger) { report_at(o,LOG_ERROR,"无法创建任务日志，采集未启动 Win32=%lu",(unsigned long)error);free(output);return 2; }
    Options resolved=*o;resolved.output=output;resolved.file_log=logger;
    char *input_path=utf8(o->input),*output_path=utf8(output),*log_path=utf8(logger_path(logger));
    report(&resolved,"RUN_START version=%s workers=%u input=%s output=%s",NETCOLLECT_VERSION,o->workers,input_path,output_path);
    report(&resolved,"任务日志: %s",log_path);free(input_path);free(output_path);free(log_path);
    report_at(&resolved,LOG_DEBUG,"SSH_LIBRARY %s",libssh2_version(0));
    int code=collect_internal(&resolved,summary);
    report_at(&resolved,code==0?LOG_INFO:code==3?LOG_WARN:LOG_ERROR,"RUN_END code=%d success=%zu failed=%zu cancelled=%zu elapsed_ms=%llu",code,summary->succeeded,summary->failed,summary->cancelled,(unsigned long long)(tick_ms()-started));
    if(logger_error(logger)) code=2;
    if(!logger_close(logger)) { code=2;report_at(o,LOG_ERROR,"任务日志写入失败，详细信息见应用日志"); }
    free(output);return code;
}
