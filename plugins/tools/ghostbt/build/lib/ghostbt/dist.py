import pathlib

from .build import build_app
from .package import package_app


def dist_app(
    app_dir: str = ".",
    target: str = None,
    out: str = None,
    make_gapp: bool = False,
) -> pathlib.Path:
    app_path = pathlib.Path(app_dir).resolve()
    build_app(app_dir=str(app_path), target=target, skip_set_target=False)
    return package_app(app_dir=str(app_path), out=out, make_gapp=make_gapp)
