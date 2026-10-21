"""bbcoop (scripts/windows/cross-build.sh): resolves MSYS2 CLANG64 packages with their dependencies, downloads
and unpacks them into a sysroot. Usage: msys2_fetch.py <package dir with clang64.db> <sysroot> <package>..."""
import io, re, subprocess, sys, tarfile, urllib.request
from pathlib import Path

pkgs, sysroot = Path(sys.argv[1]), Path(sys.argv[2])
wanted = sys.argv[3:]
P = 'mingw-w64-clang-x86_64-'
raw = subprocess.run(['zstd', '-dc', str(pkgs / 'clang64.db')], capture_output=True, check=True).stdout
db, provides = {}, {}
with tarfile.open(fileobj=io.BytesIO(raw)) as tar:
    for member in tar:
        if member.name.endswith('/desc'):
            text = tar.extractfile(member).read().decode()
            fields = {m.group(1): m.group(2).split('\n') for m in re.finditer(r'%(\w+)%\n(.*?)(?:\n\n|\Z)', text, re.S)}
            name = fields['NAME'][0]
            db[name] = fields
            for p in fields.get('PROVIDES', []):
                provides[re.split('[<>=]', p)[0]] = name
# The cross toolchain (llvm-mingw) brings its own C runtime, headers and C++ library.
SKIP = re.compile(P + r'(crt|headers|libc\+\+|libunwind|libwinpthread|winpthreads|compiler-rt|clang|llvm|lld|gcc|python|cmake|ninja|pkgconf|tcl|tk|ncurses|readline|gettext|libiconv-?$)')
order, seen = [], set()
def add(dep):
    name = re.split('[<>=]', dep)[0]
    name = name if name in db else provides.get(name, name)
    if name in seen or SKIP.match(name):
        return
    if name not in db:
        print('missing', name); return
    seen.add(name)
    for d in db[name].get('DEPENDS', []):
        add(d)
    order.append(name)
for w in wanted:
    add(P + w)
print(len(order), 'packages')
sysroot.mkdir(parents=True, exist_ok=True)
for name in order:
    filename = db[name]['FILENAME'][0]
    target = pkgs / filename
    if not target.exists():
        urllib.request.urlretrieve('https://repo.msys2.org/mingw/clang64/' + filename, target)
    data = subprocess.run(['zstd', '-dc', str(target)], capture_output=True, check=True).stdout
    with tarfile.open(fileobj=io.BytesIO(data)) as tar:
        members = [m for m in tar if m.name.startswith('clang64/') and (m.isfile() or m.isdir() or m.issym())]
        tar.extractall(sysroot, members=members, filter='data')

