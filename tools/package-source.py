"""Bundle application source, build instructions, licenses, and native source archives."""
from pathlib import Path
import zipfile

root = Path(__file__).resolve().parents[1]
output = root / "dist" / "PS5InputBridge-source.zip"
output.parent.mkdir(exist_ok=True)
paths = [root / name for name in (".gitignore", "LICENSE", "README.md", "THIRD-PARTY-NOTICES.md")]
for folder in ("src", "tests", "docs", "tools", "licenses"):
    for path in (root / folder).rglob("*"):
        if path.is_file() and not any(part in {"bin", "obj", "__pycache__"} for part in path.parts):
            paths.append(path)
for folder in ("include", "cmake", "src", "scripts", "tests", "patches"):
    paths.extend(path for path in (root / "native" / folder).rglob("*")
                 if path.is_file() and "__pycache__" not in path.parts)
paths.extend(root / "native" / name for name in (".gitignore", "README.md", "CMakeLists.txt"))
paths.extend((root / "native" / ".deps").glob("*.tar.gz"))
with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
    for path in sorted(set(paths)):
        archive.write(path, path.relative_to(root).as_posix())
print(f"Corresponding source: {output} ({output.stat().st_size:,} bytes)")
