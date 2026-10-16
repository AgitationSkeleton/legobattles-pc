#!/usr/bin/env bash
# Build the two master-server executables: a console build that shows and
# keeps logs, and a GUI build
# with a lobby sidepanel that expands to the players in each lobby.
#
#   ./build_exe.sh
#
# Output lands in dist/. Both dist/ and build/ are gitignored - they are
# build products, not source.
set -eu
cd "$(dirname "${BASH_SOURCE[0]}")"

command -v python >/dev/null || { echo "python not on PATH"; exit 1; }
python -c "import PyInstaller" 2>/dev/null || { echo "PyInstaller not installed"; exit 1; }

echo "== tests =="
python test_master_server.py | tail -2

common=(--onefile --noconfirm --clean --distpath dist --workpath build --specpath build)

echo
echo "== console build =="
python -m PyInstaller "${common[@]}" --console \
    --name lb_master_server master_server.py 2>&1 | tail -3

echo
echo "== gui build =="
# master_server.py is imported by the GUI; PyInstaller follows it from the
# same directory. tkinter ships with CPython on Windows.
python -m PyInstaller "${common[@]}" --windowed \
    --name lb_master_server_gui master_server_gui.py 2>&1 | tail -3

echo
ls -la dist/
