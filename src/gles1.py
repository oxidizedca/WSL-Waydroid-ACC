"""Bridge the system GLES 1 front end to the GLES shim and Mesa entry points."""
import re
import subprocess
import sys
from pathlib import Path

ndk, output = map(Path, sys.argv[1:])
imports = Path(__file__).with_name("gles1-imports.txt").read_text().splitlines()
headers = ndk.parent / "sysroot/usr/include/GLES"
decls = {}
for header in ("gl.h", "glext.h"):
    for result, name, args in re.findall(
        r"^GL_API\s+(.+?)\s*GL_APIENTRY\s+(gl\w+)\s*\(([^)]*)\);",
        (headers / header).read_text(), re.MULTILINE
    ):
        decls[name] = (result, args)

symbols = subprocess.check_output(
    [str(ndk / "llvm-nm"), "--extern-only", "--defined-only",
     str(output / "gles.o"), str(output / "stubs.o")], text=True
)
existing = set(re.findall(r"\bT (gl\w+)$", symbols, re.MULTILINE))
source = ['#include <GLES/gl.h>', '#include <GLES/glext.h>',
          'extern void *shim_lazy_gl(const char *name);']
aliases = []
for symbol in imports:
    name = "gl" + symbol.removeprefix("GL_")
    if name in existing:
        # Bind locally to the same implementation used by GLES 2. In particular
        # glGetString must never resolve back to Android's public GL wrapper.
        aliases.append(f"-Wl,--defsym,{symbol}={name}")
        continue
    result, args = decls[name]
    params = [] if args == "void" else [
        re.search(r"(\w+)(?:\s*\[\s*\d*\s*\])?\s*$", p).group(1) for p in args.split(",")
    ]
    source += [f'{result} {symbol}({args}) {{',
               f'    typedef {result} (*Proc)({args});',
               '    static Proc cached;',
               '    Proc proc = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);',
               '    if (!proc) {',
               f'        proc = (Proc)shim_lazy_gl("{name}");',
               '        if (proc) __atomic_store_n(&cached, proc, __ATOMIC_RELEASE);',
               '    }']
    call = f'proc({", ".join(params)})'
    source.append(f'    if (proc) {call};' if result == 'void' else f'    return proc ? {call} : 0;')
    source.append('}')

(output / "fixed.c").write_text("\n".join(source) + "\n")
(output / "aliases.rsp").write_text("\n".join(aliases) + "\n")
