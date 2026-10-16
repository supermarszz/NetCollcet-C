#include "collector.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <stdlib.h>
#include <process.h>
#include <wchar.h>
#include <shellapi.h>

#define LOG_MESSAGE (WM_APP+1)
#define DONE_MESSAGE (WM_APP+2)
enum { ID_INPUT=101,ID_OUTPUT,ID_BROWSE,ID_FOLDER,ID_WORKERS,ID_START,ID_CANCEL,ID_CHECK,ID_LOG,ID_STATUS,ID_LOG_FOLDER };
typedef struct {
    HWND window,input,output,workers,log,status,start,cancel,check,browse,folder,spin,logs;
    HWND labels[4];
    HFONT font;
    HANDLE thread,event;
    int running,closing;
    wchar_t input_path[32768],output_path[32768];
    Options options;
} App;
static App app;
static HWND control(const wchar_t *type,const wchar_t *text,DWORD style,int id) {
    HWND h=CreateWindowExW(!wcscmp(type,L"EDIT")?WS_EX_CLIENTEDGE:0,type,text,WS_CHILD|WS_VISIBLE|style,0,0,0,0,app.window,(HMENU)(INT_PTR)id,GetModuleHandleW(NULL),NULL);
    SendMessageW(h,WM_SETFONT,(WPARAM)app.font,TRUE);return h;
}
static void layout(void) {
    RECT r;GetClientRect(app.window,&r);int width=r.right,height=r.bottom;
    int pad=20,label=100,button=100,gap=10,edit=width-2*pad-label-button-gap*2;
    for(int i=0;i<2;i++) {
        MoveWindow(app.labels[i],pad,24+i*44,label,24,TRUE);
        MoveWindow(i?app.output:app.input,pad+label+gap,20+i*44,edit,30,TRUE);
        MoveWindow(i?app.folder:app.browse,width-pad-button,20+i*44,button,30,TRUE);
    }
    MoveWindow(app.labels[2],pad,112,label,24,TRUE);MoveWindow(app.workers,pad+label+gap,108,65,30,TRUE);
    MoveWindow(app.spin,pad+label+gap+65,108,20,30,TRUE);
    MoveWindow(app.check,230,108,100,30,TRUE);MoveWindow(app.start,340,108,100,30,TRUE);MoveWindow(app.cancel,450,108,100,30,TRUE);
    MoveWindow(app.logs,560,108,110,30,TRUE);
    MoveWindow(app.labels[3],pad,158,100,24,TRUE);MoveWindow(app.log,pad,188,width-2*pad,height-248,TRUE);
    MoveWindow(app.status,pad,height-42,width-2*pad,28,TRUE);
}
static void gui_log(void *context,const char *text) {
    char *copy=str_copy(text);if(!PostMessageW((HWND)context,LOG_MESSAGE,0,(LPARAM)copy)) free(copy);
}
static unsigned __stdcall run(void *unused) {
    Summary summary;int code=collect(&app.options,&summary);PostMessageW(app.window,DONE_MESSAGE,(WPARAM)code,0);return 0;
}
static void running(int value) {
    app.running=value;
    HWND fields[]={app.input,app.output,app.workers,app.spin,app.check,app.start,app.browse,app.folder};
    for(size_t i=0;i<sizeof(fields)/sizeof(fields[0]);i++) EnableWindow(fields[i],!value);
    EnableWindow(app.cancel,value);SetWindowTextW(app.status,value?L"正在采集…":L"任务已结束");
}
static void choose_file(void) {
    wchar_t path[32768]={0};OPENFILENAMEW of={0};of.lStructSize=sizeof(of);of.hwndOwner=app.window;
    of.lpstrFilter=L"Excel 工作簿 (*.xlsx;*.xlsm)\0*.xlsx;*.xlsm\0\0";of.lpstrFile=path;of.nMaxFile=32768;of.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    if(GetOpenFileNameW(&of)) SetWindowTextW(app.input,path);
}
static void choose_folder(void) {
    BROWSEINFOW bi={0};bi.hwndOwner=app.window;bi.lpszTitle=L"选择采集结果输出目录";bi.ulFlags=BIF_RETURNONLYFSDIRS|BIF_NEWDIALOGSTYLE|BIF_EDITBOX;
    PIDLIST_ABSOLUTE item=SHBrowseForFolderW(&bi);
    if(item) { wchar_t path[32768];if(SHGetPathFromIDListEx(item,path,32768,0)) SetWindowTextW(app.output,path);CoTaskMemFree(item); }
}
static void open_logs(void) {
    wchar_t selection[32768];GetWindowTextW(app.output,selection,32768);
    wchar_t *directory=*selection?full_path(selection):NULL;
    DWORD attributes=directory?GetFileAttributesW(directory):INVALID_FILE_ATTRIBUTES;
    if(attributes==INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        free(directory);directory=NULL;const wchar_t *fallback=app_log_directory();
        if(fallback) { directory=mem_alloc((wcslen(fallback)+1)*sizeof(*directory));wcscpy(directory,fallback); }
    }
    if(directory) {
        /* Explorer expects a normal path; file IO itself retains long-path support. */
        const wchar_t *display=directory;if(!wcsncmp(display,L"\\\\?\\",4) && wcsncmp(display,L"\\\\?\\UNC\\",8)) display+=4;
        HINSTANCE result=ShellExecuteW(app.window,L"open",display,NULL,NULL,SW_SHOWNORMAL);
        if((INT_PTR)result<=32) MessageBoxW(app.window,L"无法打开日志目录，请根据运行日志中显示的路径查看。",L"日志目录",MB_ICONERROR);
        free(directory);
    } else MessageBoxW(app.window,L"当前未能创建日志目录。",L"日志目录",MB_ICONERROR);
}
static void start_task(int check) {
    GetWindowTextW(app.input,app.input_path,32768);GetWindowTextW(app.output,app.output_path,32768);
    if(!*app.input_path) { MessageBoxW(app.window,L"请选择 Excel 模板。",L"模板",MB_ICONINFORMATION);return; }
    if(check) {
        Inventory inv;char error[512];
        app_log_write(LOG_INFO,"GUI_TEMPLATE_CHECK requested");
        if(inventory_load(app.input_path,&inv,error,sizeof(error))) {
            size_t telnet_count=0;for(size_t i=0;i<inv.count;i++) if(inv.devices[i].protocol==PROTOCOL_TELNET) telnet_count++;
            wchar_t text[240];swprintf(text,240,L"模板有效：%zu 台设备，%zu 种 OS 类型。\nSSH：%zu 台，Telnet：%zu 台。",inv.count,inv.set_count,inv.count-telnet_count,telnet_count);
            char *message=utf8(text);app_log_write(LOG_INFO,message);free(message);
            inventory_free(&inv);MessageBoxW(app.window,text,L"模板检查",MB_OK|MB_ICONINFORMATION);
        } else { app_log_write(LOG_ERROR,error);wchar_t *text=wide(error);MessageBoxW(app.window,text,L"模板错误",MB_OK|MB_ICONERROR);free(text); }
        return;
    }
    if(!*app.output_path) { MessageBoxW(app.window,L"请选择输出目录。",L"输出目录",MB_ICONINFORMATION);return; }
    wchar_t workers[32],*end;GetWindowTextW(app.workers,workers,32);unsigned long n=wcstoul(workers,&end,10);
    if(*end || n<1 || n>32) { MessageBoxW(app.window,L"并发数须为 1 到 32。",L"并发设备数",MB_OK|MB_ICONERROR);return; }
    ResetEvent(app.event);SetWindowTextW(app.log,L"");
    app.options=(Options){.input=app.input_path,.output=app.output_path,.workers=(unsigned)n,.cancel=app.event,.log=gui_log,.log_ctx=app.window};
    running(1);app.thread=(HANDLE)_beginthreadex(NULL,0,run,NULL,0,NULL);
    if(!app.thread) { app_log_write(LOG_ERROR,"GUI_WORKER_CREATE_FAILED");running(0);MessageBoxW(app.window,L"无法创建采集线程。",L"错误",MB_ICONERROR); }
}
static LRESULT CALLBACK window_proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_CREATE: {
        app.window=hwnd;NONCLIENTMETRICSW metrics={0};metrics.cbSize=sizeof(metrics);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS,sizeof(metrics),&metrics,0);app.font=CreateFontIndirectW(&metrics.lfMessageFont);
        app.labels[0]=control(L"STATIC",L"Excel 模板",0,0);app.labels[1]=control(L"STATIC",L"输出目录",0,0);
        app.labels[2]=control(L"STATIC",L"并发设备数",0,0);app.labels[3]=control(L"STATIC",L"运行日志",0,0);
        app.input=control(L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,ID_INPUT);app.output=control(L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,ID_OUTPUT);
        app.browse=control(L"BUTTON",L"选择文件…",WS_TABSTOP,ID_BROWSE);app.folder=control(L"BUTTON",L"选择目录…",WS_TABSTOP,ID_FOLDER);
        app.workers=control(L"EDIT",L"4",ES_NUMBER|WS_TABSTOP,ID_WORKERS);
        app.spin=control(UPDOWN_CLASSW,L"",UDS_SETBUDDYINT|UDS_ARROWKEYS,0);SendMessageW(app.spin,UDM_SETBUDDY,(WPARAM)app.workers,0);SendMessageW(app.spin,UDM_SETRANGE32,1,32);SendMessageW(app.spin,UDM_SETPOS32,0,4);
        app.check=control(L"BUTTON",L"检查模板",WS_TABSTOP,ID_CHECK);app.start=control(L"BUTTON",L"开始采集",WS_TABSTOP|BS_DEFPUSHBUTTON,ID_START);app.cancel=control(L"BUTTON",L"停止采集",WS_TABSTOP,ID_CANCEL);
        app.logs=control(L"BUTTON",L"打开日志目录",WS_TABSTOP,ID_LOG_FOLDER);
        app.log=control(L"EDIT",L"",ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL,ID_LOG);SendMessageW(app.log,EM_SETLIMITTEXT,200000,0);
        app.status=control(L"STATIC",L"就绪",SS_LEFTNOWORDWRAP,ID_STATUS);EnableWindow(app.cancel,FALSE);
        app.event=CreateEventW(NULL,TRUE,FALSE,NULL);layout();
        app_log_start();app_log_write(LOG_INFO,"GUI_OPEN");
        const wchar_t *logs=app_log_directory();
        if(logs) { char *path=utf8(logs);Buffer message={0};buf_text(&message,"应用日志目录: ");buf_text(&message,path);gui_log(hwnd,message.data);free(path);buf_free(&message); }
        else gui_log(hwnd,"ERROR: 无法创建应用日志，采集时将使用输出目录中的任务日志");
        return 0;
    }
    case WM_SIZE:layout();return 0;
    case WM_GETMINMAXINFO:((MINMAXINFO*)lp)->ptMinTrackSize=(POINT){720,450};return 0;
    case WM_COMMAND:
        if(HIWORD(wp)!=BN_CLICKED) break;
        switch(LOWORD(wp)) { case ID_BROWSE:choose_file();break;case ID_FOLDER:choose_folder();break;case ID_CHECK:start_task(1);break;case ID_START:start_task(0);break;
        case ID_CANCEL:app_log_write(LOG_WARN,"GUI_CANCEL requested");SetEvent(app.event);SetWindowTextW(app.status,L"正在停止，保存已接收结果…");EnableWindow(app.cancel,FALSE);break;
        case ID_LOG_FOLDER:open_logs();break; }return 0;
    case LOG_MESSAGE: {
        char *text=(char*)lp;wchar_t *w=wide(text);free(text);if(!w) return 0;
        int n=GetWindowTextLengthW(app.log);
        if(n>150000) { SendMessageW(app.log,EM_SETSEL,0,30000);SendMessageW(app.log,EM_REPLACESEL,FALSE,(LPARAM)L""); }
        SendMessageW(app.log,EM_SETSEL,(WPARAM)-1,(LPARAM)-1);SendMessageW(app.log,EM_REPLACESEL,FALSE,(LPARAM)w);SendMessageW(app.log,EM_REPLACESEL,FALSE,(LPARAM)L"\r\n");SendMessageW(app.log,EM_SCROLLCARET,0,0);free(w);return 0;
    }
    case DONE_MESSAGE:
        if(app.thread) { WaitForSingleObject(app.thread,INFINITE);CloseHandle(app.thread);app.thread=NULL; }running(0);
        SetWindowTextW(app.status,wp==0?L"采集完成，全部成功":wp==3?L"采集已停止":L"任务结束，详细结果见日志及 _summary.csv");
        if(app.closing) DestroyWindow(hwnd);return 0;
    case WM_CLOSE:
        if(app.running) { app_log_write(LOG_WARN,"GUI_CLOSE requested during collection; cancelling");app.closing=1;SetEvent(app.event);SetWindowTextW(app.status,L"正在停止并关闭…"); }
        else DestroyWindow(hwnd);return 0;
    case WM_DESTROY:app_log_write(LOG_INFO,"GUI_CLOSED");CloseHandle(app.event);DeleteObject(app.font);PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
int gui_main(HINSTANCE instance,int show) {
    CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);INITCOMMONCONTROLSEX cc={sizeof(cc),ICC_UPDOWN_CLASS|ICC_STANDARD_CLASSES};InitCommonControlsEx(&cc);
    WNDCLASSW wc={0};wc.lpfnWndProc=window_proc;wc.hInstance=instance;wc.lpszClassName=L"NetCollectC";wc.hCursor=LoadCursorW(NULL,IDC_ARROW);wc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);wc.hIcon=LoadIconW(NULL,IDI_APPLICATION);RegisterClassW(&wc);
    wchar_t *title=wide("NetCollcet-C " NETCOLLECT_VERSION " - 网络设备信息采集");
    HWND hwnd=CreateWindowW(wc.lpszClassName,title,WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,900,620,NULL,NULL,instance,NULL);free(title);
    if(!hwnd) { CoUninitialize();return 2; }ShowWindow(hwnd,show);UpdateWindow(hwnd);
    MSG msg;while(GetMessageW(&msg,NULL,0,0)>0) { if(!IsDialogMessageW(hwnd,&msg)) { TranslateMessage(&msg);DispatchMessageW(&msg); } }
    CoUninitialize();return (int)msg.wParam;
}
