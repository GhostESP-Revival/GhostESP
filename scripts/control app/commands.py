import serial

def send_command(serial_port, command, log_callback=None, disconnect_callback=None):
    """
    Send a command to the ESP32 via serial.

    Args:
        serial_port (serial.Serial): The serial port object.
        command (str): The command string to send.
        log_callback (callable, optional): Function to log messages.
        disconnect_callback (callable, optional): Function to call on error/disconnect.
    """
    if not serial_port or not serial_port.is_open:
        if log_callback:
            log_callback("Not connected to ESP32.")
        return

    if log_callback:
        log_callback(f"Sending command: {command}")
    try:
        serial_port.write(f"{command}\n".encode())
    except Exception as e:
        if log_callback:
            log_callback(f"Error sending command: {str(e)}")
        if disconnect_callback:
            disconnect_callback()

def parse_response(response, display_callback=None, log_callback=None):
    """
    Parse a response from the ESP32 and update the display/log.

    Args:
        response (str): The response string.
        display_callback (callable, optional): Function to update display.
        log_callback (callable, optional): Function to log messages.
    """
    # Example: handle JSON, errors, or plain text
    import json
    from datetime import datetime

    if response.startswith("Error reading serial:"):
        if log_callback:
            log_callback(response)
        return

    try:
        data = json.loads(response)
        if display_callback:
            display_callback(data)
    except json.JSONDecodeError:
        ts = datetime.now().strftime("%H:%M:%S")
        formatted = f"[{ts}] {response}"
        if display_callback:
            display_callback(formatted)
        elif log_callback:
            log_callback(formatted)