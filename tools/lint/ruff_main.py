"""Wrapper script to run ruff via Bazel's python toolchain."""

import os
import subprocess
import sys


def main() -> None:
    """Run the ruff CLI."""
    env = os.environ.copy()
    env["PYTHONPATH"] = os.pathsep.join(sys.path)

    # Use sys.executable to run ruff as a module, passing all arguments
    # check=False is used because we manually propagate the return code
    result = subprocess.run([sys.executable, "-m", "ruff", *sys.argv[1:]], env=env, check=False)  # noqa: S603
    sys.exit(result.returncode)


if __name__ == "__main__":
    main()
