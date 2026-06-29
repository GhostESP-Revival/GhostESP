#!/usr/bin/env python3
import argparse
import pathlib
import shutil
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
TEMPLATE_ROOT = ROOT / "templates"


def render_file(path: pathlib.Path, values: dict[str, str]) -> None:
    text = path.read_text(encoding="utf-8")
    for key, value in values.items():
        text = text.replace("{{" + key + "}}", value)
    path.write_text(text, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="Create a GhostESP native SD app project")
    parser.add_argument("app_id", help="Safe app id, for example my_tool")
    parser.add_argument("--name", help="Display name")
    parser.add_argument("--template", default="basic_app", help="Template under plugins/templates")
    parser.add_argument("--out", default="plugins/examples", help="Output parent directory")
    args = parser.parse_args()

    if not args.app_id.replace("_", "").replace("-", "").isalnum():
        print("app_id may only contain letters, numbers, _ and -", file=sys.stderr)
        return 2

    src = TEMPLATE_ROOT / args.template
    if not src.is_dir():
        print(f"template not found: {src}", file=sys.stderr)
        return 2

    out_parent = pathlib.Path(args.out)
    dst = out_parent / args.app_id
    if dst.exists():
        print(f"destination already exists: {dst}", file=sys.stderr)
        return 2

    shutil.copytree(src, dst)
    values = {
        "APP_ID": args.app_id,
        "APP_NAME": args.name or args.app_id.replace("_", " ").replace("-", " ").title(),
        "APP_SYMBOL": args.app_id.replace("-", "_"),
    }
    for path in sorted(dst.rglob("*"), key=lambda p: len(p.parts), reverse=True):
        if "{{APP_SYMBOL}}" in path.name:
            path = path.rename(path.with_name(path.name.replace("{{APP_SYMBOL}}", values["APP_SYMBOL"])))
        if path.is_file() and (path.suffix in {".c", ".h", ".json", ".txt", ".cmake", ".yml"} or path.name == "CMakeLists.txt"):
            render_file(path, values)
    print(dst)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
