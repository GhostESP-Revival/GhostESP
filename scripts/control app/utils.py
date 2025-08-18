from datetime import datetime

def timestamp(fmt="%Y-%m-%d %H:%M:%S"):
    """Return the current timestamp as a string."""
    return datetime.now().strftime(fmt)

def log_message(log_widget, message, show_timestamps=True):
    
    if show_timestamps:
        timestamp = datetime.now().strftime("%H:%M:%S")
        log_widget.append(f"[{timestamp}] {message}")
    else:
        log_widget.append(message)