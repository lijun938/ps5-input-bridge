"""Download pinned, project-local native build tools and source dependencies."""
from pathlib import Path
import concurrent.futures, hashlib, shutil, subprocess, sys, tarfile, urllib.request, zipfile

ROOT = Path(__file__).resolve().parents[2]
DEPS = ROOT / 'native' / '.deps'
DEPS.mkdir(parents=True, exist_ok=True)
CHIAKI = ROOT / 'third_party' / 'chiaki' / 'source'
PIN = '0c4a45df0cae2af2ba2daef84e881850b07038a3'
SOURCES = [
 ('chiaki', f'https://codeload.github.com/chiaki-ng/chiaki-ng/tar.gz/{PIN}', CHIAKI),
 ('nanopb', 'https://codeload.github.com/nanopb/nanopb/tar.gz/cad3c18ef15a663e30e3e43e3a752b66378adec1', CHIAKI/'third-party/nanopb'),
 ('jerasure', 'https://codeload.github.com/streetpea/jerasure/tar.gz/505ccb4bc69eee8d7ea40a6089a056b99671134f', CHIAKI/'third-party/jerasure'),
 ('gf-complete', 'https://codeload.github.com/streetpea/gf-complete/tar.gz/fa54a4670a5705c84abf6c24b92b0cd479625478', CHIAKI/'third-party/gf-complete'),
 ('curl', 'https://codeload.github.com/curl/curl/tar.gz/b1ef0e1a01c0bb6ee5367bd9c186a603bde3615a', CHIAKI/'third-party/curl'),
 ('mbedtls', 'https://codeload.github.com/Mbed-TLS/mbedtls/tar.gz/8b3f26a5ac38d4fdccbc5c5366229f3e01dafcc0', DEPS/'mbedtls'),
 ('json-c', 'https://codeload.github.com/json-c/json-c/tar.gz/refs/tags/json-c-0.17-20230812', DEPS/'json-c'),
 ('miniupnp', 'https://codeload.github.com/miniupnp/miniupnp/tar.gz/refs/tags/miniupnpc_2_2_8', DEPS/'miniupnp'),
 ('libevent', 'https://codeload.github.com/libevent/libevent/tar.gz/refs/tags/release-2.1.12-stable', DEPS/'libevent'),
]

def download(url, dest, sha=None):
    if dest.exists() and sha and hashlib.sha256(dest.read_bytes()).hexdigest() != sha:
        dest.unlink()
    if not dest.exists():
        print(f'Downloading {dest.name}', flush=True)
        partial = dest.with_name(dest.name+'.part')
        try:
            with urllib.request.urlopen(url, timeout=120) as src, partial.open('wb') as out:
                expected_length = src.headers.get('Content-Length')
                shutil.copyfileobj(src, out)
            if expected_length is not None and partial.stat().st_size != int(expected_length):
                raise RuntimeError(f'Truncated download: {dest.name}')
            if sha and hashlib.sha256(partial.read_bytes()).hexdigest() != sha:
                raise RuntimeError(f'Checksum mismatch: {dest.name}')
            partial.replace(dest)
        except BaseException:
            partial.unlink(missing_ok=True)
            raise
    return dest

def source(item):
    name, url, dest = item
    if (dest/'.bridge-source-ready').exists(): return
    archive = download(url, DEPS/f'{name}.tar.gz')
    stage = DEPS/f'extract-{name}'
    stage.mkdir(exist_ok=True)
    with tarfile.open(archive) as arc:
        arc.extractall(stage, filter='data')
    children = list(stage.iterdir())
    if len(children) != 1: raise RuntimeError(f'Unexpected archive layout: {name}')
    dest.mkdir(parents=True, exist_ok=True)
    shutil.copytree(children[0], dest, dirs_exist_ok=True)
    (dest/'.bridge-source-ready').write_text(url, encoding='utf-8')
    shutil.rmtree(stage)

PATCHES = ROOT / 'native' / 'patches' / 'chiaki'

def apply_patches():
    # Whole-file replacements of pinned upstream sources; reapplied every run.
    for patched in PATCHES.rglob('*'):
        if patched.is_file():
            target = CHIAKI / patched.relative_to(PATCHES)
            if not target.exists(): raise RuntimeError(f'Patch target missing: {target}')
            shutil.copyfile(patched, target)

def toolchain():
    target = DEPS/'llvm-mingw-20261006-ucrt-x86_64'
    if (target/'bin/clang.exe').exists(): return
    archive = download('https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/llvm-mingw-20261006-ucrt-x86_64.zip', DEPS/'llvm-mingw.zip', '317492c456aa27ee607a5919f1d2d38dcdc1112516a24d0bf4b00d078f52d17a')
    with zipfile.ZipFile(archive) as arc: arc.extractall(DEPS)

def protoc():
    target = DEPS/'protoc'
    if (target/'bin/protoc.exe').exists(): return
    archive = download('https://github.com/protocolbuffers/protobuf/releases/download/v25.3/protoc-25.3-win64.zip', DEPS/'protoc.zip')
    with zipfile.ZipFile(archive) as arc: arc.extractall(target)

def python_tools():
    venv = DEPS/'python'
    exe = venv/'Scripts/python.exe'
    if not exe.exists(): subprocess.check_call([sys.executable, '-m', 'venv', str(venv)])
    if not (venv/'Scripts/cmake.exe').exists():
        subprocess.check_call([str(exe), '-m', 'pip', 'install', '--disable-pip-version-check', 'cmake==3.31.6', 'ninja==1.11.1.4', 'protobuf==4.25.6'])

if __name__ == '__main__':
    # Root must precede its submodules to avoid races copying the archive.
    source(SOURCES[0])
    apply_patches()
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
        tasks = [pool.submit(source, item) for item in SOURCES[1:]]
        tasks += [pool.submit(toolchain), pool.submit(protoc), pool.submit(python_tools)]
        for task in tasks: task.result()
    prefix = DEPS/'prefix'
    shutil.copytree(DEPS/'json-c', prefix/'json-c', dirs_exist_ok=True)
    shutil.copytree(DEPS/'miniupnp/miniupnpc/include', prefix/'miniupnpc', dirs_exist_ok=True)
    print('Native dependencies ready.', flush=True)
