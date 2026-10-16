#ifndef COLLECTOR_H
#define COLLECTOR_H
#include "version.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

#define MAX_COMMAND_BYTES (32u * 1024u * 1024u)
#define MAX_ZIP_BYTES (256u * 1024u * 1024u)
typedef struct { char *data; size_t len, cap; } Buffer;
typedef struct { char *name; char **commands, **files; size_t count; } CommandSet;
typedef enum { PROTOCOL_SSH, PROTOCOL_TELNET } LoginProtocol;
typedef struct {
    char *ip, *username, *password, *os, *encoding;
    unsigned port, timeout;
    size_t set;
    LoginProtocol protocol;
} Device;
typedef struct { Device *devices; size_t count; CommandSet *sets; size_t set_count; } Inventory;
typedef void (*LogFn)(void *, const char *);
typedef struct FileLogger FileLogger;
typedef enum { LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR } LogLevel;
typedef struct {
    const wchar_t *input, *output;
    unsigned workers;
    HANDLE cancel;
    LogFn log;
    void *log_ctx;
    FileLogger *file_log;
    const char *secret;
} Options;
typedef struct { size_t succeeded, failed, cancelled; } Summary;

void *mem_alloc(size_t n);
char *str_copy(const char *s);
void buf_add(Buffer *b, const void *data, size_t n);
void buf_text(Buffer *b, const char *s);
void buf_free(Buffer *b);
wchar_t *wide(const char *s);
wchar_t *full_path(const wchar_t *path);
char *utf8(const wchar_t *s);
char *decode_output(const char *s, size_t n, const char *encoding, int *lossy);
char *encode_input(const char *s, const char *encoding);
const char *protocol_name(LoginProtocol protocol);
char *terminal_clean(const char *s);
char *prompt_name(const char *s);
int ends_at_prompt(const char *s, const char *name);
char *safe_filename(const char *s);
wchar_t *join_path(const wchar_t *dir, const char *name);
int make_dirs(const wchar_t *path);
int write_utf8(const wchar_t *path, const char *text);
uint64_t tick_ms(void);
int stopped(const Options *o);
void report(const Options *o, const char *format, ...);
void report_at(const Options *o, LogLevel level, const char *format, ...);
char *redact_secret(const char *text, const char *secret);
FileLogger *logger_open(const wchar_t *directory, const char *prefix, DWORD *error);
int logger_write(FileLogger *logger, LogLevel level, const char *text);
DWORD logger_error(FileLogger *logger);
const wchar_t *logger_path(FileLogger *logger);
int logger_close(FileLogger *logger);
int app_log_start(void);
void app_log_stop(int code);
void app_log_write(LogLevel level, const char *text);
const wchar_t *app_log_directory(void);
int inventory_load(const wchar_t *path, Inventory *inv, char *error, size_t size);
void inventory_free(Inventory *inv);
int collect(const Options *options, Summary *summary);
int gui_main(HINSTANCE instance, int show);
#endif
