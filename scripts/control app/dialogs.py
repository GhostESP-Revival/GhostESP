from PyQt6.QtWidgets import QInputDialog, QDialog, QFormLayout, QLineEdit, QTextEdit, QSpinBox, QComboBox, QPushButton, QHBoxLayout

def show_select_ap_dialog(self):
  """Show a dialog to select an access point and send the selection."""
  selected_ap, ok = QInputDialog.getText(self, "Select Access Point", "Enter Access Point name:")
  if ok and selected_ap:
      self.send_command(f"select -a {selected_ap}")

def show_custom_beacon_dialog(self):
  """Show a dialog to enter a custom SSID for beacon spam."""
  ssid, ok = QInputDialog.getText(self, "Custom Beacon", "Enter SSID for beacon spam:")
  if ok and ssid:
      self.send_command(f'beaconspam "{ssid}"')

def show_printer_dialog(self):
  """Show a dialog to print text to a network printer."""
  dialog = QDialog(self)
  dialog.setWindowTitle("Print to Network Printer")
  layout = QFormLayout(dialog)

  ip_input = QLineEdit()
  text_input = QTextEdit()
  font_size = QSpinBox()
  font_size.setRange(8, 72)
  font_size.setValue(12)

  alignment = QComboBox()
  alignment.addItems(["Center Middle (CM)", "Top Left (TL)", "Top Right (TR)",
                    "Bottom Right (BR)", "Bottom Left (BL)"])

  layout.addRow("Printer IP:", ip_input)
  layout.addRow("Text:", text_input)
  layout.addRow("Font Size:", font_size)
  layout.addRow("Alignment:", alignment)

  buttons = QHBoxLayout()
  ok_button = QPushButton("Print")
  cancel_button = QPushButton("Cancel")
  buttons.addWidget(ok_button)
  buttons.addWidget(cancel_button)
  layout.addRow(buttons)

  ok_button.clicked.connect(dialog.accept)
  cancel_button.clicked.connect(dialog.reject)

  if dialog.exec() == QDialog.DialogCode.Accepted:
      align_map = {"Center Middle (CM)": "CM", "Top Left (TL)": "TL",
                  "Top Right (TR)": "TR", "Bottom Right (BR)": "BR",
                  "Bottom Left (BL)": "BL"}
      cmd = f'powerprinter {ip_input.text()} "{text_input.toPlainText()}" {font_size.value()} {align_map[alignment.currentText()]}'
      self.send_command(cmd)
