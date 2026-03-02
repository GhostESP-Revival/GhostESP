from PyQt6.QtCore import QThread, pyqtSignal
import serial

class SerialMonitorThread(QThread):
    """Thread for monitoring incoming data from the serial port."""
    data_received = pyqtSignal(str)

    def __init__(self, serial_port):
        """
        Initialize the serial monitor thread.

        Args:
            serial_port (serial.Serial): The serial port to monitor.
        """
        super().__init__()
        self.serial_port = serial_port
        self.running = True

    def run(self):
        """Continuously read data from the serial port and emit received lines."""
        while self.running and self.serial_port.is_open:
            try:
                if self.serial_port.in_waiting:
                    data = self.serial_port.readline().decode().strip()
                    if data:
                        self.data_received.emit(data)
            except Exception as e:
                self.data_received.emit(f"Error reading serial: {str(e)}")
                parent = self.parent()
                if parent is not None and hasattr(parent, "disconnect"):
                    parent.disconnect()
                break
            self.msleep(10)

    def stop(self):
        """Stop the serial monitor thread."""
        self.running = False

class PortalFileSenderThread(QThread):
    """Thread for sending a local HTML file to the ESP32 as an evil portal."""

    send_line = pyqtSignal(str)
    finished = pyqtSignal()
    error = pyqtSignal(str)
    progress = pyqtSignal(int)

    def __init__(self, safe_html):
        """
        Initialize the portal file sender thread.

        Args:
            safe_html (str): The HTML content to send.
        """
        super().__init__()
        self.safe_html = safe_html

    def run(self):
        """Send the HTML content in chunks to the ESP32 and emit progress."""
        try:
            self.send_line.emit('evilportal -c sethtmlstr')
            self.msleep(200)
            self.send_line.emit('[HTML/BEGIN]')
            self.msleep(200)
            chunk_size = 256
            total = len(self.safe_html)
            for i in range(0, total, chunk_size):
                self.send_line.emit(self.safe_html[i:i+chunk_size])
                percent = int((i + chunk_size) / total * 100)
                self.progress.emit(min(percent, 100))
                self.msleep(50)
            self.msleep(200)
            self.send_line.emit('[HTML/CLOSE]')
            self.finished.emit()
        except Exception as e:
            self.error.emit(str(e))

class AssetDownloadThread(QThread):
    """Thread for downloading release assets from GitHub without blocking the UI."""
    
    progress_update = pyqtSignal(str, int, int)  # name, downloaded_mb, total_mb
    status_update = pyqtSignal(str)  # status message
    finished = pyqtSignal(str, str)  # file_path, status message
    error = pyqtSignal(str)  # error message
    retry_info = pyqtSignal(str, int, int)  # message, attempt, max_retries
    
    def __init__(self, url, file_path, asset_name):
        """
        Initialize the asset download thread.
        
        Args:
            url (str): The download URL
            file_path (str): Path where the file should be saved
            asset_name (str): Name of the asset being downloaded
        """
        super().__init__()
        self.url = url
        self.file_path = file_path
        self.asset_name = asset_name
        self.running = True
    
    def stop(self):
        """Stop the download thread."""
        self.running = False
    
    def run(self):
        """Download the asset in the background."""
        import requests
        import os
        import time
        from requests.exceptions import RequestException, Timeout, ConnectionError as RequestsConnectionError, ChunkedEncodingError
        
        max_retries = 5
        connect_timeout = 30
        read_timeout = 120
        chunk_size = 32768
        
        # Try to import retry utilities
        try:
            from requests.adapters import HTTPAdapter
            from urllib3.util.retry import Retry
            has_retry_support = True
        except ImportError:
            has_retry_support = False
        
        for attempt in range(max_retries):
            if not self.running:
                return
                
            try:
                # Delete partial download on retry
                if attempt > 0 and os.path.exists(self.file_path):
                    try:
                        os.remove(self.file_path)
                    except:
                        pass
                    # Exponential backoff
                    backoff_time = min(2 ** attempt, 16)
                    self.retry_info.emit(f"Retrying download (attempt {attempt + 1}/{max_retries}) in {backoff_time}s...", attempt + 1, max_retries)
                    time.sleep(backoff_time)
                
                # Create session with retry strategy
                session = requests.Session()
                
                if has_retry_support:
                    retry_strategy = Retry(
                        total=2,
                        backoff_factor=1,
                        status_forcelist=[429, 500, 502, 503, 504],
                        allowed_methods=["GET"]
                    )
                    adapter = HTTPAdapter(max_retries=retry_strategy)
                    session.mount("http://", adapter)
                    session.mount("https://", adapter)
                
                session.headers.update({
                    'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36',
                    'Accept': 'application/octet-stream, */*',
                    'Accept-Encoding': 'identity',
                })
                
                # Make request
                response = session.get(
                    self.url,
                    stream=True,
                    timeout=(connect_timeout, read_timeout),
                    allow_redirects=True
                )
                
                # Handle rate limiting
                if response.status_code == 429:
                    retry_after = int(response.headers.get("Retry-After", 60))
                    self.status_update.emit(f"Rate limited by GitHub. Waiting {retry_after} seconds...")
                    time.sleep(retry_after)
                    continue
                
                # Handle service unavailable
                if response.status_code == 503:
                    wait_time = 30
                    self.status_update.emit(f"Service unavailable. Waiting {wait_time} seconds...")
                    time.sleep(wait_time)
                    continue
                
                response.raise_for_status()
                
                # Get file size
                total_size = int(response.headers.get('content-length', 0))
                downloaded = 0
                last_update = 0
                
                # Download file
                with open(self.file_path, "wb") as f:
                    try:
                        for chunk in response.iter_content(chunk_size=chunk_size):
                            if not self.running:
                                response.close()
                                if os.path.exists(self.file_path):
                                    try:
                                        os.remove(self.file_path)
                                    except:
                                        pass
                                return
                            
                            if chunk:
                                f.write(chunk)
                                downloaded += len(chunk)
                                
                                # Update progress every 5MB
                                if total_size > 0:
                                    if downloaded - last_update >= (5 * 1024 * 1024) or downloaded == total_size:
                                        downloaded_mb = downloaded / (1024 * 1024)
                                        total_mb = total_size / (1024 * 1024)
                                        self.progress_update.emit(self.asset_name, int(downloaded_mb), int(total_mb))
                                        last_update = downloaded
                                else:
                                    if downloaded - last_update >= (5 * 1024 * 1024):
                                        downloaded_mb = downloaded / (1024 * 1024)
                                        self.progress_update.emit(self.asset_name, int(downloaded_mb), 0)
                                        last_update = downloaded
                    finally:
                        response.close()
                
                # Verify file
                if os.path.exists(self.file_path) and os.path.getsize(self.file_path) > 0:
                    if total_size > 0:
                        actual_size = os.path.getsize(self.file_path)
                        if actual_size != total_size:
                            raise Exception(f"File size mismatch: expected {total_size} bytes, got {actual_size} bytes")
                    
                    file_size_mb = os.path.getsize(self.file_path) / (1024 * 1024)
                    self.finished.emit(self.file_path, f"Downloaded {self.asset_name} ({file_size_mb:.1f}MB)")
                    return  # Success
                else:
                    raise Exception("Downloaded file is empty or missing")
                    
            except (RequestsConnectionError, Timeout, ChunkedEncodingError) as e:
                if attempt < max_retries - 1:
                    self.retry_info.emit(f"Connection error (attempt {attempt + 1}/{max_retries}): {str(e)}", attempt + 1, max_retries)
                    continue
                else:
                    self.error.emit(f"Connection error after {max_retries} attempts: {str(e)}")
                    if os.path.exists(self.file_path):
                        try:
                            os.remove(self.file_path)
                        except:
                            pass
                    return
            except RequestException as e:
                if attempt < max_retries - 1:
                    self.retry_info.emit(f"Request error (attempt {attempt + 1}/{max_retries}): {str(e)}", attempt + 1, max_retries)
                    continue
                else:
                    self.error.emit(f"Request error after {max_retries} attempts: {str(e)}")
                    if os.path.exists(self.file_path):
                        try:
                            os.remove(self.file_path)
                        except:
                            pass
                    return
            except Exception as e:
                if "size mismatch" in str(e).lower() and attempt < max_retries - 1:
                    self.retry_info.emit(f"File verification error (attempt {attempt + 1}/{max_retries}): {str(e)}", attempt + 1, max_retries)
                    continue
                else:
                    self.error.emit(f"Download error: {str(e)}")
                    if os.path.exists(self.file_path):
                        try:
                            os.remove(self.file_path)
                        except:
                            pass
                    return

class ReleaseFetchThread(QThread):
    """Thread for fetching GitHub releases without blocking the UI."""
    
    finished = pyqtSignal(list)  # releases list
    error = pyqtSignal(str)  # error message
    retry_info = pyqtSignal(str, int, int)  # message, attempt, max_retries
    
    def __init__(self, api_url, show_prereleases=False):
        """
        Initialize the release fetch thread.
        
        Args:
            api_url (str): The GitHub API URL to fetch releases from
            show_prereleases (bool): Whether to include prereleases
        """
        super().__init__()
        self.api_url = api_url
        self.show_prereleases = show_prereleases
        self.running = True
    
    def stop(self):
        """Stop the fetch thread."""
        self.running = False
    
    def run(self):
        """Fetch releases in the background."""
        import requests
        import time
        from requests.exceptions import RequestException, Timeout, ConnectionError as RequestsConnectionError
        
        max_retries = 3
        timeout = 30
        
        for attempt in range(max_retries):
            if not self.running:
                return
                
            try:
                if attempt > 0:
                    # Exponential backoff
                    backoff_time = min(2 ** attempt, 8)
                    self.retry_info.emit(f"Retrying fetch (attempt {attempt + 1}/{max_retries})...", attempt + 1, max_retries)
                    time.sleep(backoff_time)
                
                response = requests.get(self.api_url, timeout=timeout)
                response.raise_for_status()
                releases = response.json()
                
                # Filter prereleases if needed
                if not self.show_prereleases:
                    releases = [
                        r for r in releases
                        if not r.get("prerelease", False) and not r.get("tag_name", "").startswith("prerelease-")
                    ]
                
                self.finished.emit(releases)
                return  # Success
                
            except Timeout as e:
                if attempt < max_retries - 1:
                    continue
                else:
                    self.error.emit(f"Timeout: Failed to fetch releases from GitHub. The connection timed out after {timeout} seconds. Please check your internet connection and try again.")
                    return
            except RequestsConnectionError as e:
                if attempt < max_retries - 1:
                    continue
                else:
                    self.error.emit(f"Connection error: Failed to fetch releases from GitHub: {str(e)}. You can still use 'Custom local .zip' to flash a local file.")
                    return
            except RequestException as e:
                if attempt < max_retries - 1:
                    continue
                else:
                    self.error.emit(f"Failed to fetch releases from GitHub: {str(e)}. You can still use 'Custom local .zip' to flash a local file.")
                    return
            except Exception as e:
                self.error.emit(f"Unexpected error fetching releases: {str(e)}")
                return
