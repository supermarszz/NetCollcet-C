#include "collector.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);return 1; } } while(0)
int main(void) {
    char *s=safe_filename("display interface | include up");CHECK(!strcmp(s,"display interface _ include up"));free(s);
    s=safe_filename("CON");CHECK(!strcmp(s,"_CON"));free(s);
    s=safe_filename("../x?y.");CHECK(!strcmp(s,".._x_y"));free(s);
    s=terminal_clean("\x1b[31m接口\x1b[0m\r\nabc\bD\x1b]0;title\x07\r\n");CHECK(!strcmp(s,"接口\nabD\n"));free(s);
    s=prompt_name("Welcome!\n<SW-Core> ");CHECK(s && !strcmp(s,"SW-Core"));free(s);
    s=prompt_name("\nrouter-1(config)#");CHECK(s && !strcmp(s,"router-1"));free(s);
    s=prompt_name("no prompt yet");CHECK(!s);
    s=prompt_name("admin@Junos-1>");CHECK(s && !strcmp(s,"Junos-1"));free(s);
    s=prompt_name("[~Huawei-Core]");CHECK(s && !strcmp(s,"Huawei-Core"));free(s);
    CHECK(ends_at_prompt("result\r\n[H3C-Core]","H3C-Core"));
    CHECK(!ends_at_prompt("result\n--More--","H3C-Core"));
    int lossy;const char gbk[]={ (char)0xbd,(char)0xd3,(char)0xbf,(char)0xda,0 };
    s=decode_output(gbk,4,"auto",&lossy);CHECK(!strcmp(s,"接口") && !lossy);free(s);
    s=decode_output("中文",strlen("中文"),"auto",&lossy);CHECK(!strcmp(s,"中文") && !lossy);free(s);
    s=decode_output("\xff",1,"utf-8",&lossy);CHECK(lossy);free(s);
    s=decode_output("a\0bc",4,"auto",&lossy);CHECK(!strcmp(s,"abc") && !lossy);free(s);
    puts("core_tests: all passed");return 0;
}
