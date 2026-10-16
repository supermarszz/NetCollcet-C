#include "collector.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define ROTATE_BYTES (8u * 1024u * 1024u)
struct FileLogger {
    CRITICAL_SECTION lock;
    HANDLE file;
    wchar_t *directory, *path;
    char base[128];
    unsigned part;
    size_t bytes;
    DWORD error;
};
static FileLogger *application;
static volatile LONG sequence;

static int open_part(FileLogger *logger) {
    char name[160];
    if(logger->part) snprintf(name,sizeof(name),"%s_%03u.log",logger->base,logger->part);
    else snprintf(name,sizeof(name),"%s.log",logger->base);
    free(logger->path);logger->path=join_path(logger->directory,name);
    logger->file=CreateFileW(logger->path,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_DELETE,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    if(logger->file==INVALID_HANDLE_VALUE) { logger->error=GetLastError();return 0; }
    DWORD written;
    if(!WriteFile(logger->file,"\xef\xbb\xbf",3,&written,NULL) || written!=3) {
        logger->error=GetLastError();if(!logger->error) logger->error=ERROR_WRITE_FAULT;return 0;
    }
    logger->bytes=3;return 1;
}
FileLogger *logger_open(const wchar_t *directory,const char *prefix,DWORD *error) {
    *error=0;wchar_t *absolute=full_path(directory);
    if(!absolute || !make_dirs(absolute)) { *error=GetLastError();if(!*error) *error=ERROR_PATH_NOT_FOUND;free(absolute);return NULL; }
    FileLogger *logger=mem_alloc(sizeof(*logger));logger->directory=absolute;logger->file=INVALID_HANDLE_VALUE;
    InitializeCriticalSection(&logger->lock);
    SYSTEMTIME t;GetLocalTime(&t);
    snprintf(logger->base,sizeof(logger->base),"%s_%04u%02u%02u_%02u%02u%02u_%03u_%lu_%ld",prefix,t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds,(unsigned long)GetCurrentProcessId(),(long)InterlockedIncrement(&sequence));
    if(!open_part(logger)) { *error=logger->error;logger_close(logger);return NULL; }
    return logger;
}
int logger_write(FileLogger *logger,LogLevel level,const char *text) {
    if(!logger) return 1;
    const char *levels[]={"DEBUG","INFO","WARN","ERROR"};
    Buffer record={0};char header[180];int ok=0;
    EnterCriticalSection(&logger->lock);
    if(logger->error) goto done;
    SYSTEMTIME t;GetLocalTime(&t);
    snprintf(header,sizeof(header),"[%04u-%02u-%02u %02u:%02u:%02u.%03u] [%-5s] [pid=%lu tid=%lu] ",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds,levels[level],(unsigned long)GetCurrentProcessId(),(unsigned long)GetCurrentThreadId());
    buf_text(&record,header);
    /* Keep each event on one line so device text cannot forge log records. */
    for(const unsigned char *p=(const unsigned char*)text;*p;p++) {
        if(*p=='\r') buf_text(&record,"\\r");else if(*p=='\n') buf_text(&record,"\\n");
        else if(*p<32 && *p!='\t') { char escape[8];snprintf(escape,sizeof(escape),"\\x%02x",*p);buf_text(&record,escape); }
        else buf_add(&record,p,1);
    }
    buf_text(&record,"\r\n");
    if(logger->bytes>3 && logger->bytes+record.len>ROTATE_BYTES) {
        if(!FlushFileBuffers(logger->file)) { logger->error=GetLastError();goto done; }
        CloseHandle(logger->file);logger->file=INVALID_HANDLE_VALUE;logger->part++;
        if(!open_part(logger)) goto done;
    }
    DWORD written=0;
    ok=WriteFile(logger->file,record.data,(DWORD)record.len,&written,NULL) && written==record.len;
    if(ok && level==LOG_ERROR) ok=FlushFileBuffers(logger->file);
    if(!ok) { logger->error=GetLastError();if(!logger->error) logger->error=ERROR_WRITE_FAULT; }
    else logger->bytes+=record.len;
done:
    LeaveCriticalSection(&logger->lock);buf_free(&record);return ok;
}
DWORD logger_error(FileLogger *logger) {
    if(!logger) return 0;EnterCriticalSection(&logger->lock);DWORD error=logger->error;LeaveCriticalSection(&logger->lock);return error;
}
const wchar_t *logger_path(FileLogger *logger) { return logger?logger->path:NULL; }
int logger_close(FileLogger *logger) {
    if(!logger) return 1;
    int ok=!logger->error;
    if(logger->file!=INVALID_HANDLE_VALUE) { if(!FlushFileBuffers(logger->file)) ok=0;if(!CloseHandle(logger->file)) ok=0; }
    DeleteCriticalSection(&logger->lock);free(logger->directory);free(logger->path);free(logger);return ok;
}
int app_log_start(void) {
    if(application) return 1;
    wchar_t exe[32768];DWORD n=GetModuleFileNameW(NULL,exe,32768);if(!n || n>=32768) return 0;
    wchar_t *slash=wcsrchr(exe,'\\');if(!slash) return 0;
    wchar_t *folder=mem_alloc((wcslen(exe)+1)*sizeof(*folder));wcscpy(folder,exe);folder[slash-exe]=0;
    wchar_t *directory=join_path(folder,"logs");free(folder);DWORD error;
    application=logger_open(directory,"application",&error);free(directory);
    if(!application) {
        wchar_t local[32768];n=GetEnvironmentVariableW(L"LOCALAPPDATA",local,32768);
        if(n && n<32768) { directory=join_path(local,"NetCollect\\logs");application=logger_open(directory,"application",&error);free(directory); }
    }
    if(!application) return 0;
    app_log_write(LOG_INFO,"APPLICATION_START NetCollcet-C " NETCOLLECT_VERSION " x64");
    char *path=utf8(exe);Buffer message={0};buf_text(&message,"executable=");buf_text(&message,path);free(path);app_log_write(LOG_INFO,message.data);buf_free(&message);
    TIME_ZONE_INFORMATION zone;DWORD state=GetTimeZoneInformation(&zone);
    LONG bias=zone.Bias+(state==TIME_ZONE_ID_DAYLIGHT?zone.DaylightBias:zone.StandardBias);
    char info[200];snprintf(info,sizeof(info),"log_format=UTF-8-BOM local_time utc_offset_minutes=%ld rotate_bytes=%u",(long)-bias,ROTATE_BYTES);app_log_write(LOG_INFO,info);
    return 1;
}
void app_log_write(LogLevel level,const char *text) { logger_write(application,level,text); }
void app_log_stop(int code) {
    char text[100];snprintf(text,sizeof(text),"APPLICATION_EXIT code=%d",code);app_log_write(code?LOG_WARN:LOG_INFO,text);
    logger_close(application);application=NULL;
}
const wchar_t *app_log_directory(void) { return application?application->directory:NULL; }
