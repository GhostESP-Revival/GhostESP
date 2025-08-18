import os
import shutil
import zipfile
import tempfile
import urllib.request
import time
import sys
import subprocess
from PyQt6.QtWidgets import QMessageBox, QProgressDialog, QApplication

def find_esp_idf_gui(parent=None, auto_download=True):
    """
    Search for ESP-IDF in common locations, prompt user to download if not found.
    Returns the path or None.
    """
    # Check environment variable first
    if 'IDF_PATH' in os.environ:
        idf_path = os.environ['IDF_PATH']
        if os.path.exists(os.path.join(idf_path, "export.sh")) or os.path.exists(os.path.join(idf_path, "export.bat")):
            return idf_path

    # Common ESP-IDF installation paths
    search_paths = []
    
    if os.name == 'nt':  # Windows
        username = os.environ.get('USERNAME', '')
        # Get script directory for local ESP-IDF installations
        script_dir = os.path.dirname(os.path.abspath(__file__))
        search_paths = [
            r"C:\esp\esp-idf",
            rf"C:\Users\{username}\esp\esp-idf",
            r"C:\espressif\esp-idf",
            rf"C:\Users\{username}\espressif\esp-idf",
            r"C:\esp-idf",
            r"D:\esp\esp-idf",
            r"D:\espressif\esp-idf",
            r"C:\Program Files\esp-idf",
            r"C:\Program Files (x86)\esp-idf",
            r"C:\tools\esp-idf",
            # r"S:\Espressif\frameworks\esp-idf-v5.5",
            r"C:\esp\esp-idf-v5.5",
            r"C:\esp\esp-idf-v5.4.1",
            os.path.join(script_dir, "esp-idf-v5.5"),
            os.path.join(script_dir, "esp-idf-v5.4.1"),
            os.path.join(script_dir, "esp-idf")
        ]
    else:  # Unix-like systems
        home = os.path.expanduser("~")
        # Get script directory for local ESP-IDF installations
        script_dir = os.path.dirname(os.path.abspath(__file__))
        search_paths = [
            f"{home}/esp/esp-idf",
            f"{home}/.espressif/esp-idf",
            "/opt/esp-idf",
            "/usr/local/esp-idf",
            "/opt/espressif/esp-idf",
            f"{home}/esp/esp-idf-v5.5",
            f"{home}/esp/esp-idf-v5.4.1",
            f"{home}/esp/v5.5/esp-idf",
            f"{home}/esp/v5.4.1/esp-idf",
            os.path.join(script_dir, "esp-idf-v5.5"),
            os.path.join(script_dir, "esp-idf-v5.4.1"),
            os.path.join(script_dir, "esp-idf")
        ]
    
    # Search for ESP-IDF in common locations
    for path in search_paths:
        export_script = "export.bat" if os.name == 'nt' else "export.sh"
        if os.path.exists(os.path.join(path, export_script)):
            print(f"Found ESP-IDF at: {path}")
            return path

    # Not found, prompt user
    if auto_download and parent:
        ret = QMessageBox.question(
            parent,
            "ESP-IDF Not Found",
            "ESP-IDF was not found on your system. Would you like to download and install ESP-IDF v5.5 now?",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No
        )
        if ret == QMessageBox.StandardButton.Yes:
            return download_esp_idf_gui(parent, version="5.5")
    return None

def download_esp_idf_gui(parent=None, version="5.5"):
    """
    Download and extract ESP-IDF, show progress in a dialog.
    """
    urls = {
        "5.5": "https://github.com/espressif/esp-idf/releases/download/v5.5/esp-idf-v5.5.zip",
        "5.4.1": "https://github.com/espressif/esp-idf/releases/download/v5.4.1/esp-idf-v5.4.1.zip"
    }
    if version not in urls:
        QMessageBox.critical(parent, "Error", f"Unsupported ESP-IDF version {version}")
        return None

    script_dir = os.path.dirname(os.path.abspath(__file__))
    install_path = os.path.join(script_dir, f"esp-idf-v{version}")

    if os.path.exists(install_path):
        QMessageBox.information(parent, "ESP-IDF", f"ESP-IDF v{version} already exists at {install_path}")
        return install_path

    tmp_fd, tmp_path = tempfile.mkstemp(suffix='.zip', dir=script_dir)
    os.close(tmp_fd)
    try:
        url = urls[version]
        dlg = QProgressDialog(parent)
        dlg.setWindowTitle("Downloading ESP-IDF")
        dlg.setLabelText(f"Downloading ESP-IDF v{version}...")
        dlg.setRange(0, 100)
        dlg.setValue(0)
        dlg.setCancelButton(None)
        dlg.setMinimumDuration(0)
        dlg.show()
        QApplication.processEvents()

        max_retries = 3
        attempt = 0
        while True:
            try:
                existing = 0
                if os.path.exists(tmp_path):
                    existing = os.path.getsize(tmp_path)
                req = urllib.request.Request(url)
                req.add_header("User-Agent", "GhostESP-Downloader")
                if existing > 0:
                    req.add_header("Range", f"bytes={existing}-")
                with urllib.request.urlopen(req, timeout=120) as resp:
                    total_size_header = resp.getheader("Content-Length")
                    if total_size_header is not None:
                        total_size = int(total_size_header)
                        if existing > 0 and resp.getcode() == 206:
                            total_size = existing + total_size
                        dlg.setRange(0, total_size)
                    else:
                        total_size = None
                        dlg.setRange(0, 0)
                    mode = "ab" if existing > 0 else "wb"
                    downloaded = existing
                    chunk_size = 1024 * 1024
                    with open(tmp_path, mode) as f:
                        while True:
                            chunk = resp.read(chunk_size)
                            if not chunk:
                                break
                            f.write(chunk)
                            downloaded += len(chunk)
                            if total_size is not None:
                                dlg.setValue(downloaded)
                                percent = (downloaded * 100) // total_size if total_size else 0
                                dlg.setLabelText(f"Downloading ESP-IDF v{version}... {percent}% ({downloaded // 1024 // 1024} MB / {total_size // 1024 // 1024} MB)")
                            QApplication.processEvents()
                if total_size is not None and downloaded < total_size:
                    raise IOError(f"retrieval incomplete: got only {downloaded} out of {total_size} bytes")
                break
            except Exception:
                attempt += 1
                if attempt >= max_retries:
                    raise
                time.sleep(min(5 * attempt, 15))
                QApplication.processEvents()
        dlg.setLabelText("Extracting...")
        dlg.setRange(0, 0)
        dlg.setValue(0)
        QApplication.processEvents()

        with zipfile.ZipFile(tmp_path, 'r') as zip_ref:
            temp_extract_dir = os.path.join(script_dir, f"temp_extract_{version}")
            if os.path.exists(temp_extract_dir):
                shutil.rmtree(temp_extract_dir)
            os.makedirs(temp_extract_dir)
            members = zip_ref.infolist()
            total = len(members)
            if total > 0:
                dlg.setRange(0, total)
            done = 0
            for m in members:
                zip_ref.extract(m, temp_extract_dir)
                done += 1
                if total > 0:
                    dlg.setValue(done)
                QApplication.processEvents()
            extracted_items = os.listdir(temp_extract_dir)
            if len(extracted_items) == 1 and os.path.isdir(os.path.join(temp_extract_dir, extracted_items[0])):
                shutil.move(os.path.join(temp_extract_dir, extracted_items[0]), install_path)
            else:
                shutil.move(temp_extract_dir, install_path)
        dlg.close()
        QMessageBox.information(parent, "ESP-IDF", f"ESP-IDF v{version} installed to {install_path}")
        return install_path
    except Exception as e:
        try:
            dlg.close()
        except Exception:
            pass
        QMessageBox.critical(parent, "Error", f"Failed to download ESP-IDF: {e}")
        return None
    finally:
        try:
            if os.path.exists(tmp_path):
                os.unlink(tmp_path)
        except Exception:
            pass

def get_esp_idf_env(idf_path):
    """
    Returns a dict of environment variables after sourcing export.sh/bat.
    """
    if sys.platform.startswith("win"):
        export_script = os.path.join(idf_path, "export.bat")
        if not os.path.exists(export_script):
            raise FileNotFoundError(f"ESP-IDF export.bat not found at {export_script}")
        # Use 'cmd' to run export.bat and then printenv
        cmd = f'"{export_script}" && set'
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, shell=True)
        output = proc.communicate()[0].decode(errors="ignore")
        env = {}
        for line in output.splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                env[k] = v
        return env
    else:
        export_script = os.path.join(idf_path, "export.sh")
        if not os.path.exists(export_script):
            raise FileNotFoundError(f"ESP-IDF export.sh not found at {export_script}")
        # Use bash to source export.sh and print env
        cmd = f'bash -c "source \\"{export_script}\\" >/dev/null 2>&1 && env"'
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, shell=True, executable="/bin/bash")
        output = proc.communicate()[0].decode(errors="ignore")
        env = os.environ.copy()
        for line in output.splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                env[k] = v
        return env