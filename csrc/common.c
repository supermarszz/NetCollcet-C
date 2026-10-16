#include "collector.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <wchar.h>

void *mem_alloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    if (!p) { fputs("Out of memory\n", stderr); abort(); }
    return p;
}
char *str_copy(const char *s) {
    size_t n = strlen(s) + 1; char *p = mem_alloc(n); memcpy(p, s, n); return p;
}
void buf_add(Buffer *b, const void *data, size_t n) {
    if (n > SIZE_MAX - b->len - 1) abort();
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n + 1) cap *= 2;
        void *p = realloc(b->data, cap); if (!p) abort(); b->data = p; b->cap = cap;
    }
    memcpy(b->data + b->len, data, n); b->len += n; b->data[b->len] = 0;
}
void buf_text(Buffer *b, const char *s) { buf_add(b, s, strlen(s)); }
void buf_free(Buffer *b) { free(b->data); memset(b, 0, sizeof(*b)); }
wchar_t *wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    if (!n) return NULL;
    wchar_t *p = mem_alloc((size_t)n * sizeof(*p));
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, p, n); return p;
}
char *utf8(const wchar_t *s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    char *p = mem_alloc((size_t)n); WideCharToMultiByte(CP_UTF8, 0, s, -1, p, n, NULL, NULL); return p;
}
wchar_t *full_path(const wchar_t *path) {
    if(!path || !*path) return NULL;
    DWORD needed=GetFullPathNameW(path,0,NULL,NULL);if(!needed || needed>32700) return NULL;
    wchar_t *absolute=mem_alloc(((size_t)needed+1)*sizeof(*absolute));
    if(!GetFullPathNameW(path,needed+1,absolute,NULL)) { free(absolute);return NULL; }
    for(size_t i=0;absolute[i];i++) if(absolute[i]=='/') absolute[i]='\\';
    if(!wcsncmp(absolute,L"\\\\?\\",4)) return absolute;
    size_t n=wcslen(absolute);wchar_t *extended=mem_alloc((n+9)*sizeof(*extended));
    if(!wcsncmp(absolute,L"\\\\",2)) { wcscpy(extended,L"\\\\?\\UNC\\");wcscat(extended,absolute+2); }
    else { wcscpy(extended,L"\\\\?\\");wcscat(extended,absolute); }
    free(absolute);return extended;
}
char *decode_output(const char *s, size_t n, const char *encoding, int *lossy) {
    UINT cp = CP_UTF8; DWORD flags = MB_ERR_INVALID_CHARS;
    if (lossy) *lossy = 0;
    if (!n) return str_copy("");
    if (!_stricmp(encoding, "gbk")) cp = 936;
    else if (!_stricmp(encoding, "gb18030")) cp = 54936;
    else if (!_stricmp(encoding, "big5")) cp = 950;
    else if (!_stricmp(encoding, "latin1")) cp = 28591;
    int count = MultiByteToWideChar(cp, flags, s, (int)n, NULL, 0);
    if (!count && !_stricmp(encoding, "auto")) {
        cp = 54936; count = MultiByteToWideChar(cp, flags, s, (int)n, NULL, 0);
    }
    if (!count) {
        flags = 0; if (lossy) *lossy = 1;
        count = MultiByteToWideChar(cp, flags, s, (int)n, NULL, 0);
    }
    if (!count) { if (lossy) *lossy = 1; return str_copy("[encoding conversion failed]\n"); }
    wchar_t *w = mem_alloc(((size_t)count + 1) * sizeof(*w));
    MultiByteToWideChar(cp, flags, s, (int)n, w, count);
    /* Some devices insert NUL padding in otherwise valid terminal output. */
    int used=0;for(int i=0;i<count;i++) if(w[i]) w[used++]=w[i];w[used]=0;
    char *result = utf8(w); free(w); return result;
}
char *encode_input(const char *s,const char *encoding) {
    UINT cp=CP_UTF8;
    if(!_stricmp(encoding,"gbk")) cp=936;else if(!_stricmp(encoding,"gb18030")) cp=54936;
    else if(!_stricmp(encoding,"big5")) cp=950;else if(!_stricmp(encoding,"latin1")) cp=28591;
    wchar_t *w=wide(s);if(!w) return NULL;
    BOOL substituted=FALSE;BOOL *used=(cp==CP_UTF8 || cp==54936)?NULL:&substituted;
    DWORD flags=used?WC_NO_BEST_FIT_CHARS:0;
    int count=WideCharToMultiByte(cp,flags,w,-1,NULL,0,NULL,used);
    char *bytes=count?mem_alloc((size_t)count):NULL;
    if(bytes) { WideCharToMultiByte(cp,flags,w,-1,bytes,count,NULL,used);if(substituted) { SecureZeroMemory(bytes,(size_t)count);free(bytes);bytes=NULL; } }
    SecureZeroMemory(w,wcslen(w)*sizeof(*w));free(w);return bytes;
}
const char *protocol_name(LoginProtocol protocol) { return protocol==PROTOCOL_TELNET?"TELNET":"SSH"; }
char *terminal_clean(const char *s) {
    Buffer b = {0}; size_t i = 0;
    while (s[i]) {
        unsigned char c = (unsigned char)s[i++];
        if (c == 27) {
            if (s[i] == '[') { i++; while (s[i] && !((unsigned char)s[i] >= 0x40 && (unsigned char)s[i] <= 0x7e)) i++; if (s[i]) i++; }
            else if (s[i] == ']') { i++; while (s[i] && s[i] != 7 && !(s[i] == 27 && s[i+1] == '\\')) i++; if (s[i] == 7) i++; else if (s[i]) i += 2; }
            else if (s[i]) i++;
        } else if (c == '\b') {
            if (b.len && b.data[b.len-1] != '\n') { b.len--; while (b.len && ((unsigned char)b.data[b.len] & 0xc0) == 0x80) b.len--; b.data[b.len] = 0; }
        } else if (c == '\r') {
            if (s[i] != '\n') buf_text(&b, "\n");
        } else if (c >= 32 || c == '\n' || c == '\t') buf_add(&b, &c, 1);
    }
    if (!b.data) return str_copy("");
    return b.data;
}
static char *last_line(const char *s) {
    size_t n = strlen(s); while (n && isspace((unsigned char)s[n-1])) n--;
    size_t start = n; while (start && s[start-1] != '\n') start--;
    while (start < n && isspace((unsigned char)s[start])) start++;
    char *p = mem_alloc(n-start+1); memcpy(p, s+start, n-start); return p;
}
char *prompt_name(const char *s) {
    char *line = last_line(s); size_t n = strlen(line); char *name = NULL;
    if (n > 2 && ((line[0] == '<' && line[n-1] == '>') || (line[0] == '[' && line[n-1] == ']'))) {
        line[n-1] = 0; name = str_copy(line+1);
    } else if (n > 1 && strchr("#>$%", line[n-1])) {
        line[n-1] = 0; name = str_copy(line);
        char *paren = strchr(name, '('); if (paren) *paren = 0;
    }
    free(line);
    if (name) {
        char *at=strrchr(name,'@');if(at) memmove(name,at+1,strlen(at+1)+1);
        while(name[0]=='~' || name[0]=='*') memmove(name,name+1,strlen(name));
        size_t len = strlen(name);
        if (!len || len > 200 || strpbrk(name, "\t \r\n")) { free(name); return NULL; }
    }
    return name;
}
int ends_at_prompt(const char *s, const char *name) {
    char *found = prompt_name(s); int ok = found && !strcmp(found, name); free(found); return ok;
}
char *safe_filename(const char *s) {
    wchar_t *w = wide(s); if (!w) return str_copy("unknown");
    size_t n = wcslen(w);
    for (size_t i = 0; i < n; i++) if (w[i] < 32 || wcschr(L"<>:\"/\\|?*", w[i])) w[i] = '_';
    while (n && (w[n-1] == '.' || w[n-1] == ' ')) w[--n] = 0;
    /* Keep room for a collision suffix and .txt within an NTFS component. */
    if (n > 160) { n = 160; if (w[n-1] >= 0xd800 && w[n-1] <= 0xdbff) n--; w[n] = 0; }
    if (!n) { free(w); return str_copy("unknown"); }
    wchar_t base[161]; wcscpy(base, w); wchar_t *dot = wcschr(base, '.'); if (dot) *dot = 0;
    int reserved = !_wcsicmp(base,L"CON") || !_wcsicmp(base,L"PRN") || !_wcsicmp(base,L"AUX") || !_wcsicmp(base,L"NUL");
    if (wcslen(base)==4 && ((!_wcsnicmp(base,L"COM",3)) || (!_wcsnicmp(base,L"LPT",3))) && base[3]>='1' && base[3]<='9') reserved = 1;
    char *p = utf8(w); free(w);
    if (reserved) { Buffer b = {0}; buf_text(&b,"_"); buf_text(&b,p); free(p); return b.data; }
    return p;
}
wchar_t *join_path(const wchar_t *dir, const char *name) {
    wchar_t *part = wide(name); if (!part) return NULL;
    size_t n = wcslen(dir), m = wcslen(part);
    wchar_t *p = mem_alloc((n+m+2)*sizeof(*p)); wcscpy(p,dir);
    if (n && dir[n-1] != L'\\' && dir[n-1] != L'/') p[n++] = L'\\';
    wcscpy(p+n,part); free(part); return p;
}
int make_dirs(const wchar_t *path) {
    wchar_t *p = mem_alloc((wcslen(path)+1)*sizeof(*p)); wcscpy(p,path);
    size_t start=1;
    if(!wcsncmp(p,L"\\\\?\\UNC\\",8)) {
        start=8;for(int j=0;j<2;j++) { while(p[start] && p[start]!='\\') start++;if(p[start]) start++; }
    } else if(!wcsncmp(p,L"\\\\?\\",4)) start=7;
    else if(!wcsncmp(p,L"\\\\",2)) {
        start=2;for(int j=0;j<2;j++) { while(p[start] && p[start]!='\\') start++;if(p[start]) start++; }
    }
    for (size_t i=start; p[i]; i++) if (p[i]=='\\' || p[i]=='/') {
        wchar_t c=p[i]; p[i]=0; if (wcslen(p)>2) CreateDirectoryW(p,NULL); p[i]=c;
    }
    int ok = CreateDirectoryW(p,NULL) || GetLastError()==ERROR_ALREADY_EXISTS;
    DWORD attr = GetFileAttributesW(p); free(p); return ok && attr!=INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}
int write_utf8(const wchar_t *path, const char *text) {
    FILE *f = _wfopen(path,L"wb"); if (!f) return 0;
    size_t n = strlen(text); int ok = fwrite("\xef\xbb\xbf",1,3,f)==3 && fwrite(text,1,n,f)==n;
    if (fclose(f)) ok=0; return ok;
}
uint64_t tick_ms(void) { return GetTickCount64(); }
int stopped(const Options *o) { return o->cancel && WaitForSingleObject(o->cancel,0)==WAIT_OBJECT_0; }
char *redact_secret(const char *text,const char *secret) {
    if(!secret || !*secret) return str_copy(text);
    Buffer result={0};size_t length=strlen(secret);const char *match;
    while((match=strstr(text,secret))) { buf_add(&result,text,(size_t)(match-text));buf_text(&result,"[REDACTED]");text=match+length; }
    buf_text(&result,text);return result.data;
}
static void report_v(const Options *o,LogLevel level,const char *format,va_list ap) {
    va_list measure;va_copy(measure,ap);int length=vsnprintf(NULL,0,format,measure);va_end(measure);
    if(length<0) return;
    char *line=mem_alloc((size_t)length+1);vsnprintf(line,(size_t)length+1,format,ap);
    char *safe=redact_secret(line,o->secret);
    free(line);
    app_log_write(level,safe);
    if(!logger_write(o->file_log,level,safe) && o->log) {
        char error[200];snprintf(error,sizeof(error),"ERROR: 无法写入任务日志 (Win32=%lu)",(unsigned long)logger_error(o->file_log));
        o->log(o->log_ctx,error);app_log_write(LOG_ERROR,error);
    }
    if(o->log && level!=LOG_DEBUG) o->log(o->log_ctx,safe);
    free(safe);
}
void report_at(const Options *o,LogLevel level,const char *format,...) {
    va_list ap;va_start(ap,format);report_v(o,level,format,ap);va_end(ap);
}
void report(const Options *o,const char *format,...) {
    va_list ap;va_start(ap,format);report_v(o,LOG_INFO,format,ap);va_end(ap);
}
