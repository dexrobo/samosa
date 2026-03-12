import os
import subprocess
import sys

if __name__ == "__main__":
    # If run via 'bazel run', BUILD_WORKSPACE_DIRECTORY is set to the source root.
    # Jumping there avoids the '.runfiles' naming issues which confuse mypy.
    workspace = os.environ.get("BUILD_WORKSPACE_DIRECTORY")
    if workspace:
        os.chdir(workspace)

    # Use sys.executable -m mypy to run mypy as a module
    result = subprocess.run([sys.executable, "-m", "mypy"] + sys.argv[1:])
    sys.exit(result.returncode)
