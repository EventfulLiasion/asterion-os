#!/usr/bin/env python3
"""Build a relocation-free x86-64 smoke ELF using available GNU binutils."""
import argparse, shutil, struct, subprocess
from pathlib import Path
USER_BASE, PAGE = 0x0000400000000000, 4096
MESSAGE = b"Hello from independently assembled hello.elf via syscall write.\r\n"
def run(args): return subprocess.run(args, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
def build(source: Path, elf_path: Path, c_path: Path) -> None:
    assembler, objcopy, objdump = shutil.which("as"), shutil.which("objcopy"), shutil.which("objdump")
    if not assembler or not objcopy or not objdump: raise SystemExit("GNU as, objcopy, and objdump are required")
    elf_path.parent.mkdir(parents=True, exist_ok=True); c_path.parent.mkdir(parents=True, exist_ok=True)
    build_dir=source.parent/".build"; build_dir.mkdir(parents=True,exist_ok=True)
    obj, raw = build_dir/"hello.o", build_dir/"hello.bin"
    run([assembler, "--64", "-o", str(obj), str(source)])
    if "i386:x86-64" not in run([objdump,"-f",str(obj)]).stdout: raise SystemExit("assembler output is not x86-64")
    if "RELOCATION RECORDS FOR" in run([objdump,"-r","-j",".text",str(obj)]).stdout: raise SystemExit("unresolved .text relocation")
    run([objcopy,"-O","binary","-j",".text",str(obj),str(raw)])
    code=raw.read_bytes()
    if not code or MESSAGE not in code: raise SystemExit("empty code or missing expected syscall message")
    ident=b"\x7fELF"+bytes([2,1,1,0])+bytes(8)
    header=struct.pack("<16sHHIQQQIHHHHHH",ident,2,62,1,USER_BASE,64,0,0,64,56,1,0,0,0)
    ph=struct.pack("<IIQQQQQQ",1,5,PAGE,USER_BASE,USER_BASE,len(code),len(code),PAGE)
    image=header+ph+bytes(PAGE-len(header)-len(ph))+code
    elf_path.write_bytes(image)
    lines=["#include <stddef.h>","#include <stdint.h>","const uint8_t asterion_hello_elf[] = {"]
    for i in range(0,len(image),12): lines.append("    "+", ".join(f"0x{x:02X}" for x in image[i:i+12]) + ",")
    lines += ["};","const size_t asterion_hello_elf_size = sizeof(asterion_hello_elf);",""]
    c_path.write_text("\n".join(lines),encoding="ascii")
    c_path.with_suffix(".h").write_text("#ifndef ASTERION_HELLO_ELF_BLOB_H\n#define ASTERION_HELLO_ELF_BLOB_H\n#include <stddef.h>\n#include <stdint.h>\nextern const uint8_t asterion_hello_elf[];\nextern const size_t asterion_hello_elf_size;\n#endif\n",encoding="ascii")
    print(f"Built x86-64 ELF64 ET_EXEC: {elf_path} ({len(image)} bytes)")
if __name__=="__main__":
    p=argparse.ArgumentParser(); p.add_argument("--source",type=Path); p.add_argument("--elf",type=Path); p.add_argument("--c-output",type=Path); a=p.parse_args()
    here=Path(__file__).resolve().parent
    build(a.source or here/"hello.S",a.elf or here/"hello.elf",a.c_output or here.parent/"kernel"/"hello_elf_blob.c")
