import json
import os
import pathlib
import platform
import shutil
import subprocess
import sys


IDF_VERSION = "v6.0.1"
IDF_REPO_URL = "https://github.com/espressif/esp-idf.git"

GHOSTBT_HOME = pathlib.Path.home() / ".ghostbt"
CONFIG_PATH = GHOSTBT_HOME / "config.json"

TARGETS = ["esp32", "esp32s2", "esp32s3", "esp32c3", "esp32c6"]


def _load_config() -> dict:
    if CONFIG_PATH.is_file():
        return json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
    return {}


def _save_config(cfg: dict) -> None:
    GHOSTBT_HOME.mkdir(parents=True, exist_ok=True)
    CONFIG_PATH.write_text(json.dumps(cfg, indent=2) + "\n", encoding="utf-8")


def get_idf_path() -> pathlib.Path | None:
    cfg = _load_config()
    idf = cfg.get("idf_path")
    if idf:
        p = pathlib.Path(idf)
        if (p / "tools" / "idf.py").exists():
            return p
    env = os.environ.get("IDF_PATH")
    if env:
        p = pathlib.Path(env)
        if (p / "tools" / "idf.py").exists():
            return p
    return None


def get_idf_py() -> str | None:
    which = shutil.which("idf.py")
    if which:
        return which
    idf = get_idf_path()
    if idf:
        return str(idf / "tools" / "idf.py")
    return None


def _is_idf_installed(idf_dir: pathlib.Path) -> bool:
    return (idf_dir / "tools" / "idf.py").is_file()


def _detect_system_idf() -> pathlib.Path | None:
    which = shutil.which("idf.py")
    if which:
        resolved = pathlib.Path(which).resolve()
        for parent in resolved.parents:
            if (parent / "tools" / "idf.py").exists():
                return parent
    env = os.environ.get("IDF_PATH")
    if env and pathlib.Path(env, "tools", "idf.py").is_file():
        return pathlib.Path(env)
    return None


def setup_idf(
    targets: list[str] = None,
    idf_version: str = None,
    install_dir: str = None,
) -> pathlib.Path:
    existing = _detect_system_idf()
    if existing:
        print(f"ESP-IDF already found at: {existing}")
        cfg = _load_config()
        cfg["idf_path"] = str(existing)
        cfg["idf_version"] = idf_version or IDF_VERSION
        _save_config(cfg)
        print(f"Saved to {CONFIG_PATH}")
        return existing

    version = idf_version or IDF_VERSION
    targets = targets or ["esp32s3"]
    for t in targets:
        if t not in TARGETS:
            print(f"Unknown target: {t}. Supported: {', '.join(TARGETS)}", file=sys.stderr)
            raise SystemExit(2)

    if install_dir:
        idf_dir = pathlib.Path(install_dir).resolve()
    else:
        idf_dir = GHOSTBT_HOME / "esp-idf"

    if _is_idf_installed(idf_dir):
        print(f"ESP-IDF already installed at: {idf_dir}")
        cfg = _load_config()
        cfg["idf_path"] = str(idf_dir)
        cfg["idf_version"] = version
        _save_config(cfg)
        return idf_dir

    print(f"ESP-IDF not found. Installing {version} to {idf_dir}")
    print(f"Targets: {', '.join(targets)}")
    print(f"This will download ~2-4 GB. It may take several minutes.\n")

    git = shutil.which("git")
    if not git:
        print("git is required but not found on PATH.", file=sys.stderr)
        print("Install git: https://git-scm.com/downloads", file=sys.stderr)
        raise SystemExit(2)

    if idf_dir.exists():
        print(f"Removing incomplete install at {idf_dir}...")
        shutil.rmtree(idf_dir)

    print(f"[1/3] Cloning ESP-IDF {version}...")
    idf_dir.parent.mkdir(parents=True, exist_ok=True)
    subprocess.check_call([
        git, "clone",
        "--branch", version,
        "--depth", "1",
        "--recursive",
        "--shallow-submodules",
        IDF_REPO_URL,
        str(idf_dir),
    ])

    print(f"\n[2/3] Installing ESP-IDF tools for {', '.join(targets)}...")
    is_windows = platform.system() == "Windows"
    if is_windows:
        install_script = idf_dir / "install.bat"
        export_script = idf_dir / "export.bat"
    else:
        install_script = idf_dir / "install.sh"
        export_script = idf_dir / "export.sh"

    if not install_script.exists():
        print(f"Install script not found: {install_script}", file=sys.stderr)
        raise SystemExit(2)

    env = os.environ.copy()
    env["IDF_PATH"] = str(idf_dir)

    subprocess.check_call(
        [str(install_script)] + targets,
        cwd=str(idf_dir),
        env=env,
    )

    print(f"\n[3/3] Saving configuration...")
    cfg = _load_config()
    cfg["idf_path"] = str(idf_dir)
    cfg["idf_version"] = version
    cfg["targets"] = targets
    _save_config(cfg)

    print(f"\nESP-IDF {version} installed successfully!")
    print(f"  Location: {idf_dir}")
    print(f"  Config:   {CONFIG_PATH}")
    if is_windows:
        print(f"\n  To activate in a new terminal, run:")
        print(f"    {export_script}")
    else:
        print(f"\n  To activate in a new terminal, run:")
        print(f"    . {export_script}")
    print(f"\n  gbt will use this installation automatically.")

    return idf_dir


def _get_idf_version(idf_path: pathlib.Path) -> str:
    version_file = idf_path / "version.txt"
    if version_file.is_file():
        ver = version_file.read_text().strip()
    else:
        ver = ""
    if not ver:
        git = shutil.which("git")
        if git:
            try:
                result = subprocess.run(
                    [git, "describe", "--tags"],
                    cwd=str(idf_path),
                    capture_output=True, text=True, timeout=10,
                )
                if result.returncode == 0:
                    ver = result.stdout.strip()
            except (subprocess.TimeoutExpired, FileNotFoundError):
                pass
    if not ver:
        ver = "6.0.1"
    return ver.lstrip("v")


def _find_idf_python(idf_path: pathlib.Path) -> tuple:
    espRESSIF = pathlib.Path.home() / ".espressif"
    python_envs = espRESSIF / "python_env"
    if not python_envs.is_dir():
        return None, None
    idf_major = "6"
    version_file = idf_path / "version.txt"
    if version_file.is_file():
        ver = version_file.read_text().strip()
        parts = ver.split(".")
        if parts:
            idf_major = parts[0]
    for env_dir in sorted(python_envs.iterdir(), reverse=True):
        if not env_dir.is_dir():
            continue
        name = env_dir.name.lower()
        if f"idf{idf_major}" in name or f"idf{idf_major}." in name:
            if platform.system() == "Windows":
                python = env_dir / "Scripts" / "python.exe"
            else:
                python = env_dir / "bin" / "python"
            if python.exists():
                return str(python), str(env_dir)
    return None, None


def _build_idf_env(idf_path: pathlib.Path) -> dict:
    env = os.environ.copy()
    env["IDF_PATH"] = str(idf_path)
    env["ESP_IDF_VERSION"] = _get_idf_version(idf_path)
    _, idf_python_env = _find_idf_python(idf_path)
    if idf_python_env:
        env["IDF_PYTHON_ENV_PATH"] = idf_python_env

    espRESSIF = pathlib.Path.home() / ".espressif"
    tools_dir = espRESSIF / "tools"
    extra_paths = []
    if tools_dir.is_dir():
        for tool_dir in tools_dir.iterdir():
            if not tool_dir.is_dir():
                continue
            for version_dir in tool_dir.iterdir():
                if not version_dir.is_dir():
                    continue
                for candidate in (
                    version_dir / "bin",
                    version_dir / "toolchain" / "bin",
                    version_dir,
                ):
                    if candidate.is_dir() and str(candidate) not in extra_paths:
                        extra_paths.append(str(candidate))
                for subdir in version_dir.iterdir():
                    bin_dir = subdir / "bin"
                    if subdir.is_dir() and bin_dir.is_dir() and str(bin_dir) not in extra_paths:
                        extra_paths.append(str(bin_dir))
    if extra_paths:
        existing = env.get("PATH", "")
        env["PATH"] = os.pathsep.join(extra_paths + [existing]) if existing else os.pathsep.join(extra_paths)

    return env


def idf_command(*args: str) -> tuple:
    idf_py = get_idf_py()
    if not idf_py:
        print(
            "ESP-IDF not found. Run 'gbt setup' to install it automatically,\n"
            "or set IDF_PATH / add idf.py to your PATH.",
            file=sys.stderr,
        )
        raise SystemExit(1)

    idf_path = get_idf_path()
    if idf_path:
        env = _build_idf_env(idf_path)
        idf_python, _ = _find_idf_python(idf_path)
        if idf_python:
            return [idf_python, idf_py, *args], env
    else:
        env = os.environ.copy()

    if pathlib.Path(idf_py).suffix.lower() == ".py":
        return [sys.executable, idf_py, *args], env
    return [idf_py, *args], env


def get_idf_env() -> dict | None:
    idf_path = get_idf_path()
    if not idf_path:
        return None
    return _build_idf_env(idf_path)
