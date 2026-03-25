"""Create a local virtual environment using Bazel's configured Python toolchain."""

from __future__ import annotations

import argparse
import logging
import os
import shutil
import subprocess
import sys
from pathlib import Path

LOGGER = logging.getLogger(__name__)


def parse_args() -> argparse.Namespace:
    """Parse CLI arguments for virtual environment creation."""
    parser = argparse.ArgumentParser(
        description="Create a Python virtual environment from this Bazel workspace's requirements.txt."
    )
    parser.add_argument(
        "path",
        nargs="?",
        default=".venv",
        help="Destination for the virtual environment. Relative paths are resolved from the workspace root.",
    )
    parser.add_argument(
        "--recreate",
        action="store_true",
        help="Delete any existing virtual environment at the target path before creating a new one.",
    )
    return parser.parse_args()


def workspace_root() -> Path:
    """Return the Bazel workspace root for this invocation."""
    root = os.environ.get("BUILD_WORKSPACE_DIRECTORY")
    if root:
        return Path(root)
    return Path.cwd()


def resolve_venv_path(raw_path: str, root: Path) -> Path:
    """Resolve a user-provided venv path relative to the workspace root."""
    path = Path(raw_path)
    if path.is_absolute():
        return path
    return root / path


def main() -> int:
    """Create the virtual environment and install the workspace requirements."""
    args = parse_args()
    root = workspace_root()
    venv_path = resolve_venv_path(args.path, root)
    requirements_path = root / "requirements.txt"

    if args.recreate and venv_path.exists():
        shutil.rmtree(venv_path)

    if not requirements_path.exists():
        message = f"Could not find requirements.txt at {requirements_path}"
        raise FileNotFoundError(message)

    subprocess.check_call([sys.executable, "-m", "venv", str(venv_path)])  # noqa: S603

    venv_python = venv_path / "bin" / "python"
    subprocess.check_call([str(venv_python), "-m", "pip", "install", "--upgrade", "pip"])  # noqa: S603
    subprocess.check_call([str(venv_python), "-m", "pip", "install", "-r", str(requirements_path)])  # noqa: S603

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    LOGGER.info("Created virtual environment at %s", venv_path)
    LOGGER.info("Activate it with: source %s", venv_path / "bin" / "activate")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
