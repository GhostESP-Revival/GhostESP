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
