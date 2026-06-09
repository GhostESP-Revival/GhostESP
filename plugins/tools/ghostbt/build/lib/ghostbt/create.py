import pathlib
import shutil
import sys

from .config import resolve_template, resolve_sdk_path


def _render_file(path: pathlib.Path, values: dict) -> None:
    text = path.read_text(encoding="utf-8")
    for key, value in values.items():
        text = text.replace("{{" + key + "}}", value)
    path.write_text(text, encoding="utf-8")


def create_app(
    app_id: str,
    name: str = None,
    template: str = "basic_app",
    out_dir: str = ".",
) -> pathlib.Path:
    if not app_id.replace("_", "").replace("-", "").isalnum():
        print("app_id may only contain letters, numbers, _ and -", file=sys.stderr)
        raise SystemExit(2)

    src = resolve_template(template)
    out_parent = pathlib.Path(out_dir).resolve()
    dst = out_parent / app_id
    if dst.exists():
        print(f"destination already exists: {dst}", file=sys.stderr)
        raise SystemExit(2)

    shutil.copytree(src, dst)

    symbol = app_id.replace("-", "_")
    display_name = name or app_id.replace("_", " ").replace("-", " ").title()

    try:
        sdk_path = resolve_sdk_path(start=dst)
        sdk_rel = pathlib.Path("..").joinpath(*pathlib.PurePosixPath(
            pathlib.Path(dst).relative_to(dst.anchor)
        ).parts[:0])
        count = len(dst.relative_to(dst.anchor).parts) - len(out_parent.relative_to(out_parent.anchor).parts)
    except FileNotFoundError:
        sdk_rel = pathlib.Path("../../../sdk")

    values = {
        "APP_ID": app_id,
        "APP_NAME": display_name,
        "APP_SYMBOL": symbol,
    }

    for path in sorted(dst.rglob("*"), key=lambda p: len(p.parts), reverse=True):
        if "{{APP_SYMBOL}}" in path.name:
            path = path.rename(path.with_name(path.name.replace("{{APP_SYMBOL}}", symbol)))
        if path.is_file() and (
            path.suffix in {".c", ".h", ".json", ".txt", ".cmake", ".yml"}
            or path.name == "CMakeLists.txt"
        ):
            _render_file(path, values)

    print(dst)
    return dst
