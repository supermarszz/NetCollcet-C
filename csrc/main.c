#include "collector.h"
#include <shellapi.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static HANDLE cli_cancel;
static BOOL WINAPI ctrl(DWORD event) { if(event==CTRL_C_EVENT || event==CTRL_BREAK_EVENT) { SetEvent(cli_cancel);return TRUE; }return FALSE; }
static void print_log(void *unused,const char *text) { puts(text);fflush(stdout); }
static void cli_error(const char *text) { fputs(text,stderr);app_log_write(LOG_ERROR,text); }
static int program_run(void) {
    int argc;wchar_t **argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(argc<=1) { LocalFree(argv);return gui_main(GetModuleHandleW(NULL),SW_SHOWDEFAULT); }
    Options o={0};o.workers=4;o.log=print_log;int check=0;
    SetConsoleOutputCP(CP_UTF8);
    for(int i=1;i<argc;i++) {
        if(!wcscmp(argv[i],L"--check") && i+1<argc) { o.input=argv[++i];check=1; }
        else if(!wcscmp(argv[i],L"--input") && i+1<argc) o.input=argv[++i];
        else if(!wcscmp(argv[i],L"--output") && i+1<argc) o.output=argv[++i];
        else if(!wcscmp(argv[i],L"--workers") && i+1<argc) {
            wchar_t *end;unsigned long n=wcstoul(argv[++i],&end,10);
            if(*end || n<1 || n>32) { cli_error("workers must be 1..32\n");LocalFree(argv);return 2; }o.workers=(unsigned)n;
        } else if(!wcscmp(argv[i],L"--version")) {
            puts("NetCollcet-C " NETCOLLECT_VERSION);LocalFree(argv);return 0;
        } else if(!wcscmp(argv[i],L"--help")) {
            puts("NetCollcet-C " NETCOLLECT_VERSION " - Windows x64\n  netcollect-cli --version\n  netcollect-cli --check template.xlsx\n  netcollect-cli --input template.xlsx --output directory [--workers 4]\n  netcollect (no arguments): desktop GUI\nExit codes: 0 success, 1 device failure, 2 input/IO failure, 3 cancelled.");LocalFree(argv);return 0;
        } else { cli_error("Unknown or incomplete argument; use --help\n");LocalFree(argv);return 2; }
    }
    int rc=2;
    if(check && o.input) {
        Inventory inv;char error[512];
        char *input=utf8(o.input);report(&o,"TEMPLATE_CHECK input=%s",input);free(input);
        if(inventory_load(o.input,&inv,error,sizeof(error))) {
            printf("OK: devices=%zu, OS=%zu\n",inv.count,inv.set_count);
            size_t telnet_count=0;for(size_t i=0;i<inv.count;i++) if(inv.devices[i].protocol==PROTOCOL_TELNET) telnet_count++;
            printf("SSH=%zu TELNET=%zu\n",inv.count-telnet_count,telnet_count);
            for(size_t i=0;i<inv.set_count;i++) printf("%s: %zu commands\n",inv.sets[i].name,inv.sets[i].count);
            report(&o,"TEMPLATE_CHECK_OK devices=%zu OS=%zu",inv.count,inv.set_count);
            inventory_free(&inv);rc=0;
        } else { fprintf(stderr,"Template error: %s\n",error);report_at(&o,LOG_ERROR,"TEMPLATE_CHECK_FAILED: %s",error); }
    } else if(o.input && o.output) {
        cli_cancel=CreateEventW(NULL,TRUE,FALSE,NULL);o.cancel=cli_cancel;SetConsoleCtrlHandler(ctrl,TRUE);
        Summary summary;rc=collect(&o,&summary);SetConsoleCtrlHandler(ctrl,FALSE);CloseHandle(cli_cancel);
    } else cli_error("Specify --input and --output; use --help\n");
    LocalFree(argv);return rc;
}
static int program(void) {
    if(!app_log_start()) fputs("Application log unavailable; collection will still require a writable task log.\n",stderr);
    int code=program_run();app_log_stop(code);return code;
}
#ifndef NETCOLLECT_GUI
int main(void) { return program(); }
#else
int WINAPI WinMain(HINSTANCE instance,HINSTANCE prev,LPSTR command,int show) { return program(); }
#endif
