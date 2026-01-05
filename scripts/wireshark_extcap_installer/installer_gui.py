#!/usr/bin/env python3
import tkinter as tk
from tkinter import ttk, messagebox, scrolledtext
import os
import sys
import shutil
import subprocess
import threading

class GhostESPInstallerGUI:
    def __init__(self, root):
        self.root = root
        self.root.title("GhostESP Wireshark Extcap Installer")
        self.root.geometry("600x500")
        self.root.resizable(False, False)
        
        self.setup_ui()
        self.check_environment()
    
    def setup_ui(self):
        main_frame = ttk.Frame(self.root, padding="20")
        main_frame.grid(row=0, column=0, sticky=(tk.W, tk.E, tk.N, tk.S))
        
        title = ttk.Label(main_frame, text="GhostESP Wireshark Extcap Installer", 
                         font=('Arial', 16, 'bold'))
        title.grid(row=0, column=0, columnspan=2, pady=10)
        
        desc = ttk.Label(main_frame, text="Install GhostESP as a Wireshark capture interface",
                        wraplength=550)
        desc.grid(row=1, column=0, columnspan=2, pady=5)
        
        separator = ttk.Separator(main_frame, orient='horizontal')
        separator.grid(row=2, column=0, columnspan=2, sticky='ew', pady=10)
        
        self.status_frame = ttk.LabelFrame(main_frame, text="Installation Status", padding="10")
        self.status_frame.grid(row=3, column=0, columnspan=2, sticky='ew', pady=10)
        
        self.python_status = ttk.Label(self.status_frame, text="Python: Checking...")
        self.python_status.grid(row=0, column=0, sticky='w', pady=2)
        
        self.pyserial_status = ttk.Label(self.status_frame, text="PySerial: Checking...")
        self.pyserial_status.grid(row=1, column=0, sticky='w', pady=2)
        
        self.wireshark_status = ttk.Label(self.status_frame, text="Wireshark: Checking...")
        self.wireshark_status.grid(row=2, column=0, sticky='w', pady=2)
        
        self.log_frame = ttk.LabelFrame(main_frame, text="Installation Log", padding="5")
        self.log_frame.grid(row=4, column=0, columnspan=2, sticky='nsew', pady=10)
        
        self.log_text = scrolledtext.ScrolledText(self.log_frame, height=10, width=65, state='disabled')
        self.log_text.grid(row=0, column=0, sticky='nsew')
        
        button_frame = ttk.Frame(main_frame)
        button_frame.grid(row=5, column=0, columnspan=2, pady=10)
        
        self.install_btn = ttk.Button(button_frame, text="Install", command=self.start_installation, 
                                      state='disabled')
        self.install_btn.grid(row=0, column=0, padx=5)
        
        self.uninstall_btn = ttk.Button(button_frame, text="Uninstall", command=self.uninstall)
        self.uninstall_btn.grid(row=0, column=1, padx=5)
        
        self.close_btn = ttk.Button(button_frame, text="Close", command=self.root.quit)
        self.close_btn.grid(row=0, column=2, padx=5)
        
        self.progress = ttk.Progressbar(main_frame, mode='indeterminate')
        self.progress.grid(row=6, column=0, columnspan=2, sticky='ew', pady=5)
    
    def log(self, message):
        self.log_text.config(state='normal')
        self.log_text.insert(tk.END, message + '\n')
        self.log_text.see(tk.END)
        self.log_text.config(state='disabled')
    
    def check_environment(self):
        threading.Thread(target=self._check_environment_thread, daemon=True).start()
    
    def _check_environment_thread(self):
        self.log("Checking system requirements...")
        
        try:
            result = subprocess.run([sys.executable, '--version'], capture_output=True, text=True)
            python_version = result.stdout.strip()
            self.python_status.config(text=f"Python: ✓ {python_version}", foreground='green')
            self.log(f"✓ Python found: {python_version}")
        except:
            self.python_status.config(text="Python: ✗ Not found", foreground='red')
            self.log("✗ Python not found")
            return
        
        try:
            import serial
            self.pyserial_status.config(text="PySerial: ✓ Installed", foreground='green')
            self.log("✓ PySerial is installed")
        except ImportError:
            self.pyserial_status.config(text="PySerial: ⚠ Not installed (will auto-install)", foreground='orange')
            self.log("⚠ PySerial not installed - will be installed automatically")
        
        extcap_paths = [
            os.path.join(os.environ.get('APPDATA', ''), 'Wireshark', 'extcap'),
            r'C:\Program Files\Wireshark\extcap'
        ]
        
        wireshark_found = False
        for path in extcap_paths:
            if os.path.exists(path):
                self.extcap_path = path
                wireshark_found = True
                self.wireshark_status.config(text=f"Wireshark: ✓ Found at {path}", foreground='green')
                self.log(f"✓ Wireshark extcap folder: {path}")
                break
        
        if not wireshark_found:
            self.extcap_path = extcap_paths[0]
            self.wireshark_status.config(text="Wireshark: ⚠ Will create extcap folder", foreground='orange')
            self.log(f"⚠ Wireshark extcap folder not found - will create: {self.extcap_path}")
        
        self.install_btn.config(state='normal')
        self.log("\nReady to install. Click 'Install' to continue.")
    
    def start_installation(self):
        self.install_btn.config(state='disabled')
        self.progress.start()
        threading.Thread(target=self._install_thread, daemon=True).start()
    
    def _install_thread(self):
        try:
            self.log("\n=== Starting Installation ===")
            
            try:
                import serial
                self.log("✓ PySerial already installed")
            except ImportError:
                self.log("Installing PySerial dependency...")
                try:
                    result = subprocess.run(
                        [sys.executable, '-m', 'pip', 'install', 'pyserial'],
                        capture_output=True,
                        text=True,
                        check=True
                    )
                    self.log("✓ PySerial installed successfully")
                except subprocess.CalledProcessError as e:
                    self.log(f"✗ Failed to install PySerial: {e.stderr}")
                    raise Exception("PySerial installation failed. Please install manually with: pip install pyserial")
            
            os.makedirs(self.extcap_path, exist_ok=True)
            self.log(f"✓ Created extcap directory: {self.extcap_path}")
            
            script_dir = os.path.dirname(os.path.abspath(__file__))
            
            files_to_copy = ['ghostesp_extcap.py', 'ghostesp_extcap.bat']
            for filename in files_to_copy:
                src = os.path.join(script_dir, filename)
                dst = os.path.join(self.extcap_path, filename)
                
                if os.path.exists(src):
                    shutil.copy2(src, dst)
                    self.log(f"✓ Copied {filename} to {self.extcap_path}")
                else:
                    self.log(f"✗ Error: {filename} not found in installer directory")
                    raise FileNotFoundError(f"{filename} not found")
            
            self.log("\n=== Installation Complete ===")
            self.log("✓ GhostESP Wireshark extcap installed successfully!")
            self.log("\nNext steps:")
            self.log("1. Restart Wireshark")
            self.log("2. Look for 'GhostESP WiFi/BLE Capture' in the interface list")
            self.log("3. Double-click it to configure COM port")
            self.log("4. Click Start to begin capturing")
            
            self.progress.stop()
            self.root.after(0, lambda: messagebox.showinfo("Success", 
                "Installation complete!\n\nRestart Wireshark to see the GhostESP interface."))
            
        except Exception as e:
            self.log(f"\n✗ Installation failed: {str(e)}")
            self.progress.stop()
            self.root.after(0, lambda: messagebox.showerror("Error", f"Installation failed:\n{str(e)}"))
        finally:
            self.install_btn.config(state='normal')
    
    def uninstall(self):
        if not messagebox.askyesno("Confirm Uninstall", 
                                   "Are you sure you want to uninstall GhostESP Wireshark extcap?"):
            return
        
        self.log("\n=== Starting Uninstallation ===")
        
        try:
            files_to_remove = ['ghostesp_extcap.py', 'ghostesp_extcap.bat']
            for filename in files_to_remove:
                filepath = os.path.join(self.extcap_path, filename)
                if os.path.exists(filepath):
                    os.remove(filepath)
                    self.log(f"✓ Removed {filename}")
            
            self.log("\n✓ Uninstallation complete")
            messagebox.showinfo("Success", "GhostESP Wireshark extcap uninstalled successfully!")
            
        except Exception as e:
            self.log(f"\n✗ Uninstallation failed: {str(e)}")
            messagebox.showerror("Error", f"Uninstallation failed:\n{str(e)}")

def main():
    root = tk.Tk()
    app = GhostESPInstallerGUI(root)
    root.mainloop()

if __name__ == "__main__":
    main()
