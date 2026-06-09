import json
import re
from serial.tools import list_ports
import serial
from serial_threads import SerialMonitorThread, PortalFileSenderThread, AssetDownloadThread, ReleaseFetchThread
from dialogs import show_select_ap_dialog, show_custom_beacon_dialog, show_printer_dialog
from utils import log_message, timestamp
from espidf_utils import find_esp_idf_gui, download_esp_idf_gui, get_esp_idf_env
from settings import AppSettings, ThemeManager, TimestampManager, AppSettingsDialog
from datetime import datetime
from PyQt6.QtWidgets import (QApplication, QMainWindow, QWidget, QVBoxLayout,
                             QHBoxLayout, QComboBox, QPushButton, QLabel, QTextEdit,
                             QTabWidget, QGroupBox, QGridLayout, QLineEdit, QMessageBox,
                             QSplitter, QInputDialog, QSpinBox, QFormLayout, QStyle, QFileDialog, QCheckBox, QDialog, QProgressBar, QSizePolicy, QStackedWidget,
                             QMenuBar, QDialogButtonBox, QSlider, QRadioButton)
from PyQt6.QtGui import QAction
from PyQt6.QtCore import Qt, pyqtSignal, QThread, QTimer
from PyQt6.QtGui import QFont, QTextCursor, QIcon
from functools import partial
import glob
import requests
import tempfile
import os
import sys
from PyQt6.QtWidgets import QApplication

QApplication.setStyle("Fusion")

class ESP32ControlGUI(QMainWindow):
    """Main GUI class for controlling the Ghost ESP32 device."""

    def __init__(self):
        """Initialize the ESP32 control panel GUI and its components."""
        super().__init__()
        
        # Initialize flag to track initialization completion
        self._initialization_complete = False
        
        self.setWindowTitle("Ghost ESP Commander")
        self.setGeometry(100, 100, 1400, 900)
        # Set minimum size to prevent window from shrinking too much
        self.setMinimumSize(800, 600)

        # Set custom app icon
        self.setWindowIcon(QIcon("assets/gesp_ghost_trans_bg.png"))  # Use .ico, .png, or .svg

        # Initialize serial communication variables
        self.serial_port = None
        self.monitor_thread = None

        # Initialize app settings
        self.app_settings = AppSettings()
        
        # Set up menu bar
        self.setup_menu_bar()

        # Apply theme based on settings
        self.apply_theme()

        # Create central widget
        central_widget = QWidget()
        self.setCentralWidget(central_widget)

        # Create main layout for central widget
        main_layout = QVBoxLayout(central_widget)

        # Set up the main layout
        self.setup_ui(main_layout)

        # Refresh available ports
        self.refresh_ports()

        # --- Auto Reconnect Timer ---
        self.reconnect_timer = QTimer(self)
        self.reconnect_timer.setInterval(2000)  # Check every 2 seconds
        self.reconnect_timer.timeout.connect(self.check_auto_reconnect)
        self.reconnect_timer.start()
        # --- End Auto Reconnect Timer ---

        # Overlay message for no connection
        self.overlay = QLabel(self.centralWidget())
        self.overlay.setText("No serial connection.\nConnect to enable controls.")
        self.overlay.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.overlay.setStyleSheet("""
            background-color: rgba(30, 30, 30, 180);
            color: white;
            font-size: 24px;
            border-radius: 10px;
        """)
        self.overlay.hide()

        # Mark initialization as complete
        self._initialization_complete = True

        # Disable UI except serial connection bar at startup
        self.set_main_ui_enabled(False)

        # --- Fix: set overlay geometry at startup ---
        self.resizeEvent(None)

        # Command history for custom commands
        self.command_history = []
        self.history_index = -1

        # Reconnect settings
        self.reconnect_attempts = 0
        self.reconnect_base_interval = 2000  # 2 seconds



    def setup_menu_bar(self):
        """Set up the application menu bar."""
        menubar = self.menuBar()
        
        # Settings menu
        settings_menu = menubar.addMenu('Preferences')
        
        settings_action = QAction('Config...', self)
        settings_action.triggered.connect(self.show_app_settings_dialog)
        settings_menu.addAction(settings_action)

    def apply_theme(self):
        """Apply the selected theme to the application."""
        theme = self.app_settings.get("theme", "dark")
        ThemeManager.apply_theme(self, theme)
        
        # Apply text size
        text_size = self.app_settings.get("text_size", "medium")
        ThemeManager.apply_text_size(self, text_size)



    def show_app_settings_dialog(self):
        """Show the application settings dialog."""
        old_show_timestamps = self.app_settings.get("show_timestamps", True)
        dialog = AppSettingsDialog(self, self.app_settings.settings)
        if dialog.exec() == QDialog.DialogCode.Accepted:
            new_settings = dialog.get_settings()
            new_show_timestamps = new_settings.get("show_timestamps", True)
            
            self.app_settings.update(new_settings)
            self.app_settings.save_settings()
            self.apply_theme()
            
            # Update existing logs if timestamp setting changed
            if old_show_timestamps != new_show_timestamps:
                TimestampManager.update_existing_logs_timestamps(self, new_show_timestamps)
                


    def resizeEvent(self, event):
        """
        Handle window resize events to adjust overlay geometry.

        Args:
            event (QResizeEvent): The resize event.
        """
        super().resizeEvent(event)
        
        # Only proceed if initialization is complete and overlay exists
        if not self._initialization_complete or not hasattr(self, 'overlay'):
            return
            
        # Get geometry of central widget
        central_geom = self.centralWidget().geometry()
        # Find the serial connection bar widget by object name
        connection_bar = self.findChild(QGroupBox, "serial_connection_bar")
        if connection_bar:
            # Get connection bar geometry relative to central widget
            bar_geom = connection_bar.geometry()
            # Overlay should start below the connection bar
            x = 0
            y = bar_geom.y() + bar_geom.height()
            w = central_geom.width()
            h = central_geom.height() - y
            # Set overlay geometry relative to central widget
            self.overlay.setGeometry(x, y, w, h)
        else:
            # Fallback: cover everything except top 60px (adjust if needed)
            self.overlay.setGeometry(0, 60, central_geom.width(), central_geom.height() - 60)

    def set_main_ui_enabled(self, enabled):
        """
        Enable or disable the main UI panels except the serial connection bar.

        Args:
            enabled (bool): Whether to enable or disable the UI.
        """
        try:
            self.panel_container.setEnabled(enabled)
            if hasattr(self, 'panel_tabs'):
                self.panel_tabs.setEnabled(enabled)
            self.log_group.setEnabled(enabled)
            self.display_text.setEnabled(enabled)
            self.cmd_entry.setEnabled(enabled)
        except Exception:
            pass  # Ignore if not initialized yet

        # Only show overlay if not in flash mode
        if not enabled and (not hasattr(self, "flash_mode_btn") or not self.flash_mode_btn.isChecked()):
            self.overlay.show()
            self.overlay.raise_()
        else:
            self.overlay.hide()



    def setup_ui(self, main_layout):
        """
        Set up the main UI layout and widgets.

        Args:
            main_layout (QVBoxLayout): The main layout to populate.
        """
        # Create top bar for serial connection
        self.setup_connection_bar(main_layout)

        # --- Main content area (everything below serial bar) ---
        self.main_content_widget = QWidget()
        self.main_content_layout = QVBoxLayout(self.main_content_widget)
        self.main_content_layout.setContentsMargins(0, 0, 0, 0)

        # Create splitter for resizable sections (vertical: content/log)
        splitter = QSplitter(Qt.Orientation.Vertical)
        self.main_content_layout.addWidget(splitter)

        # --- Horizontal splitter for left/right ---
        content_splitter = QSplitter(Qt.Orientation.Horizontal)
        splitter.addWidget(content_splitter)

        # Left side - Command panels
        left_widget = QWidget()
        left_layout = QVBoxLayout(left_widget)
        self.setup_command_tabs(left_layout)
        left_widget.setLayout(left_layout)
        content_splitter.addWidget(left_widget)

        # Right side - Display area
        right_widget = QWidget()
        right_layout = QVBoxLayout(right_widget)
        self.setup_display_area(right_layout)
        right_widget.setLayout(right_layout)
        content_splitter.addWidget(right_widget)

        # Set initial sizes (optional)
        content_splitter.setStretchFactor(0, 1)
        content_splitter.setStretchFactor(1, 2)

        # Bottom - Log area
        self.setup_log_area()
        splitter.addWidget(self.log_group)

        # Set splitter stretch factors
        splitter.setStretchFactor(0, 3)  # Content area
        splitter.setStretchFactor(1, 1)  # Log area

        # Add main content widget to the main layout with stretch
        main_layout.addWidget(self.main_content_widget, stretch=1)

        # --- Flash mode widget placeholder (hidden by default) ---
        self.flash_mode_widget = QWidget()
        self.flash_mode_widget.hide()

        # Use a splitter for left (controls) and right (console)
        flash_splitter = QSplitter(Qt.Orientation.Horizontal, self.flash_mode_widget)

        # Left side: controls
        flash_controls_widget = QWidget()
        flash_controls_layout = QVBoxLayout(flash_controls_widget)
        flash_controls_layout.setContentsMargins(0, 0, 0, 0)

        # --- Flash Mode Panels as QTabWidget ---
        self.flash_panel_tabs = QTabWidget()
        self.flash_panel_tabs.setTabPosition(QTabWidget.TabPosition.North)
        flash_controls_layout.addWidget(self.flash_panel_tabs)

        # Panel 1: Flash Firmware
        flash_firmware_panel = QWidget()
        flash_firmware_layout = QVBoxLayout(flash_firmware_panel)
        flash_firmware_layout.addWidget(QLabel("Select a firmware file and flash your ESP32 board."))

        # --- Chip selection row ---
        chip_layout = QHBoxLayout()
        self.chip_combo = QComboBox()
        self.chip_combo.addItem("")  # Add blank/placeholder entry
        self.chip_combo.addItems(["esp32", "esp32s2", "esp32s3", "esp32c3", "esp32c5", "esp32c6"])
        self.chip_combo.setCurrentIndex(0)  # Start with blank selected
        chip_layout.addWidget(QLabel("Chip:"))
        chip_layout.addWidget(self.chip_combo)
        flash_firmware_layout.addLayout(chip_layout)
        self.selected_chip = ""  # Default to blank
        self.chip_combo.currentTextChanged.connect(self.set_chip_type)

        # --- Flash mode selection (separate files vs merged .bin) ---
        flash_mode_layout = QHBoxLayout()
        self.flash_mode_separate_radio = QRadioButton("Separate files")
        self.flash_mode_merged_radio = QRadioButton("Merged .bin file")
        self.flash_mode_separate_radio.setChecked(True)  # Default to separate files
        flash_mode_layout.addWidget(QLabel("Mode:"))
        flash_mode_layout.addWidget(self.flash_mode_separate_radio)
        flash_mode_layout.addWidget(self.flash_mode_merged_radio)
        flash_mode_layout.addStretch()
        flash_firmware_layout.addLayout(flash_mode_layout)
        self.flash_mode_separate_radio.toggled.connect(self.on_flash_mode_changed)
        self.flash_mode_merged_radio.toggled.connect(self.on_flash_mode_changed)

        # --- Container for separate files mode ---
        self.separate_files_widget = QWidget()
        separate_files_layout = QVBoxLayout(self.separate_files_widget)
        separate_files_layout.setContentsMargins(0, 0, 0, 0)

        # --- Bootloader file selection ---
        bootloader_layout = QHBoxLayout()
        self.bootloader_file_edit = QLineEdit()
        self.bootloader_file_edit.setPlaceholderText("Select bootloader.bin...")
        bootloader_layout.addWidget(self.bootloader_file_edit)
        bootloader_browse_btn = QPushButton("Browse")
        bootloader_browse_btn.clicked.connect(lambda: self.browse_bin_file(self.bootloader_file_edit))
        bootloader_layout.addWidget(bootloader_browse_btn)
        separate_files_layout.addLayout(bootloader_layout)

        # --- Partition table file selection ---
        partition_layout = QHBoxLayout()
        self.partition_file_edit = QLineEdit()
        self.partition_file_edit.setPlaceholderText("Select partition-table.bin...")
        partition_layout.addWidget(self.partition_file_edit)
        partition_browse_btn = QPushButton("Browse")
        partition_browse_btn.clicked.connect(lambda: self.browse_bin_file(self.partition_file_edit))
        partition_layout.addWidget(partition_browse_btn)
        separate_files_layout.addLayout(partition_layout)

        # --- Firmware file selection ---
        firmware_layout = QHBoxLayout()
        self.firmware_file_edit = QLineEdit()
        self.firmware_file_edit.setPlaceholderText("Select firmware.bin...")
        firmware_layout.addWidget(self.firmware_file_edit)
        firmware_browse_btn = QPushButton("Browse")
        firmware_browse_btn.clicked.connect(lambda: self.browse_bin_file(self.firmware_file_edit))
        firmware_layout.addWidget(firmware_browse_btn)
        separate_files_layout.addLayout(firmware_layout)

        flash_firmware_layout.addWidget(self.separate_files_widget)

        # --- Container for merged file mode ---
        self.merged_file_widget = QWidget()
        merged_file_layout = QVBoxLayout(self.merged_file_widget)
        merged_file_layout.setContentsMargins(0, 0, 0, 0)
        self.merged_file_widget.hide()  # Hidden by default

        # --- Merged .bin file selection ---
        merged_layout = QHBoxLayout()
        self.merged_file_edit = QLineEdit()
        self.merged_file_edit.setPlaceholderText("Select merged .bin file...")
        merged_layout.addWidget(self.merged_file_edit)
        merged_browse_btn = QPushButton("Browse")
        merged_browse_btn.clicked.connect(lambda: self.browse_bin_file(self.merged_file_edit))
        merged_layout.addWidget(merged_browse_btn)
        merged_file_layout.addLayout(merged_layout)

        flash_firmware_layout.addWidget(self.merged_file_widget)

        # --- Flash/Exit buttons and status ---
        self.flash_btn = QPushButton("Flash Board")
        self.flash_btn.clicked.connect(self.flash_board)
        flash_firmware_layout.addWidget(self.flash_btn)
        self.flash_status = QLabel("")
        self.flash_status.setWordWrap(True)
        self.flash_status.setSizePolicy(QSizePolicy.Policy.Preferred, QSizePolicy.Policy.Preferred)
        self.flash_status.setMaximumHeight(50)  # Limit height to prevent expansion
        flash_firmware_layout.addWidget(self.flash_status)
        exit_btn = QPushButton("Exit Flash Mode")
        exit_btn.clicked.connect(self.exit_flash_mode)
        flash_firmware_layout.addWidget(exit_btn)
        flash_firmware_layout.addStretch()

        # Add firmware panel as tab
        self.flash_panel_tabs.addTab(flash_firmware_panel, "Flash Firmware")


        # --- Panel 2: Flash Release Bundle ---
        release_bundle_panel = QWidget()
        release_bundle_layout = QVBoxLayout(release_bundle_panel)
        release_bundle_layout.setAlignment(Qt.AlignmentFlag.AlignTop)  # Stack all controls to the top

        # Centered top label
        top_label = QLabel("Select a release bundle (.zip) containing bootloader.bin, partition-table.bin, and firmware.bin.")
        top_label.setAlignment(Qt.AlignmentFlag.AlignHCenter | Qt.AlignmentFlag.AlignTop)
        release_bundle_layout.addWidget(top_label)

        # --- Version selection dropdown ---
        version_layout = QHBoxLayout()
        version_label = QLabel("Version:")
        version_label.setContentsMargins(0, 0, 0, 0)
        version_layout.setSpacing(0)
        self.release_version_combo = QComboBox()
        self.release_version_combo.addItem("Loading...")

        # --- Show pre-releases checkbox ---
        self.show_prereleases_checkbox = QCheckBox("Show pre-releases")
        self.show_prereleases_checkbox.setChecked(False)
        version_layout.addWidget(version_label)
        version_layout.addWidget(self.release_version_combo)
        version_layout.addWidget(self.show_prereleases_checkbox)
        release_bundle_layout.addLayout(version_layout)

        self.release_version_combo.currentIndexChanged.connect(self.update_release_assets_dropdown)

        # Connect the checkbox to refetch releases when toggled
        self.show_prereleases_checkbox.stateChanged.connect(self.fetch_github_releases)

        # --- Chip selection row for release bundle ---
        release_chip_layout = QHBoxLayout()
        release_chip_label = QLabel("Chip:")
        release_chip_label.setContentsMargins(0, 0, 0, 0)
        release_chip_layout.setSpacing(0)
        self.release_chip_combo = QComboBox()
        self.release_chip_combo.addItem("")  # Blank/placeholder
        self.release_chip_combo.addItems(["esp32", "esp32s2", "esp32s3", "esp32c3", "esp32c5", "esp32c6"])
        self.release_chip_combo.setCurrentIndex(0)
        release_chip_layout.addWidget(release_chip_label)
        release_chip_layout.addWidget(self.release_chip_combo)
        release_bundle_layout.addLayout(release_chip_layout)
        self.release_chip_combo.currentTextChanged.connect(self.set_chip_type)

        # --- ZIP file selection ---
        zip_layout = QHBoxLayout()
        self.release_zip_edit = QLineEdit()
        self.release_zip_edit.setPlaceholderText("Select release bundle .zip...")
        zip_layout.addWidget(self.release_zip_edit)
        release_zip_browse_btn = QPushButton("Browse")
        release_zip_browse_btn.clicked.connect(lambda: self.browse_zip_file(self.release_zip_edit))
        zip_layout.addWidget(release_zip_browse_btn)
        release_bundle_layout.addLayout(zip_layout)


        # --- Flash/Exit buttons and status for release bundle ---
        self.flash_bundle_btn = QPushButton("Flash Bundle")
        self.flash_bundle_btn.clicked.connect(self.flash_release_bundle)
        release_bundle_layout.addWidget(self.flash_bundle_btn)
        self.flash_bundle_status = QLabel("")
        self.flash_bundle_status.setWordWrap(True)
        self.flash_bundle_status.setSizePolicy(QSizePolicy.Policy.Preferred, QSizePolicy.Policy.Preferred)
        self.flash_bundle_status.setMaximumHeight(50)  # Limit height to prevent expansion
        release_bundle_layout.addWidget(self.flash_bundle_status)

        self.flash_panel_tabs.addTab(release_bundle_panel, "Flash Release Bundle")

        # Panel 3: Custom Build
        custom_build_panel = QWidget()
        custom_build_layout = QVBoxLayout(custom_build_panel)
        custom_build_layout.setAlignment(Qt.AlignmentFlag.AlignTop)  # Stack all controls to the top

        # Ensure all layouts added to custom_build_layout are also aligned to the top
        custom_build_layout.setAlignment(Qt.AlignmentFlag.AlignTop)

        # Top label
        custom_label = QLabel("Custom Build: Configure and compile your own custom firmware image\n idf.py must be installed and in your PATH.")
        custom_label.setAlignment(Qt.AlignmentFlag.AlignHCenter | Qt.AlignmentFlag.AlignTop)
        custom_build_layout.addWidget(custom_label)

                # Add this in your setup_ui method, after setting up the custom build panel (e.g., after custom_build_layout is created):

        idf_status_layout = QHBoxLayout()
        idf_status_label = QLabel("ESP-IDF (idf.py):")
        idf_status_label.setContentsMargins(0, 0, 0, 0)
        idf_status_layout.addWidget(idf_status_label)

        idf_path = find_esp_idf_gui(self)
        self.idf_status_indicator = QLabel()
        if idf_path:
            self.idf_status_indicator.setText("Found")
            self.idf_status_indicator.setStyleSheet("color: #44bb44; font-weight: bold;")
            self.idf_status_indicator.setToolTip(f"idf.py found at: {idf_path}")
        else:
            self.idf_status_indicator.setText("Not Found")
            self.idf_status_indicator.setStyleSheet("color: #ff4444; font-weight: bold;")
            self.idf_status_indicator.setToolTip("idf.py not found in PATH. ESP-IDF features will not work.")

        idf_status_layout.addWidget(self.idf_status_indicator)
        idf_status_layout.addStretch()
        custom_build_layout.addLayout(idf_status_layout)

        # Button to run idf.py fullclean
        self.fullclean_btn = QPushButton("Run idf.py fullclean")
        self.fullclean_btn.setToolTip("Run idf.py fullclean in your project (requires ESP-IDF in PATH)")
        self.fullclean_btn.clicked.connect(self.run_idf_fullclean)
        custom_build_layout.addWidget(self.fullclean_btn)

                # --- SDKConfig selection dropdown ---
        config_layout = QHBoxLayout()
        config_label = QLabel("Copy existing SDKConfig template:")
        config_label.setContentsMargins(0, 0, 0, 0)
        config_layout.setSpacing(0)
        self.sdkconfig_combo = QComboBox()
        self.sdkconfig_combo.setMinimumWidth(250)

        # Populate dropdown with files from ../../configs
        import os
        config_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../configs"))
        config_files = sorted(glob.glob(os.path.join(config_dir, "*")))
        for f in config_files:
            if os.path.isfile(f):
                self.sdkconfig_combo.addItem(os.path.basename(f), f)
        config_layout.addWidget(config_label)
        config_layout.addWidget(self.sdkconfig_combo)
        custom_build_layout.addLayout(config_layout)

        # Add a button with a more intuitive copy icon next to the dropdown
        copy_sdkconfig_btn = QPushButton()
        copy_sdkconfig_btn.setIcon(self.style().standardIcon(QStyle.StandardPixmap.SP_DialogOpenButton))  # Use download/open icon
        copy_sdkconfig_btn.setToolTip("Copy selected config to ../../sdkconfig and ../../sdkconfig.defaults")
        copy_sdkconfig_btn.setFixedSize(28, 28)  # Match the size of the trash icon
        config_layout.addWidget(copy_sdkconfig_btn)

        def copy_selected_sdkconfig():
            import shutil
            import os
            src = self.sdkconfig_combo.currentData()
            if not src or not os.path.isfile(src):
                QMessageBox.warning(self, "Copy Failed", "No valid config file selected.")
                return
            dest1 = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../sdkconfig"))
            dest2 = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../sdkconfig.defaults"))
            try:
                shutil.copyfile(src, dest1)
                shutil.copyfile(src, dest2)
                self.flash_console.append(f"Copied {os.path.basename(src)} to sdkconfig and sdkconfig.defaults.")
                QMessageBox.information(self, "Copied", f"Copied to:\n{dest1}\n{dest2}")
            except Exception as e:
                QMessageBox.critical(self, "Copy Failed", f"Failed to copy: {e}")

        copy_sdkconfig_btn.clicked.connect(copy_selected_sdkconfig)

        # Add a trashcan icon button to delete ../../sdkconfig and ../../sdkconfig.defaults
        delete_sdkconfig_btn = QPushButton()
        delete_sdkconfig_btn.setIcon(self.style().standardIcon(QStyle.StandardPixmap.SP_TrashIcon))
        delete_sdkconfig_btn.setToolTip("Delete ../../sdkconfig and ../../sdkconfig.defaults")
        delete_sdkconfig_btn.setFixedSize(28, 28)
        config_layout.addWidget(delete_sdkconfig_btn)

        def delete_selected_sdkconfig():
            import os
            removed = []
            errors = []
            for fname in ["../../sdkconfig", "../../sdkconfig.defaults"]:
                path = os.path.abspath(os.path.join(os.path.dirname(__file__), fname))
                try:
                    if os.path.isfile(path):
                        os.remove(path)
                        removed.append(path)
                except Exception as e:
                    errors.append(f"{path}: {e}")
            if removed:
                self.flash_console.append("Deleted:\n" + "\n".join(removed))
                QMessageBox.information(self, "Deleted", "Deleted:\n" + "\n".join(removed))
            if errors:
                self.flash_console.append("Errors:\n" + "\n".join(errors))
                QMessageBox.critical(self, "Delete Failed", "Errors:\n" + "\n".join(errors))
            if not removed and not errors:
                QMessageBox.information(self, "Nothing to Delete", "No sdkconfig files found to delete.")

        delete_sdkconfig_btn.clicked.connect(delete_selected_sdkconfig)

        # --- Chip selection (replace the current section in setup_ui for custom_build_panel) ---

        custom_chip_layout = QHBoxLayout()
        custom_chip_label = QLabel("Set idf.py target chip:")
        custom_chip_label.setContentsMargins(0, 0, 0, 0)
        custom_chip_layout.setSpacing(0)
        self.custom_chip_combo = QComboBox()
        self.custom_chip_combo.addItem("")  # Blank/placeholder
        self.custom_chip_combo.addItems(["esp32", "esp32s2", "esp32s3", "esp32c3", "esp32c5", "esp32c6"])
        self.custom_chip_combo.setCurrentIndex(0)
        custom_chip_layout.addWidget(custom_chip_label)
        custom_chip_layout.addWidget(self.custom_chip_combo)

        # Add set-target button next to chip dropdown
        set_target_btn = QPushButton("Set Target")
        set_target_btn.setToolTip("Run idf.py set-target for the selected chip (requires ESP-IDF in PATH)")
        set_target_btn.setFixedHeight(28)
        set_target_btn.clicked.connect(self.run_idf_set_target)
        custom_chip_layout.addWidget(set_target_btn)

        custom_build_layout.addLayout(custom_chip_layout)
        self.custom_chip_combo.currentTextChanged.connect(self.set_chip_type)

        # Button to run idf.py menuconfig
        self.menuconfig_btn = QPushButton("Customize SDKConfig")
        self.menuconfig_btn.setToolTip("Open ESP-IDF menuconfig for your project (requires ESP-IDF in PATH)")
        self.menuconfig_btn.clicked.connect(self.run_idf_menuconfig)
        custom_build_layout.addWidget(self.menuconfig_btn)

        # Button to run idf.py build
        self.build_btn = QPushButton("Run Build")
        self.build_btn.setToolTip("Run idf.py build in your project (requires ESP-IDF in PATH)")
        self.build_btn.clicked.connect(self.run_idf_build)
        custom_build_layout.addWidget(self.build_btn)



        # Flash/Exit buttons and status
        self.custom_flash_btn = QPushButton("Flash Custom Build")
        self.custom_flash_btn.clicked.connect(self.flash_custom_build)
        custom_build_layout.addWidget(self.custom_flash_btn)
        self.custom_flash_status = QLabel("")
        self.custom_flash_status.setWordWrap(True)
        self.custom_flash_status.setSizePolicy(QSizePolicy.Policy.Preferred, QSizePolicy.Policy.Preferred)
        self.custom_flash_status.setMaximumHeight(50)  # Limit height to prevent expansion
        custom_build_layout.addWidget(self.custom_flash_status)

        self.flash_panel_tabs.addTab(custom_build_panel, "Custom Build")

        # Connect tab change to show instructions
        self.flash_panel_tabs.currentChanged.connect(self.on_flash_panel_changed)

        # Add controls to splitter
        flash_splitter.addWidget(flash_controls_widget)

        # Right side: console output
        flasher_output_layout = QVBoxLayout()
        flasher_output_label = QLabel("Flasher Output")
        flasher_output_label.setStyleSheet("font-weight: bold; font-size: 16px;")
        flasher_output_layout.addWidget(flasher_output_label)
        
        # Progress bar for flash progress
        self.flash_progress_bar = QProgressBar()
        self.flash_progress_bar.setMinimum(0)
        self.flash_progress_bar.setMaximum(100)
        self.flash_progress_bar.setValue(0)
        self.flash_progress_bar.setVisible(False)
        self.flash_progress_label = QLabel("")
        self.flash_progress_label.setVisible(False)
        progress_layout = QVBoxLayout()
        progress_layout.addWidget(self.flash_progress_label)
        progress_layout.addWidget(self.flash_progress_bar)
        flasher_output_layout.addLayout(progress_layout)
        
        self.flash_console = QTextEdit()
        self.flash_console.setReadOnly(True)
        self.flash_console.setMinimumWidth(400)
        flasher_output_layout.addWidget(self.flash_console)

        # Create a widget to hold the label and text box
        flasher_output_widget = QWidget()
        flasher_output_widget.setLayout(flasher_output_layout)
        flash_splitter.addWidget(flasher_output_widget)

        # Set initial splitter ratio (left:right)
        flash_splitter.setStretchFactor(0, 1)
        flash_splitter.setStretchFactor(1, 2)

        # Add splitter to flash_mode_widget layout
        flash_layout = QVBoxLayout(self.flash_mode_widget)
        flash_layout.setContentsMargins(0, 0, 0, 0)
        flash_layout.addWidget(flash_splitter)

        # Add flash mode widget to the main layout with stretch
        main_layout.addWidget(self.flash_mode_widget, stretch=1)

    def toggle_flash_mode(self):
        """Toggle between the main UI and Flash Mode view."""
        if self.flash_mode_btn.isChecked():
            # If serial is connected, disconnect before entering flash mode
            if self.serial_port and self.serial_port.is_open:
                self.disconnect()
            # Turn off auto reconnect if it's on
            if self.auto_reconnect_checkbox.isChecked():
                self.auto_reconnect_checkbox.setChecked(False)
            self.main_content_widget.hide()
            self.flash_mode_widget.show()
            # Hide connect button and auto reconnect checkbox in flash mode
            self.connect_btn.hide()
            self.auto_reconnect_checkbox.hide()
            if hasattr(self, "status_indicator"):
                self.status_indicator.hide()
            self.overlay.hide()
            # Change button text to "Command Mode"
            self.flash_mode_btn.setText("Command Mode")
        else:
            self.flash_mode_widget.hide()
            self.main_content_widget.show()
            self.connect_btn.show()
            self.auto_reconnect_checkbox.show()
            if hasattr(self, "status_indicator"):
                self.status_indicator.show()
            self.connect_btn.setEnabled(True)
            is_connected = self.serial_port and self.serial_port.is_open
            self.set_main_ui_enabled(is_connected)
            # Change button text back to "Flash Mode"
            self.flash_mode_btn.setText("Flash Mode")

    def browse_flash_file(self):
        """Open a file dialog to select a firmware file."""
        file_path, _ = QFileDialog.getOpenFileName(self, "Select Firmware File", "", "BIN Files (*.bin)")
        if file_path:
            self.flash_file_edit.setText(file_path)

    def on_flash_mode_changed(self):
        """Toggle visibility of separate files vs merged file widgets based on radio button selection."""
        if self.flash_mode_separate_radio.isChecked():
            self.separate_files_widget.show()
            self.merged_file_widget.hide()
        else:
            self.separate_files_widget.hide()
            self.merged_file_widget.show()

    def parse_esptool_output(self, line):
        """
        Parse esptool output line and update progress bar if progress information is found.
        Returns tuple: (has_progress, progress_percent, error_message)
        """
        line_lower = line.lower()
        
        # Check for error patterns (but not false positives like "failed to connect" during normal operation)
        error_keywords = ['error:', 'failed:', 'exception', 'traceback', 'fatal error']
        if any(keyword in line_lower for keyword in error_keywords):
            return (False, None, line)
        
        # Match progress patterns like "Writing at 0x00010000... (20 %)" or "(100 %)"
        # Find all progress percentages in the line and take the highest one
        progress_pattern = r'\((\d+)\s*%\)'
        matches = re.findall(progress_pattern, line)
        if matches:
            # Get the highest progress value (in case multiple percentages appear)
            progress = max(int(m) for m in matches)
            return (True, progress, None)
        
        # Check for completion indicators
        if 'done' in line_lower and ('leaving' in line_lower or 'hard resetting' in line_lower):
            return (True, 100, None)
        
        return (False, None, None)

    def reset_flash_progress(self):
        """Reset the flash progress bar to initial state."""
        self.flash_progress_bar.setValue(0)
        self.flash_progress_bar.setVisible(False)
        self.flash_progress_label.setText("")
        self.flash_progress_label.setVisible(False)
        self.flash_progress_label.setStyleSheet("")  # Reset style

    def update_flash_progress(self, percent, status_text=""):
        """Update the flash progress bar."""
        self.flash_progress_bar.setValue(percent)
        self.flash_progress_bar.setVisible(True)
        if status_text:
            self.flash_progress_label.setText(status_text)
            self.flash_progress_label.setVisible(True)
        else:
            self.flash_progress_label.setText(f"Flashing... {percent}%")
            self.flash_progress_label.setVisible(True)

    def flash_board(self):
        """Flash the selected firmware, bootloader, and partition table to the ESP32 board, or a merged .bin file."""
        # get actual device from combo data if available
        data = self.port_combo.currentData()
        port = data if data else self.port_combo.currentText().split()[0]
        chip = getattr(self, "selected_chip", "")

        # Validate inputs
        if not chip:
            self.flash_status.setText("Please select a chip type before flashing.")
            return
        if not port:
            self.flash_status.setText("Please select a serial port.")
            return

        # Check which mode is selected
        use_merged = self.flash_mode_merged_radio.isChecked()
        
        if use_merged:
            # Merged .bin file mode
            merged_file = self.merged_file_edit.text().strip()
            if not merged_file:
                self.flash_status.setText("Please select a merged .bin file.")
                return
            
            # Full merged images include their own internal offsets and start at flash address 0.
            flash_offset = "0x0"

            self.flash_status.setText(f"Flashing merged .bin ({chip})... Please wait.")
            self.flash_console.clear()
            self.reset_flash_progress()
            QApplication.setOverrideCursor(Qt.CursorShape.WaitCursor)
            error_message = None
            try:
                import subprocess
                cmd = [
                    sys.executable, "-m", "esptool", "--chip", chip, "--port", port, "write_flash",
                    flash_offset, merged_file
                ]
                self.flash_console.append(f"$ {' '.join(cmd)}\n")
                process = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
                for line in process.stdout:
                    self.flash_console.append(line.rstrip())
                    self.flash_console.ensureCursorVisible()
                    
                    # Parse output for progress and errors
                    has_progress, progress, error = self.parse_esptool_output(line)
                    if error:
                        error_message = error
                        self.flash_progress_label.setText(f"Error: {error}")
                        self.flash_progress_label.setStyleSheet("color: #ff4444;")
                    elif has_progress and progress is not None:
                        self.update_flash_progress(progress)
                    
                    QApplication.processEvents()  # Keep UI responsive
                process.wait()
                if process.returncode == 0:
                    self.flash_status.setText("Flashing successful!")
                    self.update_flash_progress(100, "Flashing completed successfully!")
                    self.flash_progress_label.setStyleSheet("color: #44bb44;")
                else:
                    self.flash_status.setText("Flashing failed. See console output.")
                    if error_message:
                        self.flash_progress_label.setText(f"Error: {error_message}")
                    else:
                        self.flash_progress_label.setText("Flashing failed. See console output.")
                    self.flash_progress_label.setStyleSheet("color: #ff4444;")
            except Exception as e:
                self.flash_status.setText(f"Error: {str(e)}")
                self.flash_console.append(f"Error: {str(e)}")
                self.flash_progress_label.setText(f"Error: {str(e)}")
                self.flash_progress_label.setStyleSheet("color: #ff4444;")
                self.flash_progress_label.setVisible(True)
            finally:
                QApplication.restoreOverrideCursor()
        else:
            # Separate files mode
            bootloader = self.bootloader_file_edit.text().strip()
            partition = self.partition_file_edit.text().strip()
            firmware = self.firmware_file_edit.text().strip()

            if not all([bootloader, partition, firmware]):
                self.flash_status.setText("Please select all .bin files.")
                return

            # Determine offsets based on chip type
            if chip in ["esp32s2", "esp32"]:
                boot_offset = "0x1000"
            elif chip in ["esp32s3", "esp32c3", "esp32c5", "esp32c6"]:
                boot_offset = "0x0"
            else:
                boot_offset = "0x1000"  # Default/fallback

            partition_offset = "0x8000"
            firmware_offset = "0x10000"

            self.flash_status.setText(f"Flashing ({chip})... Please wait.")
            self.flash_console.clear()
            self.reset_flash_progress()
            QApplication.setOverrideCursor(Qt.CursorShape.WaitCursor)
            error_message = None
            try:
                import subprocess
                cmd = [
                    sys.executable, "-m", "esptool", "--chip", chip, "--port", port, "write_flash",
                    boot_offset, bootloader,
                    partition_offset, partition,
                    firmware_offset, firmware
                ]
                self.flash_console.append(f"$ {' '.join(cmd)}\n")
                process = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
                for line in process.stdout:
                    self.flash_console.append(line.rstrip())
                    self.flash_console.ensureCursorVisible()
                    
                    # Parse output for progress and errors
                    has_progress, progress, error = self.parse_esptool_output(line)
                    if error:
                        error_message = error
                        self.flash_progress_label.setText(f"Error: {error}")
                        self.flash_progress_label.setStyleSheet("color: #ff4444;")
                    elif has_progress and progress is not None:
                        self.update_flash_progress(progress)
                    
                    QApplication.processEvents()  # Keep UI responsive
                process.wait()
                if process.returncode == 0:
                    self.flash_status.setText("Flashing successful!")
                    self.update_flash_progress(100, "Flashing completed successfully!")
                    self.flash_progress_label.setStyleSheet("color: #44bb44;")
                else:
                    self.flash_status.setText("Flashing failed. See console output.")
                    if error_message:
                        self.flash_progress_label.setText(f"Error: {error_message}")
                    else:
                        self.flash_progress_label.setText("Flashing failed. See console output.")
                    self.flash_progress_label.setStyleSheet("color: #ff4444;")
            except Exception as e:
                self.flash_status.setText(f"Error: {str(e)}")
                self.flash_console.append(f"Error: {str(e)}")
                self.flash_progress_label.setText(f"Error: {str(e)}")
                self.flash_progress_label.setStyleSheet("color: #ff4444;")
                self.flash_progress_label.setVisible(True)
            finally:
                QApplication.restoreOverrideCursor()

    def flash_custom_build(self):
        """Flash the .bin files built in ../../build to the ESP32 board."""
        import os
        from PyQt6.QtWidgets import QApplication

        # get actual device from combo data if available
        data = self.port_combo.currentData()
        port = data if data else self.port_combo.currentText().split()[0]
        chip = self.custom_chip_combo.currentText()
        build_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../build"))

        # Use fixed paths as specified
        bootloader = os.path.join(build_dir, "bootloader", "bootloader.bin")
        partition = os.path.join(build_dir, "partition_table", "partition-table.bin")
        firmware = os.path.join(build_dir, "Ghost_ESP_IDF.bin")

        # Show what was found
        self.flash_console.clear()
        self.flash_console.append(f"Using files from: {build_dir}")
        self.flash_console.append(f"bootloader: {bootloader if os.path.exists(bootloader) else 'NOT FOUND'}")
        self.flash_console.append(f"partition-table: {partition if os.path.exists(partition) else 'NOT FOUND'}")
        self.flash_console.append(f"firmware: {firmware if os.path.exists(firmware) else 'NOT FOUND'}")

        if not chip:
            self.custom_flash_status.setText("Please select a chip type before flashing.")
            return
        if not (os.path.exists(bootloader) and os.path.exists(partition) and os.path.exists(firmware) and port):
            self.custom_flash_status.setText("Missing required .bin files or serial port. See console for details.")
            return

        # Determine offsets based on chip type
        if chip in ["esp32s2", "esp32"]:
            boot_offset = "0x1000"
        elif chip in ["esp32s3", "esp32c3", "esp32c5", "esp32c6"]:
            boot_offset = "0x0"
        else:
            boot_offset = "0x1000"

        partition_offset = "0x8000"
        firmware_offset = "0x10000"

        self.custom_flash_status.setText(f"Flashing ({chip})... Please wait.")
        self.reset_flash_progress()
        QApplication.setOverrideCursor(Qt.CursorShape.WaitCursor)
        error_message = None
        try:
            import subprocess
            cmd = [
                sys.executable, "-m", "esptool", "--chip", chip, "--port", port, "write_flash",
                boot_offset, bootloader,
                partition_offset, partition,
                firmware_offset, firmware
            ]
            self.flash_console.append(f"$ {' '.join(cmd)}\n")
            process = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
            for line in process.stdout:
                self.flash_console.append(line.rstrip())
                self.flash_console.ensureCursorVisible()
                
                # Parse output for progress and errors
                has_progress, progress, error = self.parse_esptool_output(line)
                if error:
                    error_message = error
                    self.flash_progress_label.setText(f"Error: {error}")
                    self.flash_progress_label.setStyleSheet("color: #ff4444;")
                elif has_progress and progress is not None:
                    self.update_flash_progress(progress)
                
                QApplication.processEvents()
            process.wait()
            if process.returncode == 0:
                self.custom_flash_status.setText("Flashing successful!")
                self.update_flash_progress(100, "Flashing completed successfully!")
                self.flash_progress_label.setStyleSheet("color: #44bb44;")
            else:
                self.custom_flash_status.setText("Flashing failed. See console output.")
                if error_message:
                    self.flash_progress_label.setText(f"Error: {error_message}")
                else:
                    self.flash_progress_label.setText("Flashing failed. See console output.")
                self.flash_progress_label.setStyleSheet("color: #ff4444;")
        except Exception as e:
            self.custom_flash_status.setText(f"Error: {str(e)}")
            self.flash_console.append(f"Error: {str(e)}")
            self.flash_progress_label.setText(f"Error: {str(e)}")
            self.flash_progress_label.setStyleSheet("color: #ff4444;")
            self.flash_progress_label.setVisible(True)
        finally:
            QApplication.restoreOverrideCursor()

    def browse_zip_file(self, line_edit):
        """Open a file dialog to select a .zip file and set it in the given QLineEdit."""
        file_path, _ = QFileDialog.getOpenFileName(self, "Select Release Bundle ZIP", "", "ZIP Files (*.zip)")
        if file_path:
            line_edit.setText(file_path)

    def flash_release_bundle(self):
        """Extract the selected ZIP and flash the contained .bin files."""
        import zipfile
        import tempfile
        import os

        zip_path = self.release_zip_edit.text().strip()
        # get actual device from combo data if available
        data = self.port_combo.currentData()
        port = data if data else self.port_combo.currentText().split()[0]
        chip = getattr(self, "selected_chip", "")
        if not zip_path or not port or not chip:
            self.flash_bundle_status.setText("Please select a .zip file, chip type, and serial port.")
            self.flash_console.append("Please select a .zip file, chip type, and serial port.")
            return

        self.flash_bundle_status.setText("Extracting and flashing bundle...")
        self.flash_console.clear()
        self.reset_flash_progress()
        self.flash_console.append(f"Extracting {zip_path} ...")
        QApplication.setOverrideCursor(Qt.CursorShape.WaitCursor)
        try:
            with tempfile.TemporaryDirectory() as tmpdir:
                with zipfile.ZipFile(zip_path, 'r') as zip_ref:
                    zip_ref.extractall(tmpdir)
                self.flash_console.append(f"Extracted bundle to {tmpdir}")

                # Accept both partition-table.bin and partitions.bin
                bootloader = os.path.join(tmpdir, "bootloader.bin")
                partition_candidates = [
                    os.path.join(tmpdir, "partition-table.bin"),
                    os.path.join(tmpdir, "partitions.bin"),
                ]
                partition = next((p for p in partition_candidates if os.path.exists(p)), None)

                all_bins = glob.glob(os.path.join(tmpdir, "*.bin"))
                firmware = None
                for f in all_bins:
                    base = os.path.basename(f)
                    if base not in ("bootloader.bin", "partition-table.bin", "partitions.bin"):
                        firmware = f
                        break

                if not (os.path.exists(bootloader) and partition and firmware and os.path.exists(firmware)):
                    msg = (
                        "ZIP must contain bootloader.bin, partition-table.bin or partitions.bin, and a firmware .bin file.\n"
                        f"Checked for:\n"
                        f"  bootloader.bin: {'FOUND' if os.path.exists(bootloader) else 'MISSING'} ({bootloader})\n"
                        f"  partition-table.bin: {'FOUND' if os.path.exists(partition_candidates[0]) else 'MISSING'} ({partition_candidates[0]})\n"
                        f"  partitions.bin: {'FOUND' if os.path.exists(partition_candidates[1]) else 'MISSING'} ({partition_candidates[1]})\n"
                        f"  firmware .bin: {'FOUND' if firmware and os.path.exists(firmware) else 'MISSING'} ({firmware if firmware else 'None found'})\n"
                        f"All .bin files found in extracted folder:\n  " +
                        "\n  ".join(os.path.basename(f) for f in all_bins)
                    )
                    self.flash_bundle_status.setText("ZIP missing required files. See console for details.")
                    self.flash_console.append(msg)
                    return

                # Determine offsets based on chip type
                if chip in ["esp32s2", "esp32"]:
                    boot_offset = "0x1000"
                elif chip in ["esp32s3", "esp32c3", "esp32c5", "esp32c6"]:
                    boot_offset = "0x0"
                else:
                    boot_offset = "0x1000"  # Default/fallback

                partition_offset = "0x8000"
                firmware_offset = "0x10000"

                import subprocess
                cmd = [
                    sys.executable, "-m", "esptool", "--chip", chip, "--port", port, "write_flash",
                    boot_offset, bootloader,
                    partition_offset, partition,
                    firmware_offset, firmware
                ]
                self.flash_console.append(f"$ {' '.join(cmd)}\n")
                error_message = None
                process = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
                for line in process.stdout:
                    self.flash_console.append(line.rstrip())
                    self.flash_console.ensureCursorVisible()
                    
                    # Parse output for progress and errors
                    has_progress, progress, error = self.parse_esptool_output(line)
                    if error:
                        error_message = error
                        self.flash_progress_label.setText(f"Error: {error}")
                        self.flash_progress_label.setStyleSheet("color: #ff4444;")
                    elif has_progress and progress is not None:
                        self.update_flash_progress(progress)
                    
                    QApplication.processEvents()
                process.wait()
                if process.returncode == 0:
                    self.flash_bundle_status.setText("Flashing successful!")
                    self.flash_console.append("Flashing successful!")
                    self.update_flash_progress(100, "Flashing completed successfully!")
                    self.flash_progress_label.setStyleSheet("color: #44bb44;")
                else:
                    self.flash_bundle_status.setText("Flashing failed. See console output.")
                    self.flash_console.append("Flashing failed. See console output.")
                    if error_message:
                        self.flash_progress_label.setText(f"Error: {error_message}")
                    else:
                        self.flash_progress_label.setText("Flashing failed. See console output.")
                    self.flash_progress_label.setStyleSheet("color: #ff4444;")
        except Exception as e:
            self.flash_bundle_status.setText(f"Error: {str(e)}")
            self.flash_console.append(f"Error: {str(e)}")
        finally:
            QApplication.restoreOverrideCursor()

    def set_chip_type(self):
        """Set the chip type for flashing from either chip combo box."""
        # Prefer the one that is currently visible/enabled
        chip = ""
        if hasattr(self, "chip_combo") and self.chip_combo.isVisible():
            chip = self.chip_combo.currentText()
        if hasattr(self, "release_chip_combo") and self.release_chip_combo.isVisible():
            chip = self.release_chip_combo.currentText()
        self.selected_chip = chip
        # Optionally update a status label if you want, but no dialog needed
        # self.flash_status.setText(f"Chip set to: {chip}")

    def setup_connection_bar(self, main_layout):
        """
        Set up the serial connection bar at the top of the UI.

        Args:
            main_layout (QVBoxLayout): The main layout to add the connection bar to.
        """
        connection_group = QGroupBox("Serial Connection")
        connection_group.setObjectName("serial_connection_bar")
        connection_layout = QHBoxLayout(connection_group)

        self.port_combo = QComboBox()
        self.port_combo.setMinimumWidth(150)
        self.port_combo.setToolTip("Select the serial port for ESP32")
        self.port_combo.currentTextChanged.connect(self.on_port_changed)
        connection_layout.addWidget(QLabel("Port:"))
        connection_layout.addWidget(self.port_combo)

        # --- Refresh Ports Button with Icon, placed directly next to port_combo ---
        refresh_btn = QPushButton()
        refresh_btn.setIcon(self.style().standardIcon(QStyle.StandardPixmap.SP_BrowserReload))
        refresh_btn.setToolTip("Refresh the list of available serial ports")
        refresh_btn.setFixedWidth(32)
        refresh_btn.clicked.connect(self.refresh_ports)
        connection_layout.addWidget(refresh_btn)
        self.refresh_btn = refresh_btn  # Save reference if you want to disable in flash mode

        self.status_indicator = QLabel()
        self.status_indicator.setFixedSize(18, 18)
        self.status_indicator.setStyleSheet("""
            background-color: #ff4444;
            border-radius: 9px;
            border: 1px solid #222;
        """)
        self.status_indicator.setToolTip("Shows current connection status")
        connection_layout.addWidget(self.status_indicator)

        self.connect_btn = QPushButton("Connect")
        self.connect_btn.clicked.connect(self.toggle_connection)
        self.connect_btn.setFixedWidth(100)
        self.connect_btn.setToolTip("Connect or disconnect from ESP32")
        connection_layout.addWidget(self.connect_btn)

        self.auto_reconnect_checkbox = QCheckBox("Auto Reconnect")
        self.auto_reconnect_checkbox.setChecked(False)
        self.auto_reconnect_checkbox.setToolTip("Automatically reconnect if connection is lost")
        connection_layout.addWidget(self.auto_reconnect_checkbox)

        # --- Add stretch here to push Flash Mode button to the right ---
        connection_layout.addStretch()

        # --- Flash Mode Button (right justified) ---
        self.flash_mode_btn = QPushButton("Flash Mode")
        self.flash_mode_btn.setCheckable(True)
        self.flash_mode_btn.setFixedWidth(130)  # Increased to fit "Command Mode" text
        self.flash_mode_btn.setToolTip("Toggle Flash Mode to flash your board")
        self.flash_mode_btn.clicked.connect(self.toggle_flash_mode)
        connection_layout.addWidget(self.flash_mode_btn)
        # --- End Flash Mode Button ---

        # Set the size policy to not expand vertically
        connection_group.setSizePolicy(QSizePolicy.Policy.Preferred, QSizePolicy.Policy.Fixed)
        main_layout.addWidget(connection_group)

    def on_port_changed(self, new_port):
        """
        Handle port combo box selection change.
        Auto-disconnect if currently connected to prevent connection issues.
        
        Args:
            new_port (str): The newly selected port.
        """
        if self.serial_port and self.serial_port.is_open:
            log_message(self.log_text, f"Port changed to {new_port}, disconnecting from current port", self.app_settings.get("show_timestamps", True))
            self.disconnect()

    def update_connection_status(self, connected):
        """
        Update the connection status indicator.

        Args:
            connected (bool): True if connected, False otherwise.
        """
        if connected:
            self.status_indicator.setStyleSheet("""
                background-color: #44bb44;
                border-radius: 9px;
                border: 1px solid #222;
            """)
        else:
            self.status_indicator.setStyleSheet("""
                background-color: #ff4444;
                border-radius: 9px;
                border: 1px solid #222;
            """)

    def setup_command_panels(self, layout):
        """
        Set up the command panel tabs and associated panels.

        Args:
            layout (QVBoxLayout): The layout to add the tabs to.
        """
        # Create tab widget for panel selection
        self.panel_tabs = QTabWidget()
        self.panel_tabs.setTabPosition(QTabWidget.TabPosition.North)
        
        # Create each panel widget and add as tabs
        self.panel_tabs.addTab(self.create_wifi_tab(), "WiFi Operations")
        self.panel_tabs.addTab(self.create_network_tab(), "Network Operations")
        self.panel_tabs.addTab(self.create_ble_tab(), "BLE Operations")
        self.panel_tabs.addTab(self.create_evil_portal_tab(), "Evil Portal")
        self.panel_tabs.addTab(self.create_settings_tab(), "Settings")
        
        layout.addWidget(self.panel_tabs)
        
        # Store reference to tab widget as panel_container for compatibility
        self.panel_container = self.panel_tabs

    def setup_command_tabs(self, layout):
        """
        Set up the command tabs (panels).

        Args:
            layout (QVBoxLayout): The layout to add the tabs to.
        """
        self.setup_command_panels(layout)

    def create_wifi_tab(self):
        """Create and return the WiFi operations tab widget with subtabs."""
        wifi_tabs = QTabWidget()
        wifi_tabs.setTabPosition(QTabWidget.TabPosition.North)
        
        # Scanning subtab
        scanning_widget = QWidget()
        scanning_layout = QGridLayout(scanning_widget)
        self.create_command_group("WiFi Scanning", [
            ("Scan Access Points", "scanap"),
            ("Scan Stations", "scansta"),
            ("Stop Scan", "stopscan"),
            ("List APs", "list -a"),
            ("List Stations", "list -s")
        ], scanning_layout, 0, 0)
        
        # Probe Request Listener
        probe_group = QGroupBox("Probe Request Listener")
        probe_layout = QHBoxLayout(probe_group)
        self.probe_channel = QLineEdit()
        self.probe_channel.setPlaceholderText("Channel (optional)")
        probe_layout.addWidget(self.probe_channel)
        start_probe_btn = QPushButton("Start Listening")
        start_probe_btn.clicked.connect(self.start_probe_listener)
        probe_layout.addWidget(start_probe_btn)
        stop_probe_btn = QPushButton("Stop Listening")
        stop_probe_btn.clicked.connect(lambda: self.send_command("listenprobes stop"))
        probe_layout.addWidget(stop_probe_btn)
        scanning_layout.addWidget(probe_group, 0, 1)
        scanning_layout.setColumnStretch(0, 1)
        scanning_layout.setColumnStretch(1, 1)
        wifi_tabs.addTab(scanning_widget, "Scanning")
        
        # Attacks subtab
        attacks_widget = QWidget()
        attacks_layout = QGridLayout(attacks_widget)
        self.create_command_group("Attack Operations", [
            ("Start Deauth", "attack -d"),
            ("Stop Deauth", "stopdeauth"),
            ("Select AP", lambda: show_select_ap_dialog(self))
        ], attacks_layout, 0, 0)
        attacks_layout.setColumnStretch(0, 1)
        wifi_tabs.addTab(attacks_widget, "Attacks")
        
        # Beacons subtab
        beacons_widget = QWidget()
        beacons_layout = QGridLayout(beacons_widget)
        self.create_command_group("Beacon Operations", [
            ("Random Beacon Spam", "beaconspam -r"),
            ("Rickroll Beacon", "beaconspam -rr"),
            ("AP List Beacon", "beaconspam -l"),
            ("Custom SSID Beacon", lambda: show_custom_beacon_dialog(self)),
            ("Stop Spam", "stopspam")
        ], beacons_layout, 0, 0)
        self.create_command_group("Beacon List Management", [
            ("Add SSID to List", self.show_beacon_add_dialog),
            ("Remove SSID from List", self.show_beacon_remove_dialog),
            ("Clear Beacon List", "beaconclear"),
            ("Show Beacon List", "beaconshow"),
            ("Spam Beacon List", "beaconspamlist"),
        ], beacons_layout, 0, 1)
        beacons_layout.setColumnStretch(0, 1)
        beacons_layout.setColumnStretch(1, 1)
        wifi_tabs.addTab(beacons_widget, "Beacons")
        
        # Capture subtab
        capture_widget = QWidget()
        capture_layout = QVBoxLayout(capture_widget)
        capture_group = QGroupBox("Capture Operations")
        capture_group_layout = QVBoxLayout(capture_group)
        self.capture_type_combo = QComboBox()
        self.capture_type_combo.addItems([
            "Probes",
            "Beacons",
            "Deauth",
            "Raw",
            "WPS",
            "Pwnagotchi"
        ])
        self.capture_type_combo.setMinimumHeight(32)
        capture_group_layout.addWidget(QLabel("Capture Type:"))
        capture_group_layout.addWidget(self.capture_type_combo)
        button_layout = QHBoxLayout()
        start_btn = QPushButton("Start Capture")
        stop_btn = QPushButton("Stop Capture")
        start_btn.setMinimumHeight(32)
        stop_btn.setMinimumHeight(32)
        button_layout.addWidget(start_btn)
        button_layout.addWidget(stop_btn)
        capture_group_layout.addLayout(button_layout)
        capture_group_layout.addStretch()
        start_btn.clicked.connect(self.start_capture)
        stop_btn.clicked.connect(lambda: self.send_command("capture -stop"))
        capture_layout.addWidget(capture_group)
        capture_layout.addStretch()
        wifi_tabs.addTab(capture_widget, "Capture")
        
        return wifi_tabs

    def create_network_tab(self):
        """Create and return the Network operations tab widget with subtabs."""
        network_tabs = QTabWidget()
        network_tabs.setTabPosition(QTabWidget.TabPosition.North)
        
        # WiFi Connection subtab
        wifi_connect_widget = QWidget()
        wifi_connect_layout = QVBoxLayout(wifi_connect_widget)
        wifi_connect_group = QGroupBox("WiFi Connection")
        wifi_connect_form = QFormLayout(wifi_connect_group)
        self.wifi_ssid = QLineEdit()
        self.wifi_password = QLineEdit()
        self.wifi_password.setEchoMode(QLineEdit.EchoMode.Password)
        wifi_connect_form.addRow("SSID:", self.wifi_ssid)
        wifi_connect_form.addRow("Password:", self.wifi_password)
        connect_btn = QPushButton("Connect to Network")
        connect_btn.clicked.connect(self.connect_to_wifi)
        wifi_connect_form.addRow(connect_btn)
        wifi_connect_layout.addWidget(wifi_connect_group)
        wifi_connect_layout.addStretch()
        network_tabs.addTab(wifi_connect_widget, "WiFi Connection")
        
        # Network Tools subtab
        network_tools_widget = QWidget()
        network_tools_layout = QGridLayout(network_tools_widget)
        self.create_command_group("Network Tools", [
            ("Cast Random YouTube Video", "dialconnect"),
            ("Print to Network Printer", lambda: show_printer_dialog(self))
        ], network_tools_layout, 0, 0)
        network_tools_layout.setColumnStretch(0, 1)
        network_tabs.addTab(network_tools_widget, "Network Tools")
        
        # Port Scanner subtab
        portscan_widget = QWidget()
        portscan_layout = QVBoxLayout(portscan_widget)
        portscan_group = QGroupBox("Port Scanner")
        portscan_form = QFormLayout(portscan_group)
        self.portscan_ip = QLineEdit()
        self.portscan_args = QLineEdit()
        portscan_form.addRow("Target IP (or 'local'):", self.portscan_ip)
        portscan_form.addRow("Args (-C, -A, or range):", self.portscan_args)
        scan_btn = QPushButton("Scan Ports")
        scan_btn.clicked.connect(self.run_port_scan)
        portscan_form.addRow(scan_btn)
        portscan_layout.addWidget(portscan_group)
        portscan_layout.addStretch()
        network_tabs.addTab(portscan_widget, "Port Scanner")
        
        return network_tabs

    def create_ble_tab(self):
        """Create and return the BLE operations tab widget with subtabs."""
        ble_tabs = QTabWidget()
        ble_tabs.setTabPosition(QTabWidget.TabPosition.North)
        
        # BLE Scanning subtab
        ble_scanning_widget = QWidget()
        ble_scanning_layout = QGridLayout(ble_scanning_widget)
        self.create_command_group("BLE Scanning", [
            ("Find Flippers", "blescan -f"),
            ("BLE Spam Detector", "blescan -ds"),
            ("AirTag Scanner", "blescan -a"),
            ("Raw BLE Scan", "blescan -r"),
            ("Stop BLE Scan", "blescan -s")
        ], ble_scanning_layout, 0, 0)
        ble_scanning_layout.setColumnStretch(0, 1)
        ble_tabs.addTab(ble_scanning_widget, "Scanning")
        
        return ble_tabs

    def create_capture_tab(self):
        """Create and return the Capture operations tab widget as a dropdown with Start/Stop buttons."""
        capture_widget = QWidget()
        layout = QVBoxLayout(capture_widget)

        # Dropdown for capture type
        self.capture_type_combo = QComboBox()
        self.capture_type_combo.addItems([
            "Probes",
            "Beacons",
            "Deauth",
            "Raw",
            "WPS",
            "Pwnagotchi"
        ])
        layout.addWidget(QLabel("Capture Type:"))
        layout.addWidget(self.capture_type_combo)

        # Start/Stop buttons
        button_layout = QHBoxLayout()
        start_btn = QPushButton("Start Capture")
        stop_btn = QPushButton("Stop Capture")
        button_layout.addWidget(start_btn)
        button_layout.addWidget(stop_btn)
        layout.addLayout(button_layout)

        # Connect buttons
        start_btn.clicked.connect(self.start_capture)
        stop_btn.clicked.connect(lambda: self.send_command("capture -stop"))

        return capture_widget

    def create_evil_portal_tab(self):
        """Create and return the Evil Portal tab widget."""
        portal_widget = QWidget()
        portal_layout = QFormLayout(portal_widget)

        # Portal Settings with default values
        self.portal_ssid = QLineEdit("FreeWiFi")
        self.portal_password = QLineEdit("password123")
        portal_layout.addRow("Portal SSID:", self.portal_ssid)
        portal_layout.addRow("Portal Password:", self.portal_password)

        # --- Available Portals Dropdown + Refresh Button ---
        dropdown_layout = QHBoxLayout()
        self.portal_dropdown = QComboBox()
        self.portal_dropdown.addItem("default")
        dropdown_layout.addWidget(self.portal_dropdown)

        list_portals_btn = QPushButton()
        list_portals_btn.setToolTip("Refresh Portal List")
        list_portals_btn.setIcon(self.style().standardIcon(QStyle.StandardPixmap.SP_BrowserReload))
        list_portals_btn.setFixedSize(28, 28)
        list_portals_btn.clicked.connect(lambda: self.send_command("listportals"))
        dropdown_layout.addWidget(list_portals_btn)

        portal_layout.addRow("Available Portals:", dropdown_layout)

        # --- Upload indicator ---
        self.portal_upload_indicator = QLabel("")
        portal_layout.addRow(self.portal_upload_indicator)

        # --- Progress Bar ---
        self.portal_progress_bar = QProgressBar()
        self.portal_progress_bar.setMinimum(0)
        self.portal_progress_bar.setMaximum(100)
        self.portal_progress_bar.setValue(0)
        self.portal_progress_bar.setVisible(False)
        portal_layout.addRow(self.portal_progress_bar)
        # --- End Progress Bar ---

        # --- File selection button ---
        file_btn = QPushButton("Send Local HTML as Portal")
        file_btn.clicked.connect(self.send_local_portal_file)
        portal_layout.addRow(file_btn)
        # --- End File selection button ---

        # Control buttons
        button_layout = QHBoxLayout()
        start_portal_btn = QPushButton("Start Portal")
        start_portal_btn.clicked.connect(self.start_evil_portal)
        stop_portal_btn = QPushButton("Stop Portal")
        stop_portal_btn.clicked.connect(lambda: self.send_command("stopportal"))
        button_layout.addWidget(start_portal_btn)
        button_layout.addWidget(stop_portal_btn)
        portal_layout.addRow(button_layout)

        return portal_widget

    def create_settings_tab(self):
        """Create and return the Settings tab widget with subtabs."""
        settings_tabs = QTabWidget()
        settings_tabs.setTabPosition(QTabWidget.TabPosition.North)
        
        # Display Settings subtab
        display_widget = QWidget()
        display_layout = QVBoxLayout(display_widget)
        display_group = QGroupBox("Display Settings")
        display_form = QFormLayout(display_group)

        rgb_mode = QComboBox()
        rgb_mode.addItems(["Normal", "Rainbow", "Stealth"])
        rgb_mode.currentIndexChanged.connect(
            lambda i: self.send_command(f"setrgbmode {rgb_mode.currentText().lower()}")
        )
        display_form.addRow("RGB Mode:", rgb_mode)

        timeout = QComboBox()
        timeout.addItems(["5s", "10s", "30s", "60s", "Never"])
        timeout.currentIndexChanged.connect(
            lambda i: self.send_command(f"settimeout {i}")
        )
        display_form.addRow("Display Timeout:", timeout)

        theme = QComboBox()
        theme.addItems([
            "Default", "Pastel", "Dark", "Bright", "Solarized", "Monochrome",
            "Rose Red", "Purple", "Blue", "Orange", "Neon", "Cyberpunk",
            "Ocean", "Sunset", "Forest"
        ])
        theme.currentIndexChanged.connect(
            lambda i: self.send_command(f"settheme {i}")
        )
        display_form.addRow("Menu Theme:", theme)

        term_color = QComboBox()
        term_color.addItems(["Green", "White", "Red", "Blue", "Yellow", "Cyan", "Magenta", "Orange"])
        term_color.currentIndexChanged.connect(
            lambda i: self.send_command(f"settermcolor {i}")
        )
        display_form.addRow("Terminal Color:", term_color)

        invert_colors = QComboBox()
        invert_colors.addItems(["Off", "On"])
        invert_colors.currentIndexChanged.connect(
            lambda i: self.send_command(f"setinvert {'on' if i else 'off'}")
        )
        display_form.addRow("Invert Colors:", invert_colors)

        max_brightness = QComboBox()
        max_brightness.addItems(["10%", "20%", "30%", "40%", "50%", "60%", "70%", "80%", "90%", "100%"])
        max_brightness.currentIndexChanged.connect(
            lambda i: self.send_command(f"setbrightness {(i+1)*10}")
        )
        display_form.addRow("Max Brightness:", max_brightness)

        display_layout.addWidget(display_group)
        display_layout.addStretch()
        settings_tabs.addTab(display_widget, "Display")

        # RGB Settings subtab
        rgb_widget = QWidget()
        rgb_layout = QVBoxLayout(rgb_widget)
        rgb_group = QGroupBox("RGB/LED Settings")
        rgb_form = QFormLayout(rgb_group)

        rgb_speed = QSpinBox()
        rgb_speed.setRange(0, 255)
        rgb_speed.setValue(128)
        rgb_speed.valueChanged.connect(
            lambda v: self.send_command(f"settings set rgb_speed {v}")
        )
        rgb_form.addRow("RGB Speed (0-255):", rgb_speed)

        rgb_data_pin = QSpinBox()
        rgb_data_pin.setRange(-1, 48)
        rgb_data_pin.setValue(-1)
        rgb_data_pin.setSpecialValueText("Not used")
        rgb_data_pin.valueChanged.connect(
            lambda v: self.send_command(f"settings set rgb_data_pin {v}")
        )
        rgb_form.addRow("RGB Data Pin:", rgb_data_pin)

        rgb_red_pin = QSpinBox()
        rgb_red_pin.setRange(-1, 48)
        rgb_red_pin.setValue(-1)
        rgb_red_pin.setSpecialValueText("Not used")
        rgb_red_pin.valueChanged.connect(
            lambda v: self.send_command(f"settings set rgb_red_pin {v}")
        )
        rgb_form.addRow("RGB Red Pin:", rgb_red_pin)

        rgb_green_pin = QSpinBox()
        rgb_green_pin.setRange(-1, 48)
        rgb_green_pin.setValue(-1)
        rgb_green_pin.setSpecialValueText("Not used")
        rgb_green_pin.valueChanged.connect(
            lambda v: self.send_command(f"settings set rgb_green_pin {v}")
        )
        rgb_form.addRow("RGB Green Pin:", rgb_green_pin)

        rgb_blue_pin = QSpinBox()
        rgb_blue_pin.setRange(-1, 48)
        rgb_blue_pin.setValue(-1)
        rgb_blue_pin.setSpecialValueText("Not used")
        rgb_blue_pin.valueChanged.connect(
            lambda v: self.send_command(f"settings set rgb_blue_pin {v}")
        )
        rgb_form.addRow("RGB Blue Pin:", rgb_blue_pin)

        neopixel_bright = QSpinBox()
        neopixel_bright.setRange(0, 100)
        neopixel_bright.setValue(50)
        neopixel_bright.setSuffix("%")
        neopixel_bright.valueChanged.connect(
            lambda v: self.send_command(f"settings set neopixel_bright {v}")
        )
        rgb_form.addRow("Neopixel Brightness:", neopixel_bright)

        rgb_layout.addWidget(rgb_group)
        rgb_layout.addStretch()
        settings_tabs.addTab(rgb_widget, "RGB/LED")

        # Network Settings subtab
        network_widget = QWidget()
        network_layout = QVBoxLayout(network_widget)
        network_group = QGroupBox("Network Settings")
        network_form = QFormLayout(network_group)

        web_auth = QComboBox()
        web_auth.addItems(["Off", "On"])
        web_auth.currentIndexChanged.connect(
            lambda i: self.send_command(f"webauth {'on' if i else 'off'}")
        )
        network_form.addRow("Web Auth:", web_auth)

        ap_enabled = QComboBox()
        ap_enabled.addItems(["Off", "On"])
        ap_enabled.currentIndexChanged.connect(
            lambda i: self.send_command(f"apenable {'on' if i else 'off'}")
        )
        network_form.addRow("AP Enabled:", ap_enabled)

        ap_ssid = QLineEdit()
        ap_ssid.setPlaceholderText("Enter AP SSID")
        ap_ssid.returnPressed.connect(
            lambda: self.send_command(f"settings set ap_ssid {ap_ssid.text()}")
        )
        ap_ssid_btn = QPushButton("Set AP SSID")
        ap_ssid_btn.clicked.connect(
            lambda: self.send_command(f"settings set ap_ssid {ap_ssid.text()}")
        )
        ap_ssid_layout = QHBoxLayout()
        ap_ssid_layout.addWidget(ap_ssid)
        ap_ssid_layout.addWidget(ap_ssid_btn)
        network_form.addRow("AP SSID:", ap_ssid_layout)

        ap_password = QLineEdit()
        ap_password.setEchoMode(QLineEdit.EchoMode.Password)
        ap_password.setPlaceholderText("Enter AP password")
        ap_password.returnPressed.connect(
            lambda: self.send_command(f"settings set ap_password {ap_password.text()}")
        )
        ap_password_btn = QPushButton("Set AP Password")
        ap_password_btn.clicked.connect(
            lambda: self.send_command(f"settings set ap_password {ap_password.text()}")
        )
        ap_password_layout = QHBoxLayout()
        ap_password_layout.addWidget(ap_password)
        ap_password_layout.addWidget(ap_password_btn)
        network_form.addRow("AP Password:", ap_password_layout)

        sta_ssid = QLineEdit()
        sta_ssid.setPlaceholderText("Enter Station SSID")
        sta_ssid.returnPressed.connect(
            lambda: self.send_command(f"settings set sta_ssid {sta_ssid.text()}")
        )
        sta_ssid_btn = QPushButton("Set Station SSID")
        sta_ssid_btn.clicked.connect(
            lambda: self.send_command(f"settings set sta_ssid {sta_ssid.text()}")
        )
        sta_ssid_layout = QHBoxLayout()
        sta_ssid_layout.addWidget(sta_ssid)
        sta_ssid_layout.addWidget(sta_ssid_btn)
        network_form.addRow("Station SSID:", sta_ssid_layout)

        sta_password = QLineEdit()
        sta_password.setEchoMode(QLineEdit.EchoMode.Password)
        sta_password.setPlaceholderText("Enter Station password")
        sta_password.returnPressed.connect(
            lambda: self.send_command(f"settings set sta_password {sta_password.text()}")
        )
        sta_password_btn = QPushButton("Set Station Password")
        sta_password_btn.clicked.connect(
            lambda: self.send_command(f"settings set sta_password {sta_password.text()}")
        )
        sta_password_layout = QHBoxLayout()
        sta_password_layout.addWidget(sta_password)
        sta_password_layout.addWidget(sta_password_btn)
        network_form.addRow("Station Password:", sta_password_layout)

        network_layout.addWidget(network_group)
        network_layout.addStretch()
        settings_tabs.addTab(network_widget, "Network")

        # Evil Portal Settings subtab
        portal_settings_widget = QWidget()
        portal_settings_layout = QVBoxLayout(portal_settings_widget)
        portal_settings_group = QGroupBox("Evil Portal Settings")
        portal_settings_form = QFormLayout(portal_settings_group)

        portal_url = QLineEdit()
        portal_url.setPlaceholderText("Enter portal URL or file path")
        portal_url.returnPressed.connect(
            lambda: self.send_command(f"settings set portal_url {portal_url.text()}")
        )
        portal_url_btn = QPushButton("Set")
        portal_url_btn.clicked.connect(
            lambda: self.send_command(f"settings set portal_url {portal_url.text()}")
        )
        portal_url_layout = QHBoxLayout()
        portal_url_layout.addWidget(portal_url)
        portal_url_layout.addWidget(portal_url_btn)
        portal_settings_form.addRow("Portal URL:", portal_url_layout)

        portal_ssid_setting = QLineEdit()
        portal_ssid_setting.setPlaceholderText("Enter portal SSID")
        portal_ssid_setting.returnPressed.connect(
            lambda: self.send_command(f"settings set portal_ssid {portal_ssid_setting.text()}")
        )
        portal_ssid_setting_btn = QPushButton("Set")
        portal_ssid_setting_btn.clicked.connect(
            lambda: self.send_command(f"settings set portal_ssid {portal_ssid_setting.text()}")
        )
        portal_ssid_setting_layout = QHBoxLayout()
        portal_ssid_setting_layout.addWidget(portal_ssid_setting)
        portal_ssid_setting_layout.addWidget(portal_ssid_setting_btn)
        portal_settings_form.addRow("Portal SSID:", portal_ssid_setting_layout)

        portal_password_setting = QLineEdit()
        portal_password_setting.setEchoMode(QLineEdit.EchoMode.Password)
        portal_password_setting.setPlaceholderText("Enter portal password")
        portal_password_setting.returnPressed.connect(
            lambda: self.send_command(f"settings set portal_password {portal_password_setting.text()}")
        )
        portal_password_setting_btn = QPushButton("Set")
        portal_password_setting_btn.clicked.connect(
            lambda: self.send_command(f"settings set portal_password {portal_password_setting.text()}")
        )
        portal_password_setting_layout = QHBoxLayout()
        portal_password_setting_layout.addWidget(portal_password_setting)
        portal_password_setting_layout.addWidget(portal_password_setting_btn)
        portal_settings_form.addRow("Portal Password:", portal_password_setting_layout)

        portal_ap_ssid = QLineEdit()
        portal_ap_ssid.setPlaceholderText("Enter portal AP SSID")
        portal_ap_ssid.returnPressed.connect(
            lambda: self.send_command(f"settings set portal_ap_ssid {portal_ap_ssid.text()}")
        )
        portal_ap_ssid_btn = QPushButton("Set")
        portal_ap_ssid_btn.clicked.connect(
            lambda: self.send_command(f"settings set portal_ap_ssid {portal_ap_ssid.text()}")
        )
        portal_ap_ssid_layout = QHBoxLayout()
        portal_ap_ssid_layout.addWidget(portal_ap_ssid)
        portal_ap_ssid_layout.addWidget(portal_ap_ssid_btn)
        portal_settings_form.addRow("Portal AP SSID:", portal_ap_ssid_layout)

        portal_domain = QLineEdit()
        portal_domain.setPlaceholderText("Enter portal domain")
        portal_domain.returnPressed.connect(
            lambda: self.send_command(f"settings set portal_domain {portal_domain.text()}")
        )
        portal_domain_btn = QPushButton("Set")
        portal_domain_btn.clicked.connect(
            lambda: self.send_command(f"settings set portal_domain {portal_domain.text()}")
        )
        portal_domain_layout = QHBoxLayout()
        portal_domain_layout.addWidget(portal_domain)
        portal_domain_layout.addWidget(portal_domain_btn)
        portal_settings_form.addRow("Portal Domain:", portal_domain_layout)

        portal_offline = QComboBox()
        portal_offline.addItems(["Off", "On"])
        portal_offline.currentIndexChanged.connect(
            lambda i: self.send_command(f"settings set portal_offline {'true' if i else 'false'}")
        )
        portal_settings_form.addRow("Portal Offline Mode:", portal_offline)

        portal_settings_layout.addWidget(portal_settings_group)
        portal_settings_layout.addStretch()
        settings_tabs.addTab(portal_settings_widget, "Evil Portal")

        # Printer Settings subtab
        printer_widget = QWidget()
        printer_layout = QVBoxLayout(printer_widget)
        printer_group = QGroupBox("Printer Settings")
        printer_form = QFormLayout(printer_group)

        printer_ip = QLineEdit()
        printer_ip.setPlaceholderText("Enter printer IP address")
        printer_ip.returnPressed.connect(
            lambda: self.send_command(f"settings set printer_ip {printer_ip.text()}")
        )
        printer_ip_btn = QPushButton("Set")
        printer_ip_btn.clicked.connect(
            lambda: self.send_command(f"settings set printer_ip {printer_ip.text()}")
        )
        printer_ip_layout = QHBoxLayout()
        printer_ip_layout.addWidget(printer_ip)
        printer_ip_layout.addWidget(printer_ip_btn)
        printer_form.addRow("Printer IP:", printer_ip_layout)

        printer_text = QLineEdit()
        printer_text.setPlaceholderText("Enter printer text")
        printer_text.returnPressed.connect(
            lambda: self.send_command(f"settings set printer_text {printer_text.text()}")
        )
        printer_text_btn = QPushButton("Set")
        printer_text_btn.clicked.connect(
            lambda: self.send_command(f"settings set printer_text {printer_text.text()}")
        )
        printer_text_layout = QHBoxLayout()
        printer_text_layout.addWidget(printer_text)
        printer_text_layout.addWidget(printer_text_btn)
        printer_form.addRow("Printer Text:", printer_text_layout)

        printer_font_size = QSpinBox()
        printer_font_size.setRange(1, 100)
        printer_font_size.setValue(12)
        printer_font_size.valueChanged.connect(
            lambda v: self.send_command(f"settings set printer_font_size {v}")
        )
        printer_form.addRow("Printer Font Size:", printer_font_size)

        printer_alignment = QComboBox()
        printer_alignment.addItems(["0", "1", "2", "3", "4"])
        printer_alignment.currentIndexChanged.connect(
            lambda i: self.send_command(f"settings set printer_alignment {i}")
        )
        printer_form.addRow("Printer Alignment:", printer_alignment)

        printer_layout.addWidget(printer_group)
        printer_layout.addStretch()
        settings_tabs.addTab(printer_widget, "Printer")

        # System Settings subtab
        system_widget = QWidget()
        system_layout = QVBoxLayout(system_widget)
        system_group = QGroupBox("System Settings")
        system_form = QFormLayout(system_group)

        thirds_control = QComboBox()
        thirds_control.addItems(["Off", "On"])
        thirds_control.currentIndexChanged.connect(
            lambda i: self.send_command(f"setthirdcontrol {'on' if i else 'off'}")
        )
        system_form.addRow("Third Control:", thirds_control)

        power_save = QComboBox()
        power_save.addItems(["Off", "On"])
        power_save.currentIndexChanged.connect(
            lambda i: self.send_command(f"setpowersave {'on' if i else 'off'}")
        )
        system_form.addRow("Power Saving Mode:", power_save)

        channel_delay = QSpinBox()
        channel_delay.setRange(0, 10000)
        channel_delay.setValue(100)
        channel_delay.setSuffix(" ms")
        channel_delay.valueChanged.connect(
            lambda v: self.send_command(f"settings set channel_delay {v}")
        )
        system_form.addRow("Channel Delay:", channel_delay)

        broadcast_speed = QSpinBox()
        broadcast_speed.setRange(1, 1000)
        broadcast_speed.setValue(1)
        broadcast_speed.valueChanged.connect(
            lambda v: self.send_command(f"settings set broadcast_speed {v}")
        )
        system_form.addRow("Broadcast Speed:", broadcast_speed)

        gps_rx_pin = QSpinBox()
        gps_rx_pin.setRange(-1, 48)
        gps_rx_pin.setValue(-1)
        gps_rx_pin.setSpecialValueText("Not used")
        gps_rx_pin.valueChanged.connect(
            lambda v: self.send_command(f"settings set gps_rx_pin {v}")
        )
        system_form.addRow("GPS RX Pin:", gps_rx_pin)

        zebra_menus = QComboBox()
        zebra_menus.addItems(["Off", "On"])
        zebra_menus.currentIndexChanged.connect(
            lambda i: self.send_command(f"settings set zebra_menus {'true' if i else 'false'}")
        )
        system_form.addRow("Zebra Menus:", zebra_menus)

        nav_buttons = QComboBox()
        nav_buttons.addItems(["Off", "On"])
        nav_buttons.currentIndexChanged.connect(
            lambda i: self.send_command(f"settings set nav_buttons {'true' if i else 'false'}")
        )
        system_form.addRow("Navigation Buttons:", nav_buttons)

        menu_layout = QComboBox()
        menu_layout.addItems(["Carousel", "Grid", "List"])
        menu_layout.currentIndexChanged.connect(
            lambda i: self.send_command(f"settings set menu_layout {i}")
        )
        system_form.addRow("Menu Layout:", menu_layout)

        infrared_easy = QComboBox()
        infrared_easy.addItems(["Off", "On"])
        infrared_easy.currentIndexChanged.connect(
            lambda i: self.send_command(f"settings set infrared_easy {'true' if i else 'false'}")
        )
        system_form.addRow("Infrared Easy Mode:", infrared_easy)

        rts_enabled = QComboBox()
        rts_enabled.addItems(["Off", "On"])
        rts_enabled.currentIndexChanged.connect(
            lambda i: self.send_command(f"settings set rts_enabled {'true' if i else 'false'}")
        )
        system_form.addRow("RTS Enabled:", rts_enabled)

        # Reboot and Save buttons side by side
        button_layout = QHBoxLayout()
        reboot_btn = QPushButton("Reboot")
        reboot_btn.clicked.connect(lambda: self.send_command("reboot"))
        button_layout.addWidget(reboot_btn)

        save_btn = QPushButton("Save Settings")
        save_btn.clicked.connect(lambda: self.send_command("savesetting"))
        button_layout.addWidget(save_btn)

        system_form.addRow(button_layout)

        system_layout.addWidget(system_group)
        system_layout.addStretch()
        settings_tabs.addTab(system_widget, "System")

        # Custom Settings subtab
        custom_widget = QWidget()
        custom_layout = QVBoxLayout(custom_widget)
        custom_group = QGroupBox("Custom Settings")
        custom_form = QFormLayout(custom_group)

        flappy_name = QLineEdit()
        flappy_name.setPlaceholderText("Enter Flappy Ghost name")
        flappy_name.returnPressed.connect(
            lambda: self.send_command(f"settings set flappy_name {flappy_name.text()}")
        )
        flappy_name_btn = QPushButton("Set")
        flappy_name_btn.clicked.connect(
            lambda: self.send_command(f"settings set flappy_name {flappy_name.text()}")
        )
        flappy_name_layout = QHBoxLayout()
        flappy_name_layout.addWidget(flappy_name)
        flappy_name_layout.addWidget(flappy_name_btn)
        custom_form.addRow("Flappy Ghost Name:", flappy_name_layout)

        timezone_setting = QLineEdit()
        timezone_setting.setPlaceholderText("e.g., America/New_York")
        timezone_setting.returnPressed.connect(
            lambda: self.send_command(f"settings set timezone {timezone_setting.text()}")
        )
        timezone_setting_btn = QPushButton("Set")
        timezone_setting_btn.clicked.connect(
            lambda: self.send_command(f"settings set timezone {timezone_setting.text()}")
        )
        timezone_setting_layout = QHBoxLayout()
        timezone_setting_layout.addWidget(timezone_setting)
        timezone_setting_layout.addWidget(timezone_setting_btn)
        custom_form.addRow("Timezone:", timezone_setting_layout)

        accent_color = QLineEdit()
        accent_color.setPlaceholderText("e.g., #FF0000")
        accent_color.returnPressed.connect(
            lambda: self.send_command(f"settings set accent_color {accent_color.text()}")
        )
        accent_color_btn = QPushButton("Set")
        accent_color_btn.clicked.connect(
            lambda: self.send_command(f"settings set accent_color {accent_color.text()}")
        )
        accent_color_layout = QHBoxLayout()
        accent_color_layout.addWidget(accent_color)
        accent_color_layout.addWidget(accent_color_btn)
        custom_form.addRow("Accent Color (hex):", accent_color_layout)

        custom_layout.addWidget(custom_group)
        custom_layout.addStretch()
        settings_tabs.addTab(custom_widget, "Custom")

        return settings_tabs

    def create_command_group(self, title, commands, layout, row, col):
        """
        Create a group of command buttons.

        Args:
            title (str): The group title.
            commands (list): List of (button text, command/callback) tuples.
            layout (QGridLayout): The layout to add the group to.
            row (int): Row position in the grid.
            col (int): Column position in the grid.
        """
        group = QGroupBox(title)
        group_layout = QVBoxLayout(group)

        for text, command in commands:
            btn = QPushButton(text)
            btn.setToolTip(f"Send command: {text}")
            if callable(command):
                btn.clicked.connect(command)
            else:
                btn.clicked.connect(partial(self.send_command, command))
            group_layout.addWidget(btn)

        group_layout.addStretch()
        layout.addWidget(group, row, col)

    def setup_display_area(self, layout):
        """
        Set up the display area for responses and custom commands.

        Args:
            layout (QVBoxLayout): The layout to add the display area to.
        """
        # Make display area resizable using a QSplitter
        display_splitter = QSplitter(Qt.Orientation.Vertical)

        # --- Display Group ---
        display_group = QGroupBox("Display")
        display_layout = QVBoxLayout(display_group)

        self.display_text = QTextEdit()
        self.display_text.setReadOnly(True)
        display_layout.addWidget(self.display_text)

        button_layout = QHBoxLayout()
        clear_display_btn = QPushButton("Clear Display")
        clear_display_btn.clicked.connect(self.display_text.clear)
        button_layout.addWidget(clear_display_btn)

        save_log_btn = QPushButton("Save Log")
        save_log_btn.clicked.connect(self.save_log)
        button_layout.addWidget(save_log_btn)
        display_layout.addLayout(button_layout)

        # --- Custom Command Box ---
        cmd_layout = QHBoxLayout()
        self.cmd_entry = QLineEdit()
        self.cmd_entry.setPlaceholderText("Enter custom command...")
        self.cmd_entry.returnPressed.connect(self.send_custom_command)
        self.cmd_entry.installEventFilter(self)  # Add this line
        cmd_layout.addWidget(self.cmd_entry)
        send_cmd_btn = QPushButton("Send")
        send_cmd_btn.clicked.connect(self.send_custom_command)
        cmd_layout.addWidget(send_cmd_btn)
        display_layout.addLayout(cmd_layout)
        # --- End Custom Command Box ---

        display_splitter.addWidget(display_group)

        # Optionally, add another widget below (e.g., log area) for further resizing
        # display_splitter.addWidget(self.log_group)

        layout.addWidget(display_splitter)

    def setup_log_area(self):
        """Set up the log area for logging messages and saving logs."""
        self.log_group = QGroupBox("Log")
        log_layout = QVBoxLayout(self.log_group)

        self.log_text = QTextEdit()
        self.log_text.setReadOnly(True)
        self.log_text.setMaximumHeight(200)
        log_layout.addWidget(self.log_text)

        button_layout = QHBoxLayout()
        clear_log_btn = QPushButton("Clear Log")
        clear_log_btn.clicked.connect(self.log_text.clear)
        button_layout.addWidget(clear_log_btn)

        save_log_btn = QPushButton("Save Log")
        save_log_btn.clicked.connect(self.save_log)
        button_layout.addWidget(save_log_btn)

        log_layout.addLayout(button_layout)

    def refresh_ports(self):
        """Refresh the list of available serial ports."""
        self.port_combo.clear()
        ports = []
        for port in list_ports.comports():
            if port.device.startswith("/dev/ttyS"):
                continue
            if port.device.startswith("/dev/ttyAMA"):
                continue
            if port.device.startswith("/dev/ttyprintk"):
                continue
            if port.device.startswith("/dev/pts"):
                continue
            label = port.device
            try:
                desc = (port.description or "").lower()
                manu = (getattr(port, "manufacturer", "") or "").lower()
                vid = getattr(port, "vid", None)
                pid = getattr(port, "pid", None)
                looks_esp = False
                if vid is not None and pid is not None:
                    if (vid == 0x303A) or (vid == 0x0403 and pid == 0x6001) or (vid == 0x10C4 and pid == 0xEA60) or (vid == 0x1A86 and pid in (0x7523, 0x5523)):
                        looks_esp = True
                if ("cp210" in desc) or ("ch340" in desc) or ("ftdi" in desc) or ("esp32" in desc) or ("silicon labs" in manu) or ("wch" in manu):
                    looks_esp = True
                if looks_esp:
                    label = f"Maybe ESP32: {port.description}"
                else:
                    if port.description:
                        label = f"{port.description}"
            except Exception:
                pass
            ports.append((label, port.device))
        for label, device in ports:
            self.port_combo.addItem(label, device)
        if ports:
            self.port_combo.setCurrentIndex(0)

    def toggle_connection(self):
        """Connect or disconnect from the selected serial port."""
        if not self.serial_port or not self.serial_port.is_open:
            try:
                # get actual device from combo data if available
                data = self.port_combo.currentData()
                port = data if data else self.port_combo.currentText().split()[0]
                self.serial_port = serial.Serial(port, 115200, timeout=1)
                self.connect_btn.setText("Disconnect")
                self.connect_btn.setStyleSheet("")
                log_message(self.log_text, f"Connected to {port}", self.app_settings.get("show_timestamps", True))

                # Start monitor thread
                self.monitor_thread = SerialMonitorThread(self.serial_port)
                self.monitor_thread.data_received.connect(self.process_response)
                self.monitor_thread.start()

                self.auto_reconnect_enabled = True
                self.set_main_ui_enabled(True)
                self.update_connection_status(True)

            except serial.SerialException as e:
                error_msg = str(e)
                if "Permission denied" in error_msg:
                    error_msg += "\n\nTry running as root or check your user permissions for serial devices."
                elif "Device or resource busy" in error_msg:
                    error_msg += "\n\nThe port may be in use by another application."
                QMessageBox.critical(self, "Connection Error", error_msg)
                log_message(self.log_text, f"Connection error: {error_msg}", self.app_settings.get("show_timestamps", True))
                self.update_connection_status(False)
            except Exception as e:
                QMessageBox.critical(self, "Connection Error", str(e))
                log_message(self.log_text, f"Connection error: {str(e)}", self.app_settings.get("show_timestamps", True))
                self.update_connection_status(False)
        else:
            self.disconnect()
            self.auto_reconnect_enabled = False

    def disconnect(self):
        """Disconnect from the serial port and update UI."""
        if self.monitor_thread:
            self.monitor_thread.stop()
            self.monitor_thread.wait()
        if self.serial_port and self.serial_port.is_open:
            self.serial_port.close()
        self.connect_btn.setText("Connect")
        self.connect_btn.setStyleSheet("")  # Remove highlight
        log_message(self.log_text, "Disconnected", self.app_settings.get("show_timestamps", True))
        self.set_main_ui_enabled(False)
        self.update_connection_status(False)  # Ensure status updates on disconnect

    def send_command(self, command):
        """
        Send a command to the ESP32 via serial.

        Args:
            command (str): The command to send.
        """
        if not self.serial_port or not self.serial_port.is_open:
            QMessageBox.warning(self, "Not Connected", "Please connect to ESP32 first")
            return

        log_message(self.log_text, f"Sending command: {command}", self.app_settings.get("show_timestamps", True))
        try:
            self.serial_port.write(f"{command}\n".encode())
        except Exception as e:
            log_message(self.log_text, f"Error sending command: {str(e)}", self.app_settings.get("show_timestamps", True))
            self.disconnect()  # Disconnect

    def eventFilter(self, obj, event):
        """
        Handle custom command history navigation in the command entry.

        Args:
            obj (QObject): The object receiving the event.
            event (QEvent): The event to filter.

        Returns:
            bool: True if event handled, False otherwise.
        """
        if obj == self.cmd_entry and event.type() == event.Type.KeyPress:
            if event.key() == Qt.Key.Key_Up:
                if self.command_history and self.history_index > 0:
                    self.history_index -= 1
                    self.cmd_entry.setText(self.command_history[self.history_index])
                return True
            elif event.key() == Qt.Key.Key_Down:
                if self.command_history and self.history_index < len(self.command_history) - 1:
                    self.history_index += 1
                    self.cmd_entry.setText(self.command_history[self.history_index])
                elif self.history_index == len(self.command_history) - 1:
                    self.history_index += 1
                    self.cmd_entry.clear()
                return True
        return super().eventFilter(obj, event)

    def send_custom_command(self):
        """Send a custom command entered by the user."""
        command = self.cmd_entry.text().strip()
        if command:
            self.send_command(command)
            self.command_history.append(command)
            self.history_index = len(self.command_history)
            self.cmd_entry.clear()

    def process_response(self, response):
        """
        Process a response received from the ESP32.

        Args:
            response (str): The response string.
        """
        if response.startswith("Error reading serial:"):
            log_message(self.log_text, response, self.app_settings.get("show_timestamps", True))
            self.disconnect()
            self.update_connection_status(False)  # <-- Ensure status updates on error
            return

        # Check for evil portal list output
        if "Available Evil Portals:" in response or (
            hasattr(self, "_portal_list_mode") and self._portal_list_mode
        ):
            # Start portal list mode if header detected
            if "Available Evil Portals:" in response:
                self.portal_dropdown.clear()
                self.portal_dropdown.addItem("default")  # Always add "default" portal
                self._portal_list_mode = True
                self._portal_lines = []
                # Don't return yet, continue to parse this line

            # Parse lines for .html files, stripping timestamps
            lines = response.splitlines()
            for line in lines:
                line = line.strip()
                # Remove timestamp if present
                if "] " in line:
                    line = line.split("] ", 1)[-1]
                if line.endswith(".html"):
                    self._portal_lines.append(line)
                    self.portal_dropdown.addItem(line)
            self.display_text.append(response)
            self.display_text.ensureCursorVisible()
            return

        try:
            # Try to parse as JSON for structured data
            data = json.loads(response)
            if 'scan_result' in data:
                self.update_display_scan(data['scan_result'])
            elif 'status' in data:
                self.update_display_status(data['status'])
            else:
                self.display_text.append(response)

        except json.JSONDecodeError:
            # Format the text with timestamp if enabled
            if self.app_settings.get("show_timestamps", True):
                ts = timestamp("%H:%M:%S")
                formatted_text = f"[{ts}] {response}"
                self.display_text.append(formatted_text)
            else:
                self.display_text.append(response)

        self.display_text.ensureCursorVisible()

    def save_log(self):
        """Save the display log to a file."""
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        filename = f"ghost_esp_log_{timestamp}.txt"
        try:
            with open(filename, 'w') as f:
                f.write(self.display_text.toPlainText())
            log_message(self.log_text, f"Log saved to {filename}", self.app_settings.get("show_timestamps", True))
        except Exception as e:
            QMessageBox.critical(self, "Error", f"Failed to save log: {str(e)}")

    def connect_to_wifi(self):
        """Send WiFi connection credentials to the ESP32."""
        ssid = self.wifi_ssid.text()
        password = self.wifi_password.text()
        if ssid and password:
            self.send_command(f"connect {ssid} {password}")
        else:
            QMessageBox.warning(self, "Input Error", "Please enter both SSID and password")

    def start_evil_portal(self):
        """Start the evil portal with selected settings."""
        ssid = self.portal_ssid.text()
        password = self.portal_password.text()
        portal_file = self.portal_dropdown.currentText()

        if all([ssid, password, portal_file]):
            cmd = f"startportal {portal_file} {ssid} {password}"
            self.send_command(cmd)
        else:
            QMessageBox.warning(self, "Input Error", "Please fill all required fields and select a portal")

    def run_port_scan(self):
        """Run a port scan with the specified IP and arguments."""
        ip = self.portscan_ip.text().strip()
        args = self.portscan_args.text().strip()
        if ip and args:
            self.send_command(f"scanports {ip} {args}")
        else:
            QMessageBox.warning(self, "Input Error", "Please enter both IP and arguments")

    def start_capture(self):
        """Send the capture command to the ESP32 based on selected type."""
        capture_type = self.capture_type_combo.currentText().lower()
        self.send_command(f"capture -{capture_type}")

    def update_display_scan(self, scan_data):
        """
        Update the display area with scan results.

        Args:
            scan_data (list): List of scan result items.
        """
        self.display_text.append("\n=== Scan Results ===")
        for item in scan_data:
            self.display_text.append(f"- {item}")
        self.display_text.append("==================\n")
        self.display_text.ensureCursorVisible()

    def update_display_status(self, status):
        """
        Update the display area with a status message.

        Args:
            status (str): The status message.
        """
        if self.app_settings.get("show_timestamps", True):
            ts = timestamp("%H:%M:%S")
            self.display_text.append(f"[{ts}] Status: {status}")
        else:
            self.display_text.append(f"Status: {status}")
        self.display_text.ensureCursorVisible()

    def closeEvent(self, event):
        """
        Handle the window close event and disconnect serial if needed.

        Args:
            event (QCloseEvent): The close event.
        """
        if self.serial_port and self.serial_port.is_open:
            self.disconnect()
        super().closeEvent(event)

    def show_beacon_add_dialog(self):
        """Show a dialog to add an SSID to the beacon list."""
        ssid, ok = QInputDialog.getText(self, "Add SSID", "Enter SSID to add to beacon list:")
        if ok and ssid:
            self.send_command(f'beaconadd "{ssid}"')

    def show_beacon_remove_dialog(self):
        """Show a dialog to remove an SSID from the beacon list."""
        ssid, ok = QInputDialog.getText(self, "Remove SSID", "Enter SSID to remove from beacon list:")
        if ok and ssid:
            self.send_command(f'beaconremove "{ssid}"')

    def show_sdmmc_dialog(self):
        """Show a dialog to set SDMMC pins."""
        pins, ok = QInputDialog.getText(self, "Set SDMMC Pins", "Enter pins: clk cmd d0 d1 d2 d3 (space-separated)")
        if ok and pins:
            self.send_command(f"sd_pins_mmc {pins}")

    def show_sdspi_dialog(self):
        """Show a dialog to set SPI pins."""
        pins, ok = QInputDialog.getText(self, "Set SPI Pins", "Enter pins: cs clk miso mosi (space-separated)")
        if ok and pins:
            self.send_command(f"sd_pins_spi {pins}")

    def start_probe_listener(self):
        """Start listening for probe requests on the specified channel."""
        channel = self.probe_channel.text().strip()
        if channel:
            self.send_command(f"listenprobes {channel}")
        else:
            self.send_command("listenprobes")

    def show_rgbpins_dialog(self):
        """Show a dialog to set RGB pins."""
        pins, ok = QInputDialog.getText(self, "Set RGB Pins", "Enter RGB pins (R1 G1 B1 R2 G2 B2 ...):")
        if ok and pins:
            self.send_command(f"rgb_pins {pins}")

    def send_local_portal_file(self):
        """Send a local HTML file as an evil portal to the ESP32."""
        file_path, _ = QFileDialog.getOpenFileName(self, "Select HTML File", "", "HTML Files (*.html *.htm)")
        if file_path:
            try:
                with open(file_path, "r", encoding="utf-8") as f:
                    html_content = f.read()
                safe_html = html_content
                self.portal_upload_indicator.setText("Uploading portal file...")
                self.portal_progress_bar.setVisible(True)
                self.portal_progress_bar.setValue(0)
                self.portal_sender_thread = PortalFileSenderThread(safe_html)
                self.portal_sender_thread.send_line.connect(self.send_command)
                self.portal_sender_thread.finished.connect(self._portal_upload_finished)
                self.portal_sender_thread.error.connect(self._portal_upload_error)
                self.portal_sender_thread.progress.connect(self._portal_upload_progress)  # Add this signal
                self.portal_sender_thread.start()
            except Exception as e:
                self.portal_upload_indicator.setText("")
                self.portal_progress_bar.setVisible(False)
                QMessageBox.critical(self, "Error", f"Failed to send file: {str(e)}")

    def _portal_upload_progress(self, percent):
        """
        Update the portal upload progress bar.

        Args:
            percent (int): Progress percentage.
        """
        self.portal_progress_bar.setValue(percent)

    def _portal_upload_finished(self):
        """Handle completion of portal file upload."""
        self.portal_upload_indicator.setText("")
        self.portal_progress_bar.setVisible(False)
        QMessageBox.information(self, "Portal Sent", "HTML file sent as evil portal.")

    def _portal_upload_error(self, e):
        """
        Handle an error during portal file upload.

        Args:
            e (str): Error message.
        """
        self.portal_upload_indicator.setText("")
        self.portal_progress_bar.setVisible(False)
        QMessageBox.critical(self, "Error", f"Failed to send file: {e}")

    def check_auto_reconnect(self):
        """Check and handle auto-reconnect logic for the serial port."""
        if getattr(self, "auto_reconnect_enabled", False) and self.auto_reconnect_checkbox.isChecked():
            if not self.serial_port or not self.serial_port.is_open:
                        # get actual device from combo data if available
                data = self.port_combo.currentData()
                port = data if data else self.port_combo.currentText().split()[0]
                if port:
                    try:
                        self.serial_port = serial.Serial(port, 115200, timeout=1)
                        self.connect_btn.setText("Disconnect")
                        self.connect_btn.setStyleSheet("")
                        log_message(self.log_text, f"Auto-reconnected to {port}", self.app_settings.get("show_timestamps", True))

                        self.monitor_thread = SerialMonitorThread(self.serial_port)
                        self.monitor_thread.data_received.connect(self.process_response)
                        self.monitor_thread.start()
                        self.update_connection_status(True)
                        self.set_main_ui_enabled(True)
                        self.reconnect_attempts = 0  # Reset on success
                        self.reconnect_timer.setInterval(self.reconnect_base_interval)
                    except serial.SerialException as e:
                        error_msg = str(e)
                        if "Permission denied" in error_msg:
                            error_msg += "\n\nTry running as root or check your user permissions for serial devices."
                        elif "Device or resource busy" in error_msg:
                            error_msg += "\n\nThe port may be in use by another application."
                        log_message(self.log_text, f"Auto-reconnect failed: {error_msg}")
                        self.update_connection_status(False)
                        self.set_main_ui_enabled(False)
                        self.reconnect_attempts += 1
                        # Exponential backoff, max 32 seconds
                        backoff = min(self.reconnect_base_interval * (2 ** self.reconnect_attempts), 32000)
                        self.reconnect_timer.setInterval(backoff)
                    except Exception as e:
                        log_message(self.log_text, f"Auto-reconnect failed: {str(e)}", self.app_settings.get("show_timestamps", True))
                        self.update_connection_status(False)
                        self.set_main_ui_enabled(False)
                        self.reconnect_attempts += 1
                        backoff = min(self.reconnect_base_interval * (2 ** self.reconnect_attempts), 32000)
                        self.reconnect_timer.setInterval(backoff)
        self.auto_reconnect_checkbox.stateChanged.connect(self.toggle_reconnect_timer)

    def toggle_reconnect_timer(self, state):
        """
        Toggle the auto-reconnect timer based on checkbox state.

        Args:
            state (int): Checkbox state.
        """
        if state:
            self.reconnect_timer.start()
        else:
            self.reconnect_timer.stop()

    def showEvent(self, event):
        """
        Handle the show event to adjust overlay geometry.

        Args:
            event (QShowEvent): The show event.
        """
        super().showEvent(event)
        self.resizeEvent(None)

    def browse_bin_file(self, line_edit):
        """Open a file dialog to select a .bin file and set it in the given QLineEdit."""
        file_path, _ = QFileDialog.getOpenFileName(self, "Select BIN File", "", "BIN Files (*.bin)")
        if file_path:
            line_edit.setText(file_path)

    def browse_zip_file(self, line_edit):
        """Open a file dialog to select a .zip file and set it in the given QLineEdit."""
        file_path, _ = QFileDialog.getOpenFileName(self, "Select Release Bundle ZIP", "", "ZIP Files (*.zip)")
        if file_path:
            line_edit.setText(file_path)

    def exit_flash_mode(self):
        """Exit Flash Mode and return to the main UI."""
        self.flash_mode_btn.setChecked(False)
        self.toggle_flash_mode()

    def fetch_github_releases(self):
        """Fetch release tags from GitHub and populate the version dropdown."""
        api_url = "https://api.github.com/repos/jaylikesbunda/Ghost_ESP/releases"
        
        # Clean up any existing thread
        if hasattr(self, 'release_fetch_thread') and self.release_fetch_thread:
            self.release_fetch_thread.finished.disconnect()
            self.release_fetch_thread.error.disconnect()
            self.release_fetch_thread.retry_info.disconnect()
            self.release_fetch_thread.stop()
            self.release_fetch_thread.wait(100)
            self.release_fetch_thread = None
        
        # Show loading state
        self.release_version_combo.blockSignals(True)
        self.release_version_combo.clear()
        self.release_version_combo.addItem("Loading releases...")
        self.release_version_combo.setEnabled(False)
        
        # Create and start the fetch thread
        show_prereleases = self.show_prereleases_checkbox.isChecked()
        self.release_fetch_thread = ReleaseFetchThread(api_url, show_prereleases)
        self.release_fetch_thread.finished.connect(self.on_releases_fetched)
        self.release_fetch_thread.error.connect(self.on_releases_fetch_error)
        self.release_fetch_thread.retry_info.connect(self.on_releases_fetch_retry)
        self.release_fetch_thread.start()
    
    def on_releases_fetched(self, releases):
        """Handle successful release fetch."""
        self._github_releases = releases  # Save for asset lookup
        versions = [release.get("tag_name", "Unknown") for release in releases if "tag_name" in release]
        self.release_version_combo.clear()
        # Add versions first, then "Custom local .zip" at the end
        if versions:
            self.release_version_combo.addItems(versions)
        else:
            self.release_version_combo.addItem("No releases found")
        # Add "Custom local .zip" as the last option
        self.release_version_combo.addItem("Custom local .zip")
        self.release_version_combo.setCurrentIndex(0)  # Select first version by default
        self.release_version_combo.setEnabled(True)
        self.release_version_combo.blockSignals(False)
        
        # Always update assets dropdown as well
        self.update_release_assets_dropdown()
        
        # Clean up thread
        if hasattr(self, 'release_fetch_thread') and self.release_fetch_thread:
            self.release_fetch_thread.finished.disconnect()
            self.release_fetch_thread.error.disconnect()
            self.release_fetch_thread.retry_info.disconnect()
            self.release_fetch_thread.wait(100)
            self.release_fetch_thread = None
    
    def on_releases_fetch_error(self, error_msg):
        """Handle release fetch error."""
        self.release_version_combo.clear()
        if "Timeout" in error_msg:
            self.release_version_combo.addItem("Timeout: Failed to load releases (network too slow)")
        else:
            self.release_version_combo.addItem("Failed to load releases")
        self.release_version_combo.addItem("Custom local .zip")
        self.release_version_combo.setCurrentIndex(0)
        self.release_version_combo.setEnabled(True)
        self._github_releases = []
        self.release_version_combo.blockSignals(False)
        
        if hasattr(self, 'flash_console'):
            self.flash_console.append(f"Error: {error_msg}")
        print(f"Error fetching releases: {error_msg}")
        
        # Always update assets dropdown as well
        self.update_release_assets_dropdown()
        
        # Clean up thread
        if hasattr(self, 'release_fetch_thread') and self.release_fetch_thread:
            self.release_fetch_thread.finished.disconnect()
            self.release_fetch_thread.error.disconnect()
            self.release_fetch_thread.retry_info.disconnect()
            self.release_fetch_thread.wait(100)
            self.release_fetch_thread = None
    
    def on_releases_fetch_retry(self, message, attempt, max_retries):
        """Handle release fetch retry."""
        self.release_version_combo.clear()
        self.release_version_combo.addItem(f"Retrying... (attempt {attempt}/{max_retries})")

    def update_release_assets_dropdown(self):
        """Populate the asset dropdown based on the selected version."""
        if not hasattr(self, "release_asset_combo"):
            # Create container for asset selection and progress
            asset_container = QWidget()
            asset_container_layout = QVBoxLayout(asset_container)
            asset_container_layout.setContentsMargins(0, 0, 0, 0)
            asset_container_layout.setSpacing(5)
            
            asset_layout = QHBoxLayout()
            asset_label = QLabel("Asset:")
            asset_label.setContentsMargins(0, 0, 0, 0)
            asset_layout.setSpacing(0)
            self.release_asset_combo = QComboBox()
            self.release_asset_combo.addItem("Select a version first")
            asset_layout.addWidget(asset_label)
            asset_layout.addWidget(self.release_asset_combo)
            asset_container_layout.addLayout(asset_layout)
            
            # Add progress bar below the dropdown
            self.asset_download_progress = QProgressBar()
            self.asset_download_progress.setMinimum(0)
            self.asset_download_progress.setMaximum(100)
            self.asset_download_progress.setValue(0)
            self.asset_download_progress.setVisible(False)
            self.asset_download_progress.setMinimumHeight(20)
            self.asset_download_progress.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
            asset_container_layout.addWidget(self.asset_download_progress)

            # Insert asset_container directly after the version_layout
            parent_layout = self.release_version_combo.parentWidget().layout()
            insert_index = None
            for i in range(parent_layout.count()):
                item = parent_layout.itemAt(i)
                if item and item.layout():
                    for j in range(item.layout().count()):
                        subitem = item.layout().itemAt(j)
                        widget = subitem.widget()
                        if widget is self.release_version_combo:
                            insert_index = i + 1
                           
                            break
                if insert_index is not None:
                    break
            if insert_index is not None:
                parent_layout.insertWidget(insert_index, asset_container)
            else:
                parent_layout.addWidget(asset_container)  # fallback

            # Connect asset selection to download handler
            self.release_asset_combo.currentIndexChanged.connect(self.download_selected_asset)

        self.release_asset_combo.clear()
        selected_version = self.release_version_combo.currentText()
        releases = getattr(self, "_github_releases", [])
        assets = []
        for release in releases:
            if release.get("tag_name") == selected_version:
                assets = release.get("assets", [])
                break
        if assets:
            self.release_asset_combo.addItem("Select Release Asset...")
            for asset in assets:
                name = asset.get("name", "Unknown")
                self.release_asset_combo.addItem(name, asset.get("browser_download_url"))
            self.release_asset_combo.setCurrentIndex(0)
        else:
            self.release_asset_combo.addItem("No assets found")

    def download_selected_asset(self, index):
        """Download the selected asset (if not the prompt) and set the path in the zip file field.
           Also auto-select the chip based on asset name using the release_assets dictionary."""
        if not hasattr(self, "release_asset_combo"):
            return
        url = self.release_asset_combo.currentData()
        name = self.release_asset_combo.currentText()
        if not url or "Select Release Asset" in name or "No assets found" in name:
            return

        # --- Auto-select chip based on asset name ---
        release_assets = {
            "esp32-generic.zip": "esp32",
            "esp32s2-generic.zip": "esp32s2",
            "esp32s3-generic.zip": "esp32s3",
            "esp32c3-generic.zip": "esp32c3",
            "esp32c5-generic-v01.zip": "esp32c5",
            "esp32c6-generic.zip": "esp32c6",
            "esp32v5_awok.zip": "esp32s2",
            "ghostboard.zip": "esp32c6",
            "MarauderV4_FlipperHub.zip": "esp32",
            "MarauderV6_AwokDual.zip": "esp32",
            "AwokMini.zip": "esp32s2",
            "ESP32-S3-Cardputer.zip": "esp32s3",
            "CYD2USB.zip": "esp32",
            "CYDMicroUSB.zip": "esp32",
            "CYDDualUSB.zip": "esp32",
        }
        chip = release_assets.get(name, "")
        if chip:
            self.custom_chip_combo.setCurrentText(chip)
            self.selected_chip = chip

        # --- Download the asset in background thread ---
        import tempfile
        import os
        
        temp_dir = tempfile.gettempdir()
        safe_name = os.path.basename(name)
        temp_path = os.path.join(temp_dir, safe_name)
        
        # Stop any existing download thread
        if hasattr(self, 'asset_download_thread') and self.asset_download_thread.isRunning():
            self.asset_download_thread.stop()
            self.asset_download_thread.wait(1000)  # Wait up to 1 second for thread to stop
        
        # Show download status
        self.flash_console.append(f"Downloading asset: {name} ...")
        self.flash_bundle_status.setText(f"Downloading {name}...")
        
        # Show and reset progress bar
        if hasattr(self, 'asset_download_progress'):
            self.asset_download_progress.setValue(0)
            self.asset_download_progress.setVisible(True)
            self.asset_download_progress.setFormat("Downloading... 0%")
        
        # Create and start download thread
        self.asset_download_thread = AssetDownloadThread(url, temp_path, name)
        
        # Connect signals
        self.asset_download_thread.progress_update.connect(self.on_download_progress)
        self.asset_download_thread.status_update.connect(self.on_download_status)
        self.asset_download_thread.finished.connect(self.on_download_finished)
        self.asset_download_thread.error.connect(self.on_download_error)
        self.asset_download_thread.retry_info.connect(self.on_download_retry)
        
        # Start the download thread
        self.asset_download_thread.start()
    
    def on_download_progress(self, name, downloaded_mb, total_mb):
        """Update UI with download progress."""
        if total_mb > 0:
            percent = int((downloaded_mb / total_mb) * 100)
            self.flash_bundle_status.setText(
                f"Downloading {name}... {percent:.1f}% ({downloaded_mb}MB/{total_mb}MB)"
            )
            # Update progress bar
            if hasattr(self, 'asset_download_progress'):
                self.asset_download_progress.setValue(percent)
                self.asset_download_progress.setFormat(f"Downloading... {percent}% ({downloaded_mb}MB/{total_mb}MB)")
        else:
            self.flash_bundle_status.setText(f"Downloading {name}... {downloaded_mb}MB")
            # Update progress bar (indeterminate mode)
            if hasattr(self, 'asset_download_progress'):
                self.asset_download_progress.setValue(0)
                self.asset_download_progress.setFormat(f"Downloading... {downloaded_mb}MB")
    
    def on_download_status(self, message):
        """Update UI with download status message."""
        # Truncate very long status messages to prevent window expansion
        max_status_length = 80
        if len(message) > max_status_length:
            truncated_message = message[:max_status_length] + "..."
        else:
            truncated_message = message
        self.flash_bundle_status.setText(truncated_message)
        self.flash_console.append(message)
        # Update progress bar text (truncate for progress bar too)
        if hasattr(self, 'asset_download_progress'):
            progress_format = truncated_message[:50] if len(truncated_message) > 50 else truncated_message
            self.asset_download_progress.setFormat(progress_format)
    
    def on_download_retry(self, message, attempt, max_retries):
        """Update UI with retry information."""
        self.flash_console.append(message)
        self.flash_bundle_status.setText(f"Retrying download (attempt {attempt}/{max_retries})...")
        # Reset progress bar on retry
        if hasattr(self, 'asset_download_progress'):
            self.asset_download_progress.setValue(0)
            self.asset_download_progress.setFormat(f"Retrying... (attempt {attempt}/{max_retries})")
    
    def on_download_finished(self, file_path, message):
        """Handle successful download completion."""
        self.release_zip_edit.setText(file_path)
        self.flash_bundle_status.setText(message)
        self.flash_console.append(message)
        # Update progress bar to show completion
        if hasattr(self, 'asset_download_progress'):
            self.asset_download_progress.setValue(100)
            self.asset_download_progress.setFormat("Download complete!")
            # Hide progress bar after a short delay
            QTimer.singleShot(2000, lambda: self.asset_download_progress.setVisible(False) if hasattr(self, 'asset_download_progress') else None)
        # Clean up thread - disconnect signals and wait for it to finish
        if hasattr(self, 'asset_download_thread') and self.asset_download_thread:
            self.asset_download_thread.progress_update.disconnect()
            self.asset_download_thread.status_update.disconnect()
            self.asset_download_thread.finished.disconnect()
            self.asset_download_thread.error.disconnect()
            self.asset_download_thread.retry_info.disconnect()
            self.asset_download_thread.wait(100)  # Wait briefly for thread to finish
            self.asset_download_thread = None
    
    def on_download_error(self, error_message):
        """Handle download error."""
        # Truncate very long error messages to prevent window expansion
        max_error_length = 60  # Reduced to prevent expansion
        if len(error_message) > max_error_length:
            truncated_error = error_message[:max_error_length] + "..."
        else:
            truncated_error = error_message
        
        # Use a shorter, fixed message format to prevent layout expansion
        self.flash_bundle_status.setText(f"Download failed: {truncated_error}")
        
        # Preserve window size by ensuring layout doesn't expand
        current_size = self.size()
        QApplication.processEvents()  # Process any pending layout updates
        # Restore window size if it changed
        if self.size() != current_size:
            self.resize(current_size)
        self.flash_console.append(f"Failed to download asset: {error_message}")
        self.flash_console.append("This may be due to network issues or GitHub server problems.")
        self.flash_console.append("Please try again later, or use 'Custom local .zip' to browse for a local file.")
        # Update progress bar to show error
        if hasattr(self, 'asset_download_progress'):
            self.asset_download_progress.setValue(0)
            self.asset_download_progress.setFormat("Download failed")
            self.asset_download_progress.setStyleSheet("QProgressBar::chunk { background-color: #ff4444; }")
            # Hide progress bar after a delay
            QTimer.singleShot(3000, lambda: self.asset_download_progress.setVisible(False) if hasattr(self, 'asset_download_progress') else None)
            # Reset style after hiding
            QTimer.singleShot(3000, lambda: self.asset_download_progress.setStyleSheet("") if hasattr(self, 'asset_download_progress') else None)
        # Clean up thread - disconnect signals and wait for it to finish
        if hasattr(self, 'asset_download_thread') and self.asset_download_thread:
            self.asset_download_thread.progress_update.disconnect()
            self.asset_download_thread.status_update.disconnect()
            self.asset_download_thread.finished.disconnect()
            self.asset_download_thread.error.disconnect()
            self.asset_download_thread.retry_info.disconnect()
            self.asset_download_thread.wait(100)  # Wait briefly for thread to finish
            self.asset_download_thread = None

    def on_flash_panel_changed(self, index):
        """Show instructions when flash panel tab is changed."""
        # Show instructions in the flasher output window for each panel
        self.flash_console.clear()
        if index == 0:  # Flash Firmware
            self.flash_console.append(
                "Instructions: Flash Firmware\n"
                "1. Select the correct chip type for your board.\n"
                "2. Browse and select the bootloader.bin, partition-table.bin, and firmware.bin files.\n"
                "3. Select the serial port.\n"
                "4. Click 'Flash Board' to flash your ESP32.\n"
                "5. Use 'Exit Flash Mode' to return to the main UI."
            )
        elif index == 1:  # Flash Release Bundle
            self.flash_console.append(
                "Instructions: Flash Release Bundle\n"
                "1. Select a release version or choose 'Custom local .zip' to use your own bundle.\n"
                "2. Select the desired asset if multiple are available.\n"
                "3. Download the asset or browse for a local .zip file.\n"
                "4. Select the chip type and serial port.\n"
                "5. Click 'Flash Bundle' to flash your ESP32.\n"
                "6. Use 'Exit Flash Mode' to return to the main UI."
            )
            self.fetch_github_releases()
        elif index == 2:  # Custom Build
            self.flash_console.append(
                "Instructions: Custom Build\n"
                "1. Copy an SDKConfig template or edit your existing one with the edit sdkconfig button.\n"
                "2. Set the target chip and run 'Set Target'.\n"
                "3. Use 'Run Build' to compile your firmware (requires ESP-IDF in PATH).\n"
                "4. Use 'Run idf.py fullclean' to clean the build folder if needed.\n"
                "5. Click 'Flash Custom Build' to flash the built firmware from the build folder.\n"
                "6. Use 'Exit Flash Mode' to return to the main UI.\n"
                "Note: Use at your own risk. Support will not be provided for unofficial images."
            )
            QMessageBox.warning(
                self,
                "Custom Build Warning",
                "Use at your own risk. Support will not be provided for unofficial images"
            )

    def run_idf_menuconfig(self):
        """Run idf.py menuconfig in the project root (../../) in a new terminal window (cross-platform, with ESP-IDF env)."""
        import os
        import sys
        import shutil
        from PyQt6.QtWidgets import QMessageBox

        project_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../"))
        idf_path = find_esp_idf_gui(self)
        if not idf_path:
            QMessageBox.critical(self, "ESP-IDF Not Found", "ESP-IDF not found. Please install or configure ESP-IDF first.")
            return

        self.flash_console.append(f"Opening terminal to run: idf.py menuconfig\n")

        try:
            if sys.platform.startswith("linux"):
                export_script = os.path.join(idf_path, "export.sh")
                if not os.path.exists(export_script):
                    raise RuntimeError(f"ESP-IDF export.sh not found at {export_script}")
                terminals = [
                    ("gnome-terminal", f'-- bash -c "cd \\"{project_root}\\"; source \\"{export_script}\\"; idf.py menuconfig"'),
                    ("xfce4-terminal", f'--command="bash -c \'cd \\"{project_root}\\"; source \\"{export_script}\\"; idf.py menuconfig\'"'),
                    ("konsole", f'--workdir "{project_root}" -e bash -c "source \\"{export_script}\\"; idf.py menuconfig"'),
                    ("xterm", f'-e "cd \\"{project_root}\\"; source \\"{export_script}\\"; idf.py menuconfig"'),
                    ("lxterminal", f'-e bash -c "cd \\"{project_root}\\"; source \\"{export_script}\\"; idf.py menuconfig"'),
                    ("mate-terminal", f'-- bash -c "cd \\"{project_root}\\"; source \\"{export_script}\\"; idf.py menuconfig"'),
                    ("tilix", f'-e bash -c "cd \\"{project_root}\\"; source \\"{export_script}\\"; idf.py menuconfig"'),
                    ("alacritty", f'-e bash -c "cd \\"{project_root}\\"; source \\"{export_script}\\"; idf.py menuconfig"'),
                    ("kitty", f'-e bash -c "cd \\"{project_root}\\"; source \\"{export_script}\\"; idf.py menuconfig"'),
                ]
                for term, args in terminals:
                    if shutil.which(term):
                        os.system(f'{term} {args}')
                        return
                raise RuntimeError("No supported terminal emulator found. Please install gnome-terminal, konsole, xterm, etc.")

            elif sys.platform.startswith("win"):
                export_script = os.path.join(idf_path, "export.bat")
                if not os.path.exists(export_script):
                    raise RuntimeError(f"ESP-IDF export.bat not found at {export_script}")
                cmd = f'start cmd.exe /K "cd /d {project_root} && call \\"{export_script}\\" && idf.py menuconfig"'
                os.system(cmd)
                return

            elif sys.platform == "darwin":
                export_script = os.path.join(idf_path, "export.sh")
                if not os.path.exists(export_script):
                    raise RuntimeError(f"ESP-IDF export.sh not found at {export_script}")
                osa_script = f'''
                tell application "Terminal"
                    activate
                    do script "cd \\"{project_root}\\"; source \\"{export_script}\\"; idf.py menuconfig; exit"
                end tell
                '''
                os.system(f'osascript -e \'{osa_script}\'')
                return

            else:
                raise RuntimeError(f"Unsupported OS: {sys.platform}")

        except Exception as e:
            self.flash_console.append(f"Failed to run idf.py menuconfig: {e}")
            QMessageBox.critical(self, "Error", f"Failed to run idf.py menuconfig:\n{e}")

    def run_idf_set_target(self):
        """Run idf.py set-target for the selected chip."""
        import subprocess
        import os
        from PyQt6.QtWidgets import QMessageBox

        chip = self.custom_chip_combo.currentText()
        if not chip or chip == "Select Chip":
            QMessageBox.warning(self, "No Chip Selected", "Please select a chip type first.")
            return

        self.flash_console.append(f"Setting ESP-IDF target to {chip}...\n")
        try:
            # Set working directory to two levels up from this script
            project_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../"))
            idf_path = find_esp_idf_gui(self)
            env = get_esp_idf_env(idf_path) if idf_path else None
            process = subprocess.Popen(
                ["idf.py", "set-target", chip],
                cwd=project_root,
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True
            )
            for line in process.stdout:
                self.flash_console.append(line.rstrip())
                self.flash_console.ensureCursorVisible()
                QApplication.processEvents()
            process.wait()
            if process.returncode == 0:
                self.flash_console.append(f"ESP-IDF target set to {chip} successfully.")
            else:
                self.flash_console.append("Failed to set ESP-IDF target. See errors above.")
        except Exception as e:
            self.flash_console.append(f"Error: {e}")
            QMessageBox.critical(self, "Error", f"Failed to run idf.py set-target:\n{e}")

    def run_idf_fullclean(self):
        """Run idf.py fullclean in the project root (../../) as a subprocess and show output in the console."""
        import os
        import subprocess
        from PyQt6.QtWidgets import QMessageBox, QApplication

        project_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../"))
        idf_cmd = ["idf.py", "fullclean"]

        self.flash_console.append("Running: idf.py fullclean\n")
        QApplication.setOverrideCursor(Qt.CursorShape.WaitCursor)
        try:
            idf_path = find_esp_idf_gui(self)
            env = get_esp_idf_env(idf_path) if idf_path else None
            process = subprocess.Popen(
                idf_cmd,
                cwd=project_root,
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True
            )
            for line in process.stdout:
                self.flash_console.append(line.rstrip())
                self.flash_console.ensureCursorVisible()
                QApplication.processEvents()
            process.wait()
            if process.returncode == 0:
                self.flash_console.append("idf.py fullclean finished successfully.")
            else:
                self.flash_console.append("idf.py fullclean exited with errors.")
        except Exception as e:
            self.flash_console.append(f"Failed to run idf.py fullclean: {e}")
            QMessageBox.critical(self, "Error", f"Failed to run idf.py fullclean:\n{e}")
        finally:
            QApplication.restoreOverrideCursor()
            
    def run_idf_build(self):
        """Run idf.py build in the project root (../../) as a subprocess and show output in the console."""
        import os
        import subprocess
        from PyQt6.QtWidgets import QMessageBox, QApplication

        project_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../"))
        idf_cmd = ["idf.py", "build"]

        if hasattr(self, 'flash_console'):
            self.flash_console.append("Running: idf.py build\n")
        QApplication.setOverrideCursor(Qt.CursorShape.WaitCursor)
        try:
            idf_path = find_esp_idf_gui(self)
            env = get_esp_idf_env(idf_path) if idf_path else None
            process = subprocess.Popen(
                idf_cmd,
                cwd=project_root,
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True
            )
            if hasattr(self, 'flash_console'):
                for line in process.stdout:
                    self.flash_console.append(line.rstrip())
                    self.flash_console.ensureCursorVisible()
                    QApplication.processEvents()
            process.wait()
            if hasattr(self, 'flash_console'):
                if process.returncode == 0:
                    self.flash_console.append("idf.py build finished successfully.")
                else:
                    self.flash_console.append("idf.py build exited with errors.")
        except Exception as e:
            error_msg = f"Failed to run idf.py build: {e}"
            if hasattr(self, 'flash_console'):
                self.flash_console.append(error_msg)
            QMessageBox.critical(self, "Error", error_msg)
        finally:
            QApplication.restoreOverrideCursor()



