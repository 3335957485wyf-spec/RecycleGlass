"""Launcher: `python run.py`"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from recycle_glass.app import main

if __name__ == "__main__":
    raise SystemExit(main())
