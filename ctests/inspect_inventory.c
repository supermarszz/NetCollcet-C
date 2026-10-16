#include "collector.h"
#include <shellapi.h>
#include <stdlib.h>
int main(void) {
    int argc;wchar_t **argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(argc!=2) { LocalFree(argv);return 2; }
    Inventory inventory;char error[512];
    if(!inventory_load(argv[1],&inventory,error,sizeof(error))) { fputs(error,stderr);LocalFree(argv);return 2; }
    puts("[");
    for(size_t i=0;i<inventory.count;i++) printf("%s{\"protocol\":\"%s\",\"port\":%u}",i?",":"",protocol_name(inventory.devices[i].protocol),inventory.devices[i].port);
    puts("]");inventory_free(&inventory);LocalFree(argv);return 0;
}
