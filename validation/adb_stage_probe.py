"""Generate a timing-only copy of the deployed encoder in a temporary build directory."""
from pathlib import Path
import sys
p=Path(sys.argv[1]);s=Path('src/adb-png.c').read_text()
def inject(old,new):
    global s
    if s.count(old)!=1:raise RuntimeError(f"source marker changed: {old}")
    s=s.replace(old,new)
inject('int main(void) {','''#include <time.h>
#include <inttypes.h>
static int64_t mark(void) {
    struct timespec t; if(clock_gettime(CLOCK_MONOTONIC,&t)!=0) exit(1);
    return (int64_t)t.tv_sec*1000000000+t.tv_nsec;
}
int main(void) {''')
inject('    size_t stride =','    int64_t header=mark();\n    size_t stride =')
inject('    struct libdeflate_compressor *c=', '    int64_t eof=mark();\n    struct libdeflate_compressor *c=')
inject('    return write_all(png,p-png);','''    int64_t ready=mark();
    int rc=write_all(png,p-png);
    int64_t sent=mark();
    FILE *f=fopen("pipe-times","w");
    if(!f)return 1;
    fprintf(f,"%" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 "\\n",header,eof,ready,sent);
    return fclose(f)==0 ? rc : 1;''')
p.write_text(s)
