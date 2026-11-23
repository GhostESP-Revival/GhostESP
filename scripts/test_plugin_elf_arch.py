"""Verify the production ELF architecture check with GCC and Python on PATH."""
from pathlib import Path
import struct
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[1]


def main():
    source = (REPO / "main/managers/plugin_manager.c").read_text(encoding="utf-8")
    start = source.index("static bool app_binary_matches_arch(")
    end = source.index("\n}\n", start) + 3
    helper = source[start:end]
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        for name, machine, elf_class, endian in (
            ("riscv", 243, 1, 1), ("xtensa", 94, 1, 1),
            ("elf64", 243, 2, 1), ("bigendian", 243, 1, 2),
            ("unknown", 0, 1, 1),
        ):
            header = bytearray(52)
            header[:7] = b"\x7fELF" + bytes((elf_class, endian, 1))
            struct.pack_into("<H", header, 18, machine)
            (work / name).write_bytes(header)
        (work / "short").write_bytes(b"\x7fELF")
        (work / "invalid").write_bytes(bytes(52))
        for arch, accepted, rejected in (("RISCV", "riscv", "xtensa"),
                                        ("XTENSA", "xtensa", "riscv")):
            harness = work / "check.c"
            harness.write_text(
                "#include <stdbool.h>\n#include <stdio.h>\n#include <string.h>\n"
                + helper + "\nint main(int argc, char **argv) {\n"
                + " if (argc != 9 || !app_binary_matches_arch(argv[1])) return 1;\n"
                + " for (int i = 2; i < argc; ++i) if (app_binary_matches_arch(argv[i])) return 2;\n"
                + " return 0;\n}\n", encoding="utf-8")
            exe = work / "check.exe"
            subprocess.run(["gcc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            f"-DCONFIG_IDF_TARGET_ARCH_{arch}=1", str(harness),
                            "-o", str(exe)], check=True)
            subprocess.run([str(exe)] + [str(work / name) for name in
                           (accepted, rejected, "elf64", "bigendian", "unknown",
                            "short", "invalid", "missing")], check=True)
    print("ELF architecture: RISC-V/Xtensa compatibility, invalid headers and missing files passed")


if __name__ == "__main__":
    main()
