"""bbcoop (packaging/windows/package-cross.sh): every function a package's executables and DLLs import from a
DLL the package ships must be exported by that DLL. Windows refuses to start a program otherwise
(0xC0000139, "entry point not found"); Wine only stubs the import, so its checks miss it. The toolchain's
libc++.dll lacked _Unwind_Resume, which MSYS2's DLLs import from it (2026-10-26).
Usage: check_imports.py <llvm-readobj> <package folder>; prints each missing import, exit 1 if any."""
import re
import subprocess
import sys
from pathlib import Path

readobj, folder = sys.argv[1], Path(sys.argv[2])


def read(option, path):
    return subprocess.run([readobj, option, str(path)], capture_output=True, text=True, check=True).stdout


files = [p for p in folder.rglob('*') if p.suffix.lower() in ('.exe', '.dll') and 'python' not in p.relative_to(folder).parts]
exports = {}  # per folder: Windows takes a DLL from the importing program's own folder
for path in files:
    if path.suffix.lower() == '.dll':
        exports[(path.parent, path.name.lower())] = set(re.findall(r'^\s*Name: (\S+)', read('--coff-exports', path), re.M))
missing = 0
for path in files:
    for block in re.split(r'\n(?=Import \{)', read('--coff-imports', path)):
        name = re.search(r'Name: (\S+)', block)
        # A DLL's own folder, else the program's (bin/shadnet/sqldrivers/qsqlite.dll loads from bin/shadnet).
        names = name and (exports.get((path.parent, name.group(1).lower())) or
                          exports.get((path.parent.parent, name.group(1).lower())))
        if names is None:
            continue  # Windows' own
        for symbol in re.findall(r'Symbol: (\S+)', block):
            if symbol not in names:
                print(f'{path.relative_to(folder)}: {name.group(1)}!{symbol} is missing')
                missing += 1
sys.exit(1 if missing else 0)
