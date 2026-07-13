"""GhostScript bytecode compiler for GhostBT.

Compiles .gs source files to pre-compiled .gsb bytecode using luac 5.4.7.
Uses a bundled binary if available, otherwise builds from source.
"""

import os
import pathlib
import platform
import shutil
import stat
import subprocess
import sys

_PACKAGE_DIR = pathlib.Path(__file__).resolve().parent
_LUAC_DIR = _PACKAGE_DIR / "data" / "luac"
_LUA_SRC = _PACKAGE_DIR / "data" / "lua-src"

_LUAC_NAMES = {
    ("Windows", "AMD64"):   "luac-windows-amd64.exe",
    ("Windows", "x86_64"):  "luac-windows-amd64.exe",
    ("Darwin", "x86_64"):   "luac-macos-x86_64",
    ("Darwin", "arm64"):    "luac-macos-arm64",
    ("Linux", "x86_64"):    "luac-linux-x86_64",
    ("Linux", "aarch64"):   "luac-linux-aarch64",
}


def _find_luac() -> str:
    """Find bundled luac, or build from source if no binary exists for this platform."""
    key = (platform.system(), platform.machine())
    name = _LUAC_NAMES.get(key)
    if name:
        bundled = _LUAC_DIR / name
        if bundled.exists():
            if platform.system() != "Windows":
                bundled.chmod(bundled.stat().st_mode | stat.S_IEXEC)
            return str(bundled)

    # No pre-built binary — try building from source
    built = _build_from_source()
    if built:
        return built

    raise FileNotFoundError(
        f"No luac for {key[0]} {key[1]}.\n"
        "Either:\n"
        "  1. Build and add a binary to ghostbt/data/luac/\n"
        "  2. Install Lua 5.4: brew install lua / apt install lua5.4"
    )


def _build_from_source() -> str:
    """Build luac from bundled Lua 5.4.7 source using the system C compiler."""
    if not _LUA_SRC.exists():
        return None

    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not cc:
        return None

    key = (platform.system(), platform.machine())
    name = _LUAC_NAMES.get(key, "luac")
    out = _LUAC_DIR / name

    srcs = [
        "lapi", "lcode", "lctype", "ldebug", "ldo", "ldump", "lfunc", "lgc",
        "llex", "lmem", "lobject", "lopcodes", "lparser", "lstate", "lstring",
        "ltable", "ltm", "lundump", "lvm", "lzio", "lauxlib", "luac",
    ]

    _LUAC_DIR.mkdir(parents=True, exist_ok=True)
    objs = []
    for s in srcs:
        c_file = _LUA_SRC / f"{s}.c"
        o_file = _LUAC_DIR / f"{s}.o"
        try:
            subprocess.check_call(
                [cc, "-O2", "-c", "-o", str(o_file), str(c_file)],
                stderr=subprocess.DEVNULL,
            )
            objs.append(str(o_file))
        except subprocess.CalledProcessError:
            return None

    try:
        subprocess.check_call(
            [cc, "-O2", "-o", str(out)] + objs + ["-lm"],
            stderr=subprocess.DEVNULL,
        )
    except subprocess.CalledProcessError:
        return None

    # Clean up .o files
    for o in objs:
        pathlib.Path(o).unlink(missing_ok=True)

    if out.exists():
        if platform.system() != "Windows":
            out.chmod(out.stat().st_mode | stat.S_IEXEC)
        print(f"Built luac from source: {out}")
        return str(out)
    return None


def compile_one(src: pathlib.Path, dst: pathlib.Path = None, luac: str = None) -> bool:
    if dst is None:
        dst = src.with_suffix(".gsb")
    luac = luac or _find_luac()
    try:
        subprocess.check_call([luac, "-o", str(dst), str(src)],
                              stderr=subprocess.STDOUT)
        return True
    except subprocess.CalledProcessError as e:
        print(f"  ERROR: {src.name}: {e}", file=sys.stderr)
        return False


def compile_dir(d: pathlib.Path, out: pathlib.Path = None, luac: str = None) -> tuple:
    files = sorted(d.glob("*.gs"))
    if not files:
        return 0, 0, []
    luac = luac or _find_luac()
    if out:
        out.mkdir(parents=True, exist_ok=True)
    ok = 0
    compiled = []
    for src in files:
        dst = (out / src.with_suffix(".gsb").name) if out else src.with_suffix(".gsb")
        src_size = src.stat().st_size
        if compile_one(src, dst, luac):
            dst_size = dst.stat().st_size
            pct = 100 * dst_size / src_size if src_size else 0
            print(f"  {src.name:<30s} {src_size:>6d} -> {dst_size:>6d} bytes ({pct:.0f}%)")
            ok += 1
            compiled.append(dst)
    return ok, len(files), compiled


def compile_scripts(
    path: str = ".",
    out: str = None,
    deploy: bool = False,
    deploy_dir: str = None,
) -> int:
    target = pathlib.Path(path).resolve()
    luac = _find_luac()

    if target.is_file():
        dst = pathlib.Path(out).resolve() if out else target.with_suffix(".gsb")
        src_size = target.stat().st_size
        if not compile_one(target, dst, luac):
            return 1
        dst_size = dst.stat().st_size
        pct = 100 * dst_size / src_size if src_size else 0
        print(f"{target.name}: {src_size} -> {dst_size} bytes ({pct:.0f}%)")
        if deploy:
            return 0 if _deploy(dst, deploy_dir) else 1
        return 0

    if target.is_dir():
        files = sorted(target.glob("*.gs"))
        if not files:
            print(f"No .gs files found in {target}", file=sys.stderr)
            return 1
        print(f"Compiling {len(files)} script(s)...\n")
        out_dir = pathlib.Path(out).resolve() if out else None
        ok, total, compiled = compile_dir(target, out_dir, luac)
        print(f"\n{ok}/{total} compiled successfully.")
        if deploy:
            deployed = True
            for dst in compiled:
                if not _deploy(dst, deploy_dir):
                    deployed = False
            if not deployed:
                return 1
        return 0 if ok == total else 1

    print(f"Not found: {target}", file=sys.stderr)
    return 1


def _deploy(src: pathlib.Path, deploy_dir: str = None) -> bool:
    sd_path = pathlib.Path(deploy_dir or "/mnt/ghostesp/scripts") / src.name
    try:
        shutil.copy2(str(src), str(sd_path))
        print(f"  -> {sd_path}")
        return True
    except OSError as e:
        print(f"  deploy failed: {e}", file=sys.stderr)
        return False
