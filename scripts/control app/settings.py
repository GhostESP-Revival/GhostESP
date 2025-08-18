import json
import os
import re
from datetime import datetime
from PyQt6.QtWidgets import (QDialog, QVBoxLayout, QGroupBox, QButtonGroup, 
                             QRadioButton, QCheckBox, QDialogButtonBox, QApplication)
from PyQt6.QtGui import QPalette, QColor, QFont
from PyQt6.QtCore import Qt


class AppSettings:
    """Manages application settings loading, saving, and theme application."""
    
    def __init__(self, settings_file="app_settings.json"):
        self.settings_file = settings_file
        self.settings = self.load_settings()
    
    def load_settings(self):
        """Load application settings from JSON file."""
        default_settings = {
            "theme": "dark",
            "text_size": "medium",
            "show_timestamps": True
        }
        try:
            if os.path.exists(self.settings_file):
                with open(self.settings_file, 'r') as f:
                    settings = json.load(f)
                    return {**default_settings, **settings}
        except Exception:
            pass
        return default_settings
    
    def save_settings(self):
        """Save application settings to JSON file."""
        try:
            with open(self.settings_file, 'w') as f:
                json.dump(self.settings, f, indent=2)
        except Exception:
            pass
    
    def get(self, key, default=None):
        """Get a setting value."""
        return self.settings.get(key, default)
    
    def set(self, key, value):
        """Set a setting value."""
        self.settings[key] = value
    
    def update(self, new_settings):
        """Update settings with new values."""
        self.settings.update(new_settings)


class ThemeManager:
    """Manages theme application for the GUI."""
    
    @staticmethod
    def apply_theme(widget, theme):
        """Apply the selected theme to a widget."""
        if theme == "dark":
            ThemeManager.setup_dark_theme(widget)
        elif theme == "light":
            ThemeManager.setup_light_theme(widget)
        elif theme == "system":
            ThemeManager.setup_system_theme(widget)
    
    @staticmethod
    def setup_dark_theme(widget):
        """Apply a dark theme to the widget."""
        palette = QPalette()
        palette.setColor(QPalette.ColorRole.Window, QColor(53, 53, 53))
        palette.setColor(QPalette.ColorRole.WindowText, Qt.GlobalColor.white)
        palette.setColor(QPalette.ColorRole.Base, QColor(25, 25, 25))
        palette.setColor(QPalette.ColorRole.AlternateBase, QColor(53, 53, 53))
        palette.setColor(QPalette.ColorRole.ToolTipBase, Qt.GlobalColor.white)
        palette.setColor(QPalette.ColorRole.ToolTipText, Qt.GlobalColor.white)
        palette.setColor(QPalette.ColorRole.Text, Qt.GlobalColor.white)
        palette.setColor(QPalette.ColorRole.Button, QColor(53, 53, 53))
        palette.setColor(QPalette.ColorRole.ButtonText, Qt.GlobalColor.white)
        palette.setColor(QPalette.ColorRole.BrightText, Qt.GlobalColor.red)
        palette.setColor(QPalette.ColorRole.Link, QColor(42, 130, 218))
        palette.setColor(QPalette.ColorRole.Highlight, QColor(42, 130, 218))
        palette.setColor(QPalette.ColorRole.HighlightedText, Qt.GlobalColor.black)
        widget.setPalette(palette)
    
    @staticmethod
    def setup_light_theme(widget):
        """Apply a light theme to the widget."""
        palette = QPalette()
        palette.setColor(QPalette.ColorRole.Window, QColor(240, 240, 240))
        palette.setColor(QPalette.ColorRole.WindowText, Qt.GlobalColor.black)
        palette.setColor(QPalette.ColorRole.Base, QColor(255, 255, 255))
        palette.setColor(QPalette.ColorRole.AlternateBase, QColor(245, 245, 245))
        palette.setColor(QPalette.ColorRole.ToolTipBase, Qt.GlobalColor.white)
        palette.setColor(QPalette.ColorRole.ToolTipText, Qt.GlobalColor.black)
        palette.setColor(QPalette.ColorRole.Text, Qt.GlobalColor.black)
        palette.setColor(QPalette.ColorRole.Button, QColor(240, 240, 240))
        palette.setColor(QPalette.ColorRole.ButtonText, Qt.GlobalColor.black)
        palette.setColor(QPalette.ColorRole.BrightText, Qt.GlobalColor.red)
        palette.setColor(QPalette.ColorRole.Link, QColor(0, 0, 255))
        palette.setColor(QPalette.ColorRole.Highlight, QColor(0, 120, 215))
        palette.setColor(QPalette.ColorRole.HighlightedText, Qt.GlobalColor.white)
        widget.setPalette(palette)
    
    @staticmethod
    def setup_system_theme(widget):
        """Apply the system default theme."""
        widget.setPalette(QApplication.style().standardPalette())
    
    @staticmethod
    def apply_text_size(widget, text_size):
        """Apply the selected text size to the widget."""
        size_map = {
            "small": 9,
            "medium": 11,
            "large": 13,
            "extra_large": 15
        }
        
        font_size = size_map.get(text_size, 11)
        font = QFont()
        font.setPointSize(font_size)
        widget.setFont(font)
        
        # Apply to text areas specifically
        if hasattr(widget, 'display_text'):
            text_font = QFont("Consolas", font_size)
            widget.display_text.setFont(text_font)
        if hasattr(widget, 'log_text'):
            log_font = QFont("Consolas", font_size)
            widget.log_text.setFont(log_font)


class TimestampManager:
    """Manages timestamp display in text widgets."""
    
    @staticmethod
    def update_existing_logs_timestamps(widget, show_timestamps):
        """Update existing logs in display and log text areas to add/remove timestamps."""
        # Update display text area
        if hasattr(widget, 'display_text'):
            TimestampManager._update_text_widget_timestamps(widget.display_text, show_timestamps)
                
        # Update log text area
        if hasattr(widget, 'log_text'):
            TimestampManager._update_text_widget_timestamps(widget.log_text, show_timestamps)
    
    @staticmethod
    def _update_text_widget_timestamps(text_widget, show_timestamps):
        """Update timestamps in a single text widget."""
        content = text_widget.toPlainText()
        if content:
            lines = content.split('\n')
            updated_lines = []
            
            for line in lines:
                if not line.strip():
                    updated_lines.append(line)
                    continue
                    
                # Check if line already has timestamp (format: [HH:MM:SS])
                timestamp_pattern = r'^\[\d{2}:\d{2}:\d{2}\] '
                has_timestamp = bool(re.match(timestamp_pattern, line))
                
                if show_timestamps and not has_timestamp:
                    # Add timestamp to line
                    timestamp_str = datetime.now().strftime("[%H:%M:%S] ")
                    updated_lines.append(timestamp_str + line)
                elif not show_timestamps and has_timestamp:
                    # Remove timestamp from line
                    updated_lines.append(re.sub(timestamp_pattern, '', line))
                else:
                    # Keep line as is
                    updated_lines.append(line)
                    
            text_widget.setPlainText('\n'.join(updated_lines))


class AppSettingsDialog(QDialog):
    """Dialog for application settings."""
    
    def __init__(self, parent, current_settings):
        super().__init__(parent)
        self.setWindowTitle("Config")
        self.setModal(True)
        self.setMinimumSize(450, 350)
        self.resize(500, 400)
        
        self.settings = current_settings.copy()
        self.setup_ui()
        self.apply_theme()
        
    def setup_ui(self):
        """Set up the settings dialog UI."""
        layout = QVBoxLayout(self)
        
        # Theme settings
        theme_group = QGroupBox("Theme")
        theme_layout = QVBoxLayout(theme_group)
        
        self.theme_group = QButtonGroup()
        themes = [("Dark", "dark"), ("Light", "light"), ("System", "system")]
        
        for text, value in themes:
            radio = QRadioButton(text)
            radio.setChecked(self.settings.get("theme") == value)
            radio.toggled.connect(lambda checked, v=value: self.on_theme_changed(v, checked))
            self.theme_group.addButton(radio)
            theme_layout.addWidget(radio)
            
        layout.addWidget(theme_group)
        
        # Text size settings
        size_group = QGroupBox("Text Size")
        size_layout = QVBoxLayout(size_group)
        
        self.size_group = QButtonGroup()
        sizes = [("Small", "small"), ("Medium", "medium"), ("Large", "large"), ("Extra Large", "extra_large")]
        
        for text, value in sizes:
            radio = QRadioButton(text)
            radio.setChecked(self.settings.get("text_size") == value)
            radio.toggled.connect(lambda checked, v=value: self.on_size_changed(v, checked))
            self.size_group.addButton(radio)
            size_layout.addWidget(radio)
            
        layout.addWidget(size_group)
        
        # Display settings
        display_group = QGroupBox("Display Options")
        display_layout = QVBoxLayout(display_group)
        
        self.timestamps_checkbox = QCheckBox("Show timestamps in output")
        self.timestamps_checkbox.setChecked(self.settings.get("show_timestamps", True))
        self.timestamps_checkbox.toggled.connect(self.on_timestamps_changed)
        display_layout.addWidget(self.timestamps_checkbox)
        
        layout.addWidget(display_group)
        
        # Buttons
        button_box = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        button_box.accepted.connect(self.accept)
        button_box.rejected.connect(self.reject)
        layout.addWidget(button_box)
        
    def on_theme_changed(self, value, checked):
        """Handle theme selection change."""
        if checked:
            self.settings["theme"] = value
            self.apply_theme()
            
    def on_size_changed(self, value, checked):
        """Handle text size selection change."""
        if checked:
            self.settings["text_size"] = value
            
    def on_timestamps_changed(self, checked):
        """Handle timestamp checkbox change."""
        self.settings["show_timestamps"] = checked
        
    def get_settings(self):
        """Return the current settings."""
        return self.settings
        
    def apply_theme(self):
        """Apply the current theme to the settings dialog."""
        theme = self.settings.get("theme", "dark")
        
        if theme == "dark":
            self.setStyleSheet("""
                QDialog {
                    background-color: #2b2b2b;
                    color: #ffffff;
                }
                QGroupBox {
                    font-weight: bold;
                    border: 2px solid #555555;
                    border-radius: 5px;
                    margin-top: 1ex;
                    padding-top: 10px;
                }
                QGroupBox::title {
                    subcontrol-origin: margin;
                    left: 10px;
                    padding: 0 5px 0 5px;
                }
                QRadioButton {
                    spacing: 5px;
                    padding: 5px;
                }
                QCheckBox {
                    spacing: 5px;
                    padding: 5px;
                }
                QPushButton {
                    background-color: #404040;
                    border: 1px solid #555555;
                    border-radius: 4px;
                    padding: 6px;
                    min-width: 80px;
                }
                QPushButton:hover {
                    background-color: #505050;
                }
                QPushButton:pressed {
                    background-color: #353535;
                }
            """)
        elif theme == "light":
            self.setStyleSheet("""
                QDialog {
                    background-color: #ffffff;
                    color: #000000;
                }
                QGroupBox {
                    font-weight: bold;
                    border: 2px solid #cccccc;
                    border-radius: 5px;
                    margin-top: 1ex;
                    padding-top: 10px;
                    color: #000000;
                }
                QGroupBox::title {
                    subcontrol-origin: margin;
                    left: 10px;
                    padding: 0 5px 0 5px;
                    color: #000000;
                }
                QRadioButton {
                    spacing: 5px;
                    padding: 5px;
                    color: #000000;
                }
                QCheckBox {
                    spacing: 5px;
                    padding: 5px;
                    color: #000000;
                }
                QPushButton {
                    background-color: #f0f0f0;
                    border: 1px solid #cccccc;
                    border-radius: 4px;
                    padding: 6px;
                    min-width: 80px;
                    color: #000000;
                }
                QPushButton:hover {
                    background-color: #e0e0e0;
                }
                QPushButton:pressed {
                    background-color: #d0d0d0;
                }
            """)
        else:
            self.setStyleSheet("")
