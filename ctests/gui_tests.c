/* Exercise the real native window procedure and worker, including cancellation. */
#include "../csrc/gui.c"
#include <shellapi.h>
#include <string.h>

static int screenshot(HWND window,const wchar_t *path) {
    RECT r;GetWindowRect(window,&r);int w=r.right-r.left,h=r.bottom-r.top;
    HDC screen=GetDC(window),memory=CreateCompatibleDC(screen);
    BITMAPINFO info={0};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=w;info.bmiHeader.biHeight=-h;
    info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    void *pixels;HBITMAP bitmap=CreateDIBSection(screen,&info,DIB_RGB_COLORS,&pixels,NULL,0);HGDIOBJ previous=SelectObject(memory,bitmap);
    int ok=PrintWindow(window,memory,0);
    if(ok) {
        BITMAPFILEHEADER header={0};header.bfType=0x4d42;header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER);header.bfSize=header.bfOffBits+(DWORD)(w*h*4);
        FILE *f=_wfopen(path,L"wb");if(!f) ok=0;
        else { ok=fwrite(&header,1,sizeof(header),f)==sizeof(header) && fwrite(&info.bmiHeader,1,sizeof(info.bmiHeader),f)==sizeof(info.bmiHeader) && fwrite(pixels,1,(size_t)w*h*4,f)==(size_t)w*h*4;fclose(f); }
    }
    SelectObject(memory,previous);DeleteObject(bitmap);DeleteDC(memory);ReleaseDC(window,screen);return ok;
}
int main(void) {
    int argc;wchar_t **argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(argc!=5) { fputs("gui_tests input.xlsx output screenshot.bmp success|cancel\n",stderr);return 2; }
    int cancel=!wcscmp(argv[4],L"cancel"),code=-1,checked=0;uint64_t cancel_after=0;
    HINSTANCE instance=GetModuleHandleW(NULL);CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX cc={sizeof(cc),ICC_STANDARD_CLASSES|ICC_UPDOWN_CLASS};InitCommonControlsEx(&cc);
    WNDCLASSW wc={0};wc.lpfnWndProc=window_proc;wc.hInstance=instance;wc.lpszClassName=L"NetCollectGuiTest";wc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);wc.hCursor=LoadCursor(NULL,IDC_ARROW);RegisterClassW(&wc);
    HWND window=CreateWindowW(wc.lpszClassName,L"网络设备信息采集 - C 语言版",WS_OVERLAPPEDWINDOW,20,20,900,620,NULL,NULL,instance,NULL);
    if(!window) return 2;
    ShowWindow(window,SW_SHOWNOACTIVATE);UpdateWindow(window);
    SetWindowTextW(app.input,argv[1]);SetWindowTextW(app.output,argv[2]);SetWindowTextW(app.workers,L"1");
    PostMessageW(window,WM_COMMAND,MAKEWPARAM(ID_START,BN_CLICKED),0);
    uint64_t deadline=tick_ms()+25000;
    while(code<0 && tick_ms()<deadline) {
        MSG msg;
        while(PeekMessageW(&msg,NULL,0,0,PM_REMOVE)) {
            if(msg.message==LOG_MESSAGE && cancel && strstr((const char*)msg.lParam,"display stall")) cancel_after=tick_ms()+350;
            if(msg.message==DONE_MESSAGE) code=(int)msg.wParam;
            TranslateMessage(&msg);DispatchMessageW(&msg);
        }
        if(app.running && !checked) {
            if(IsWindowEnabled(app.start) || IsWindowEnabled(app.input) || !IsWindowEnabled(app.cancel)) { fputs("GUI state incorrect while running\n",stderr);return 1; }checked=1;
        }
        if(cancel_after && tick_ms()>=cancel_after) { SendMessageW(window,WM_COMMAND,MAKEWPARAM(ID_CANCEL,BN_CLICKED),0);cancel_after=0; }
        Sleep(10);
    }
    if(code<0) { SetEvent(app.event);if(app.thread) WaitForSingleObject(app.thread,20000);fputs("GUI task timeout\n",stderr);return 1; }
    int ok=checked && code==(cancel?3:0) && IsWindowEnabled(app.start) && !IsWindowEnabled(app.cancel);
    UpdateWindow(window);if(!screenshot(window,argv[3])) ok=0;
    DestroyWindow(window);app_log_stop(ok?0:1);LocalFree(argv);CoUninitialize();
    printf("gui_tests: mode=%s exit=%d states=%s\n",cancel?"cancel":"success",code,ok?"PASS":"FAIL");return ok?0:1;
}
