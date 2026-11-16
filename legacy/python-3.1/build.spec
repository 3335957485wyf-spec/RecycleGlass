# -*- mode: python ; coding: utf-8 -*-
"""PyInstaller spec for RecycleGlass — single-file, windowed, custom icon."""
from pathlib import Path

ROOT = Path(SPECPATH)
ASSETS = ROOT / "recycle_glass" / "assets"

a = Analysis(
    ["run.py"],
    pathex=[str(ROOT)],
    binaries=[],
    datas=[(str(ASSETS), "recycle_glass/assets")],
    hiddenimports=[],   # no tkinter any more: settings live in the flyout
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[
        "matplotlib", "numpy",
        "unittest", "email", "html", "http", "xml",
        "pydoc", "doctest", "bz2", "lzma", "tkinter",
        "PIL.ImageQt", "PIL.ImageTk",
        "ctypes.macholib", "setuptools", "pkg_resources",
    ],
    noarchive=False,
)

pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name="RecycleGlass",
    version=str(ROOT / "version_info.txt"),
    debug=False,
    bootloader_ignore_signals=False,
    strip=True,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon=str(ASSETS / "app-icon.ico"),
)
