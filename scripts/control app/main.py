import sys
import os
import subprocess

VENV_DIR = "ghost-control-venv"
REQ_FILE = "requirements.txt"

def in_venv():
    return (
        hasattr(sys, 'real_prefix') or
        (hasattr(sys, 'base_prefix') and sys.base_prefix != sys.prefix)
    )

def create_venv():
    print("Creating virtual environment...")
    subprocess.check_call([sys.executable, "-m", "venv", VENV_DIR])

def run_in_venv():
    # Re-run this script using the venv's Python
    if os.name == "nt":
        python_bin = os.path.join(VENV_DIR, "Scripts", "python.exe")
        print(f"Re-running in venv: {python_bin}")
        result = subprocess.run([python_bin] + sys.argv)
        sys.exit(result.returncode)
    else:
        python_bin = os.path.join(VENV_DIR, "bin", "python")
        print(f"Re-running in venv: {python_bin}")
        os.execv(python_bin, [python_bin] + sys.argv)

def install_requirements():
    print(f"Installing dependencies from {REQ_FILE}...")
    subprocess.check_call([sys.executable, "-m", "pip", "install", "-r", REQ_FILE])

if not in_venv():
    if not os.path.isdir(VENV_DIR):
        create_venv()
    run_in_venv()

# Now in venv, ensure dependencies from requirements.txt
if os.path.exists(REQ_FILE):
    install_requirements()
else:
    print(f"Warning: {REQ_FILE} not found. No dependencies installed.")

from PyQt6.QtWidgets import QApplication
from gui import ESP32ControlGUI

if __name__ == "__main__":
    app = QApplication(sys.argv)
    window = ESP32ControlGUI()
    window.show()
    sys.exit(app.exec())