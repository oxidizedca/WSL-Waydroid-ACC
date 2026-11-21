"""Build-only filter experiment; keep the production PNG format and all fallback checks."""
import sys
from pathlib import Path

src = Path('src/adb-png.c').read_text()
def change(before,after):
    global src
    if src.count(before)!=1:
        raise ValueError('production encoder changed: '+before)
    src=src.replace(before,after)

# Rank predictors by the signed-byte residual of a sample spread over the row.
# Filter and validate every byte, regardless of the sampling used to choose a predictor.
change('int main(void) {', '''static inline unsigned char paeth(int a,int b,int c) {
    int ac=abs(b-c),bc=abs(a-c),cc=abs(a+b-2*c);
    return ac<=bc && ac<=cc ? a : bc<=cc ? b : c;
}
static inline unsigned cost(unsigned a,unsigned b) {
    unsigned d=(a-b)&255u;
    return d<128 ? d : 256-d;
}
int main(void) {''')
change('    for (unsigned y=', '''    unsigned char previous[1920*4]={0};
    for (unsigned y=''')
change('        for (size_t i=stride;i>4;--i) row[i]-=row[i-4];','''        unsigned char *v=row+1;
        int mode=FILTER;
        if(mode==0) {
            unsigned sub=0,up=0,p=0;
            for (size_t x=4;x+3<stride;x+=32) {
                for (size_t ch=0;ch<3;ch++) {
                    size_t i=x+ch;
                    sub+=cost(v[i],v[i-4]);up+=cost(v[i],previous[i]);
                    p+=cost(v[i],paeth(v[i-4],previous[i],previous[i-4]));
                }
            }
            mode=sub<=up && sub<=p ? 1 : up<=p ? 2 : 4;
        }
        row[0]=mode;
        unsigned char orig[1920*4];
        memcpy(orig,v,stride);
        if(mode==1) {
            for(size_t i=4;i<stride;i++)v[i]=orig[i]-orig[i-4];
        } else if(mode==2) {
            for(size_t i=0;i<stride;i++)v[i]=orig[i]-previous[i];
        } else {
            for(size_t i=0;i<4;i++)v[i]=orig[i]-previous[i];
            for(size_t i=4;i<stride;i++)v[i]=orig[i]-paeth(orig[i-4],previous[i],previous[i-4]);
        }
        memcpy(previous,orig,stride);''')
change('libdeflate_alloc_compressor(1)','libdeflate_alloc_compressor(LEVEL)')
Path(sys.argv[1]).write_text(src)
