import sys
from pathlib import Path

# The tool's modules are plain scripts in the parent directory.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
