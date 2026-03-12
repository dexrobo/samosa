import subprocess
import sys

if __name__ == "__main__":
    # Use sys.executable to run ruff as a module, passing all arguments
    result = subprocess.run([sys.executable, "-m", "ruff"] + sys.argv[1:])
    sys.exit(result.returncode)
