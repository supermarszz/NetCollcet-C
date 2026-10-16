#include "collector.h"
#include "miniz.h"
#include <expat.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct { char *name, *rid, *target; } Sheet;
typedef struct {
    XML_Parser parser;
    Inventory *inv;
    Sheet *sheets; size_t sheet_count;
    char **strings; size_t string_count;
    size_t device_capacity,string_capacity,sheet_capacity,command_capacity;
    int mode, in_text, in_value, in_cell, phonetic, header, command_col;
    unsigned row_number, col;
    char type[24];
    Buffer value, text;
    char *cells[256];
    int fields[8];
    CommandSet *set;
    char *error; size_t error_size;
    int failed;
} Reader;
static void *grow(void *data,size_t *capacity,size_t used,size_t item_size) {
    if(used<*capacity) return data;
    *capacity=*capacity?*capacity*2:32;
    void *p=realloc(data,*capacity*item_size);if(!p) abort();return p;
}
typedef struct { char *key; unsigned next_suffix; } FileKey;
static char *file_key(const char *text) {
    wchar_t *w=wide(text);int n=LCMapStringW(LOCALE_INVARIANT,LCMAP_UPPERCASE,w,-1,NULL,0);
    wchar_t *upper=mem_alloc(((size_t)n+1)*sizeof(*upper));
    if(n) LCMapStringW(LOCALE_INVARIANT,LCMAP_UPPERCASE,w,-1,upper,n);
    char *key=utf8(n?upper:w);free(upper);free(w);return key;
}
static FileKey *file_slot(FileKey *table,size_t capacity,const char *key) {
    uint64_t hash=14695981039346656037ull;
    for(const unsigned char *p=(const unsigned char*)key;*p;p++) { hash^=*p;hash*=1099511628211ull; }
    size_t index=(size_t)hash & (capacity-1);
    while(table[index].key && strcmp(table[index].key,key)) index=(index+1)&(capacity-1);
    return &table[index];
}
static void prepare_names(CommandSet *set) {
    size_t capacity=32;while(capacity<set->count*4) capacity*=2;
    FileKey *table=mem_alloc(capacity*sizeof(*table));set->files=mem_alloc(set->count*sizeof(char*));
    for(size_t i=0;i<set->count;i++) {
        char *base=safe_filename(set->commands[i]);Buffer name={0};buf_text(&name,base);buf_text(&name,".txt");
        char *key=file_key(name.data);FileKey *root=file_slot(table,capacity,key),*slot=root;
        if(root->key) {
            free(key);
            do {
                if(!root->next_suffix) root->next_suffix=2;
                char suffix[32];snprintf(suffix,sizeof(suffix)," (%u).txt",root->next_suffix++);
                buf_free(&name);buf_text(&name,base);buf_text(&name,suffix);
                key=file_key(name.data);slot=file_slot(table,capacity,key);
                if(slot->key) { free(key);key=NULL; }
            } while(!key);
        }
        slot->key=key;slot->next_suffix=2;set->files[i]=name.data;free(base);
    }
    for(size_t i=0;i<capacity;i++) free(table[i].key);free(table);
}
static const char *local(const char *s) { const char *p=strrchr(s,'|'); return p?p+1:s; }
static const char *attr(const char **attrs,const char *key) {
    for (int i=0;attrs[i];i+=2) if (!strcmp(local(attrs[i]),key)) return attrs[i+1]; return "";
}
static void fail(Reader *r,const char *message) {
    if (!r->failed) snprintf(r->error,r->error_size,"%s (Excel row %u)",message,r->row_number);
    r->failed=1; if(r->parser) XML_StopParser(r->parser,XML_FALSE);
}
static char *trim_copy(const char *s) {
    while (isspace((unsigned char)*s)) s++;
    size_t n=strlen(s); while(n && isspace((unsigned char)s[n-1])) n--;
    char *p=mem_alloc(n+1); memcpy(p,s,n); return p;
}
static const char *cell(Reader *r,int n) { return n>=0 && r->cells[n]?r->cells[n]:""; }
static int number(const char *s,unsigned def,unsigned min,unsigned max,unsigned *out) {
    if(!*s) { *out=def; return 1; }
    char *end; unsigned long n=strtoul(s,&end,10);
    while(isspace((unsigned char)*end)) end++;
    if(*end || n<min || n>max) return 0; *out=(unsigned)n; return 1;
}
static void append_row(Reader *r) {
    int nonempty=0; for(int i=0;i<256;i++) if(r->cells[i] && *r->cells[i]) nonempty=1;
    if(!nonempty || r->failed) return;
    if(!r->header) {
        r->header=1;
        if(r->mode==3) {
            const char *headers[]={"ip地址","用户名","密码","OS类型","端口","编码","超时秒","登录方式"};
            for(int i=0;i<8;i++) r->fields[i]=-1;
            for(int i=0;i<256;i++) if(r->cells[i]) {
                char *t=trim_copy(r->cells[i]);
                for(int j=0;j<8;j++) if(!_stricmp(t,headers[j])) { if(r->fields[j]>=0) fail(r,"Duplicate header"); r->fields[j]=i; }
                free(t);
            }
            for(int j=0;j<4;j++) if(r->fields[j]<0) { fail(r,"Missing required header: ip地址 / 用户名 / 密码 / OS类型"); return; }
            return;
        }
        r->command_col=0;
        for(int i=0;i<256;i++) if(r->cells[i]) {
            char *t=trim_copy(r->cells[i]); int match=!_stricmp(t,"命令") || !_stricmp(t,"command") || !_stricmp(t,"cmd"); free(t);
            if(match) { r->command_col=i; return; }
        }
    }
    if(r->mode==3) {
        if(r->inv->count>=100000) { fail(r,"Too many devices (limit 100000)"); return; }
        Device d={0};
        d.ip=trim_copy(cell(r,r->fields[0])); d.username=trim_copy(cell(r,r->fields[1]));
        d.password=str_copy(cell(r,r->fields[2])); d.os=trim_copy(cell(r,r->fields[3]));
        d.encoding=trim_copy(cell(r,r->fields[5])); if(!*d.encoding) { free(d.encoding); d.encoding=str_copy("auto"); }
        char *protocol=trim_copy(cell(r,r->fields[7]));
        int protocol_ok=!*protocol || !_stricmp(protocol,"SSH") || !_stricmp(protocol,"TELNET");
        d.protocol=!_stricmp(protocol,"TELNET")?PROTOCOL_TELNET:PROTOCOL_SSH;free(protocol);
        int valid=protocol_ok && *d.ip && (*d.username || d.protocol==PROTOCOL_TELNET) && *d.password && *d.os;
        valid=valid && number(cell(r,r->fields[4]),d.protocol==PROTOCOL_TELNET?23:22,1,65535,&d.port) && number(cell(r,r->fields[6]),90,1,3600,&d.timeout);
        if(d.protocol==PROTOCOL_TELNET && (strpbrk(d.username,"\r\n") || strpbrk(d.password,"\r\n") || strlen(d.username)>8192 || strlen(d.password)>8192)) valid=0;
        const char *encs[]={"auto","utf-8","gb18030","gbk","big5","latin1"}; int enc_ok=0;
        for(int i=0;i<6;i++) if(!_stricmp(d.encoding,encs[i])) enc_ok=1;
        if(!valid || !enc_ok || strpbrk(d.ip,"\r\n\t ")) {
            free(d.ip);free(d.username);free(d.password);free(d.os);free(d.encoding);
            fail(r,"Invalid device fields, port, timeout, encoding or 登录方式 (SSH/TELNET)"); return;
        }
        r->inv->devices=grow(r->inv->devices,&r->device_capacity,r->inv->count,sizeof(Device));
        if(!r->inv->devices) abort(); r->inv->devices[r->inv->count++]=d;
    } else {
        char *cmd=trim_copy(cell(r,r->command_col));
        if(*cmd && *cmd!='#') {
            if(strlen(cmd)>8192 || strpbrk(cmd,"\r\n") || r->set->count>=10000) { free(cmd); fail(r,"Command too long, multiline or too many commands"); return; }
            r->set->commands=grow(r->set->commands,&r->command_capacity,r->set->count,sizeof(char*));
            if(!r->set->commands) abort(); r->set->commands[r->set->count++]=cmd;
        } else free(cmd);
    }
}
static void XMLCALL start(void *ctx,const char *tag,const char **attrs) {
    Reader *r=ctx; tag=local(tag); if(r->failed) return;
    if(r->mode==0 && !strcmp(tag,"sheet")) {
        r->sheets=grow(r->sheets,&r->sheet_capacity,r->sheet_count,sizeof(Sheet)); if(!r->sheets) abort();
        Sheet *s=&r->sheets[r->sheet_count++]; s->name=trim_copy(attr(attrs,"name")); s->rid=str_copy(attr(attrs,"id")); s->target=NULL;
    } else if(r->mode==1 && !strcmp(tag,"Relationship")) {
        if(!_stricmp(attr(attrs,"TargetMode"),"External")) return;
        for(size_t i=0;i<r->sheet_count;i++) if(!strcmp(r->sheets[i].rid,attr(attrs,"Id"))) {
            const char *t=attr(attrs,"Target"); if(strstr(t,"..") || strchr(t,'\\')) { fail(r,"Invalid worksheet relationship"); return; }
            Buffer b={0}; if(*t=='/') t++; else if(strncmp(t,"xl/",3)) buf_text(&b,"xl/");
            buf_text(&b,t); free(r->sheets[i].target); r->sheets[i].target=b.data;
        }
    } else if(r->mode==2) {
        if(!strcmp(tag,"si")) { buf_free(&r->text); r->phonetic=0; }
        if(!strcmp(tag,"rPh")) r->phonetic=1;
        if(!strcmp(tag,"t") && !r->phonetic) r->in_text=1;
    } else if(r->mode>=3) {
        if(!strcmp(tag,"row")) r->row_number=(unsigned)strtoul(attr(attrs,"r"),NULL,10);
        else if(!strcmp(tag,"c")) {
            const char *p=attr(attrs,"r"); unsigned col=0;
            while(*p>='A' && *p<='Z') { col=col*26+(unsigned)(*p-'A'+1); p++; if(col>256) break; }
            r->col=col?col-1:256; r->in_cell=1;
            snprintf(r->type,sizeof(r->type),"%s",attr(attrs,"t")); buf_free(&r->value);
        } else if(r->in_cell && (!strcmp(tag,"v") || !strcmp(tag,"t"))) r->in_value=1;
    }
}
static void XMLCALL content(void *ctx,const char *text,int len) {
    Reader *r=ctx; if(r->failed) return;
    Buffer *b=NULL;
    if(r->mode==2 && r->in_text) b=&r->text;
    else if(r->mode>=3 && r->in_value) b=&r->value;
    if(b) { if(b->len+(size_t)len>1048576) fail(r,"Cell exceeds 1 MiB"); else buf_add(b,text,(size_t)len); }
}
static void XMLCALL end(void *ctx,const char *tag) {
    Reader *r=ctx; tag=local(tag); if(r->failed) return;
    if(r->mode==2) {
        if(!strcmp(tag,"t")) r->in_text=0;
        if(!strcmp(tag,"rPh")) r->phonetic=0;
        if(!strcmp(tag,"si")) {
            if(r->string_count>=1000000) { fail(r,"Too many shared strings"); return; }
            r->strings=grow(r->strings,&r->string_capacity,r->string_count,sizeof(char*)); if(!r->strings) abort();
            r->strings[r->string_count++]=str_copy(r->text.data?r->text.data:"");
        }
    } else if(r->mode>=3) {
        if(!strcmp(tag,"v") || !strcmp(tag,"t")) r->in_value=0;
        else if(!strcmp(tag,"c")) {
            if(r->col<256) {
                const char *v=r->value.data?r->value.data:"";
                if(!strcmp(r->type,"s")) {
                    char *tail; unsigned long idx=strtoul(v,&tail,10);
                    if(!*v || *tail || idx>=r->string_count) { fail(r,"Bad shared string reference"); return; }
                    v=r->strings[idx];
                }
                free(r->cells[r->col]); r->cells[r->col]=str_copy(v);
            }
            r->in_cell=0;
        } else if(!strcmp(tag,"row")) {
            append_row(r); for(int i=0;i<256;i++) { free(r->cells[i]); r->cells[i]=NULL; }
        }
    }
}
static void XMLCALL doctype(void *ctx,const char *name,const char *sys,const char *pub,int internal) { fail(ctx,"DTD is not allowed in XLSX"); }
static int parse(Reader *r,mz_zip_archive *zip,const char *member,int optional) {
    int index=mz_zip_reader_locate_file(zip,member,NULL,0);
    if(index<0) { if(!optional) fail(r,"Missing XLSX part"); return optional; }
    mz_zip_archive_file_stat stat;
    if(!mz_zip_reader_file_stat(zip,(mz_uint)index,&stat) || stat.m_uncomp_size>MAX_ZIP_BYTES) { fail(r,"XLSX part exceeds size limit"); return 0; }
    size_t size; char *xml=mz_zip_reader_extract_to_heap(zip,(mz_uint)index,&size,0);
    if(!xml) { fail(r,"Unable to decompress XLSX"); return 0; }
    r->parser=XML_ParserCreateNS(NULL,'|'); if(!r->parser) abort();
    XML_SetUserData(r->parser,r); XML_SetElementHandler(r->parser,start,end); XML_SetCharacterDataHandler(r->parser,content);
    XML_SetStartDoctypeDeclHandler(r->parser,doctype);
    if(XML_Parse(r->parser,xml,(int)size,1)==XML_STATUS_ERROR && !r->failed) fail(r,"Invalid XML in XLSX");
    XML_ParserFree(r->parser); r->parser=NULL; mz_free(xml); return !r->failed;
}
static int compare_device(const void *a,const void *b) {
    const Device *x=*(const Device *const *)a,*y=*(const Device *const *)b;
    int cmp=_stricmp(x->ip,y->ip); return cmp?cmp:(x->port>y->port)-(x->port<y->port);
}
int inventory_load(const wchar_t *path,Inventory *inv,char *error,size_t size) {
    memset(inv,0,sizeof(*inv)); error[0]=0;
    wchar_t *absolute=full_path(path);FILE *f=absolute?_wfopen(absolute,L"rb"):NULL;free(absolute);
    if(!f) { snprintf(error,size,"Cannot open Excel file"); return 0; }
    _fseeki64(f,0,SEEK_END); long long length=_ftelli64(f); rewind(f);
    if(length<=0 || length>MAX_ZIP_BYTES) { fclose(f); snprintf(error,size,"Excel must be an unencrypted .xlsx/.xlsm file below 256 MiB"); return 0; }
    char *data=mem_alloc((size_t)length); int read_ok=fread(data,1,(size_t)length,f)==(size_t)length; fclose(f);
    mz_zip_archive zip={0};
    if(!read_ok || !mz_zip_reader_init_mem(&zip,data,(size_t)length,0)) { free(data); snprintf(error,size,"Invalid XLSX (legacy .xls and encrypted files are unsupported)"); return 0; }
    Reader r={0}; r.inv=inv;r.error=error;r.error_size=size;
    if(!parse(&r,&zip,"xl/workbook.xml",0) || !r.sheet_count) { if(!r.failed) fail(&r,"No worksheets"); goto done; }
    r.mode=1; if(!parse(&r,&zip,"xl/_rels/workbook.xml.rels",0)) goto done;
    r.mode=2; if(!parse(&r,&zip,"xl/sharedStrings.xml",1)) goto done;
    r.mode=3; if(!r.sheets[0].target || !parse(&r,&zip,r.sheets[0].target,0)) { if(!r.failed) fail(&r,"Invalid inventory sheet relationship"); goto done; }
    if(!inv->count) { fail(&r,"Inventory contains no devices"); goto done; }
    Device **sorted=mem_alloc(inv->count*sizeof(*sorted)); for(size_t i=0;i<inv->count;i++) sorted[i]=&inv->devices[i];
    qsort(sorted,inv->count,sizeof(*sorted),compare_device);
    for(size_t i=1;i<inv->count;i++) if(!compare_device(&sorted[i-1],&sorted[i])) { fail(&r,"Duplicate management address and port"); break; }
    free(sorted); if(r.failed) goto done;
    for(size_t i=0;i<inv->count;i++) {
        size_t j; for(j=0;j<inv->set_count;j++) if(!strcmp(inv->sets[j].name,inv->devices[i].os)) break;
        if(j==inv->set_count) {
            size_t k; for(k=1;k<r.sheet_count;k++) if(!strcmp(r.sheets[k].name,inv->devices[i].os)) break;
            if(k==r.sheet_count || !r.sheets[k].target) { fail(&r,"OS type has no matching command sheet"); goto done; }
            inv->sets=realloc(inv->sets,(inv->set_count+1)*sizeof(CommandSet)); if(!inv->sets) abort();
            r.set=&inv->sets[inv->set_count++]; memset(r.set,0,sizeof(*r.set)); r.set->name=str_copy(inv->devices[i].os);
            r.mode=4;r.header=0;r.in_cell=0;r.in_value=0;r.command_capacity=0;
            if(!parse(&r,&zip,r.sheets[k].target,0)) goto done;
            if(!r.set->count) { fail(&r,"Command sheet is empty"); goto done; }
            prepare_names(r.set);
        }
        inv->devices[i].set=j;
    }
done:
    for(size_t i=0;i<r.sheet_count;i++) { free(r.sheets[i].name);free(r.sheets[i].rid);free(r.sheets[i].target); }
    free(r.sheets); for(size_t i=0;i<r.string_count;i++) free(r.strings[i]); free(r.strings);
    for(int i=0;i<256;i++) free(r.cells[i]); buf_free(&r.value);buf_free(&r.text);
    mz_zip_reader_end(&zip);free(data);
    if(r.failed) { inventory_free(inv); return 0; } return 1;
}
void inventory_free(Inventory *inv) {
    for(size_t i=0;i<inv->count;i++) {
        Device *d=&inv->devices[i]; free(d->ip);free(d->username);
        if(d->password) { SecureZeroMemory(d->password,strlen(d->password));free(d->password); }
        free(d->os);free(d->encoding);
    }
    for(size_t i=0;i<inv->set_count;i++) {
        for(size_t j=0;j<inv->sets[i].count;j++) { free(inv->sets[i].commands[j]);if(inv->sets[i].files) free(inv->sets[i].files[j]); }
        free(inv->sets[i].files);free(inv->sets[i].commands);free(inv->sets[i].name);
    }
    free(inv->devices);free(inv->sets);memset(inv,0,sizeof(*inv));
}
