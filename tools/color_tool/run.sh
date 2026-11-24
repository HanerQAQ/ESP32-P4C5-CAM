#!/usr/bin/env bash
# ColorTool Launcher for Linux & macOS
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

# Determine Python interpreter
if command -v python3 >/dev/null 2>&1; then
    PYTHON_CMD="python3"
elif command -v python >/dev/null 2>&1; then
    PYTHON_CMD="python"
else
    echo "[ERROR] Python 3 not found in PATH! Please install Python 3.9+."
    exit 1
fi

# Detect virtual environment if present
if [ -d ".venv" ] && [ -f ".venv/bin/activate" ]; then
    echo "[INFO] Activating virtual environment (.venv)..."
    source .venv/bin/activate
fi

echo "[INFO] Launching ColorTool v1.0..."
exec "${PYTHON_CMD}" main.py "$@"
