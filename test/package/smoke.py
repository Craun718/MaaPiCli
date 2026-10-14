"""Check the shipped runtime and launch the CLI from an isolated package copy."""

import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


def runtime_libraries(binary_dir):
    patterns = {
        "win32": (r"onnxruntime\.dll", r"(?:lib)?webgpu_dawn\.dll"),
        "darwin": (
            r"libonnxruntime(?:\.[0-9]+)*\.dylib",
            r"libwebgpu_dawn(?:\.[0-9]+)*\.dylib",
        ),
    }.get(sys.platform, (
        r"libonnxruntime\.so(?:\.[0-9]+)*",
        r"libwebgpu_dawn\.so(?:\.[0-9]+)*",
    ))
    libraries = []
    for library, pattern in zip(("onnxruntime", "webgpu_dawn"), patterns):
        matches = sorted(
            path for path in binary_dir.iterdir()
            if path.is_file() and re.fullmatch(pattern, path.name)
        )
        if not matches:
            raise RuntimeError(f"Package is missing the {library} runtime")
        libraries.append(matches[0])
    return libraries


def main():
    package = Path(sys.argv[1]).resolve()
    runtime_libraries(package / "bin")

    fixture = Path(__file__).resolve().parents[1] / "cli/fixtures/eof/interface.json"
    with tempfile.TemporaryDirectory(prefix="maapicli-package-") as directory:
        binary_dir = Path(directory) / "bin"
        shutil.copytree(package / "bin", binary_dir, symlinks=True)
        shutil.copyfile(fixture, binary_dir / "interface.json")

        # Do not let build/development prefixes hide missing packaged libraries.
        environment = os.environ.copy()
        for variable in (
            "LD_LIBRARY_PATH",
            "LD_PRELOAD",
            "DYLD_LIBRARY_PATH",
            "DYLD_FALLBACK_LIBRARY_PATH",
            "DYLD_INSERT_LIBRARIES",
        ):
            environment.pop(variable, None)
        if sys.platform == "win32":
            windows = Path(os.environ["SystemRoot"])
            environment["PATH"] = os.pathsep.join(
                str(path) for path in (binary_dir, windows / "System32", windows)
            )

        executable = "MaaPiCli.exe" if sys.platform == "win32" else "MaaPiCli"
        result = subprocess.run(
            [str(binary_dir / executable)],
            cwd=directory,
            env=environment,
            input="",
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=30,
        )
        print(result.stdout)
        if result.returncode != 0 or "### Select controller ###" not in result.stdout:
            raise RuntimeError(f"Packaged CLI startup failed: exit {result.returncode}")


if __name__ == "__main__":
    main()
