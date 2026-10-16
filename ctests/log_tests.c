#include "collector.h"
#include <process.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"FAIL log_tests line %d: %s\n",__LINE__,#x);return 1; } } while(0)

static unsigned __stdcall write_events(void *context) {
    FileLogger *logger=context;char text[4096];memset(text,'z',sizeof(text)-1);text[sizeof(text)-1]=0;
    for(int i=0;i<800;i++) { if(!logger_write(logger,LOG_INFO,text)) return 1; }return 0;
}
int main(void) {
    DWORD error;FileLogger *logger=logger_open(L"test-output/logger", "thread_test",&error);CHECK(logger);
    wchar_t *pattern=mem_alloc((wcslen(logger_path(logger))+10)*sizeof(*pattern));wcscpy(pattern,logger_path(logger));
    wchar_t *ext=wcsrchr(pattern,'.');wcscpy(ext,L"*.log");
    CHECK(logger_write(logger,LOG_WARN,"live event\ninjected line"));
    FILE *live=_wfopen(logger_path(logger),L"rb");CHECK(live);char initial[512]={0};fread(initial,1,sizeof(initial)-1,live);fclose(live);
    CHECK(strstr(initial,"[WARN ") && strstr(initial,"live event\\ninjected line"));
    HANDLE threads[3];
    for(int i=0;i<3;i++) { threads[i]=(HANDLE)_beginthreadex(NULL,0,write_events,logger,0,NULL);CHECK(threads[i]); }
    WaitForMultipleObjects(3,threads,TRUE,INFINITE);
    for(int i=0;i<3;i++) { DWORD code;GetExitCodeThread(threads[i],&code);CloseHandle(threads[i]);CHECK(code==0); }
    CHECK(!logger_error(logger));CHECK(logger_close(logger));
    WIN32_FIND_DATAW item;HANDLE find=FindFirstFileW(pattern,&item);CHECK(find!=INVALID_HANDLE_VALUE);
    size_t events=0;unsigned files=0;
    do {
        files++;CHECK(item.nFileSizeHigh==0 && item.nFileSizeLow<=8u*1024u*1024u);
        wchar_t *dir=full_path(L"test-output/logger");char *name=utf8(item.cFileName);wchar_t *path=join_path(dir,name);free(dir);free(name);
        FILE *f=_wfopen(path,L"rb");free(path);CHECK(f);unsigned char bom[3];CHECK(fread(bom,1,3,f)==3 && !memcmp(bom,"\xef\xbb\xbf",3));
        char line[8192];while(fgets(line,sizeof(line),f)) { CHECK(line[0]=='[' && strstr(line,"[pid=") && strstr(line," tid=") && strstr(line,"\r\n"));events++; }fclose(f);
    } while(FindNextFileW(find,&item));FindClose(find);free(pattern);
    CHECK(events==2401 && files==2);
    char *safe=redact_secret("one secret two secret","secret");CHECK(!strcmp(safe,"one [REDACTED] two [REDACTED]"));free(safe);
    CHECK(!logger_open(L"?:\\invalid","test",&error) && error);
    puts("log_tests: live writes, 2401 concurrent events, rotation, UTF-8 BOM and redaction PASS");return 0;
}
