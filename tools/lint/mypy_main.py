"""Wrapper script to run mypy via Bazel's python toolchain."""

import os
import subprocess
import sys


def main() -> None:
    """Run the mypy CLI."""
    # If run via 'bazel run', BUILD_WORKSPACE_DIRECTORY is set to the source root.
    # Jumping there avoids the '.runfiles' naming issues which confuse mypy.
    workspace = os.environ.get("BUILD_WORKSPACE_DIRECTORY")
    if workspace:
        os.chdir(workspace)

    # Use sys.executable -m mypy to run mypy as a module
    # check=False is used because we manually propagate the return code
    result = subprocess.run([sys.executable, "-m", "mypy", *sys.argv[1:]], check=False)  # noqa: S603
    sys.exit(result.returncode)


if __name__ == "__main__":
    main()
