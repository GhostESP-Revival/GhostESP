#![cfg_attr(target_os = "windows", windows_subsystem = "windows")]

use std::collections::hash_map::DefaultHasher;
use std::fs::{self, OpenOptions};
use std::hash::{Hash, Hasher};
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::time::Duration;

use tinyfiledialogs::{input_box, message_box_ok, MessageBoxIcon};
use tray_icon::menu::{Menu, MenuEvent, MenuItem, PredefinedMenuItem};
use tray_icon::{Icon, TrayIconBuilder};

#[cfg(windows)]
use std::os::windows::process::CommandExt;

#[cfg(windows)]
const CREATE_NO_WINDOW: u32 = 0x08000000;

#[cfg(windows)]
fn pump_windows_messages() {
    use windows_sys::Win32::UI::WindowsAndMessaging::{
        DispatchMessageW, PeekMessageW, TranslateMessage, MSG, PM_REMOVE,
    };

    unsafe {
        let mut msg: MSG = std::mem::zeroed();
        while PeekMessageW(&mut msg, std::ptr::null_mut(), 0, 0, PM_REMOVE) != 0 {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

#[cfg(not(windows))]
fn pump_windows_messages() {}

#[derive(Default)]
struct AppState {
    child: Option<Child>,
    script_dir: PathBuf,
    source_index: Option<i32>,
    fps: u32,
    usb_baud: u32,
    wifi_target: String,
    usb_port: String,
    stop_requested: bool,
    active_mode: String,
}

include!(concat!(env!("OUT_DIR"), "/embedded_worker.rs"));

fn show_error(title: &str, message: &str) {
    let _ = message_box_ok(title, message, MessageBoxIcon::Error);
}

fn show_info(title: &str, message: &str) {
    let _ = message_box_ok(title, message, MessageBoxIcon::Info);
}

fn status_log_path() -> PathBuf {
    std::env::temp_dir()
        .join("ghostesp_rave")
        .join("rave_tray.log")
}

fn set_status(status_item: &MenuItem, tray: &tray_icon::TrayIcon, status: &str) {
    status_item.set_text(format!("Status: {status}"));
    let _ = tray.set_tooltip(Some(format!("GhostESP Visualizer - {status}")));
}

fn open_log_file() -> io::Result<()> {
    let log_path = status_log_path();

    #[cfg(windows)]
    {
        Command::new("cmd")
            .arg("/C")
            .arg("start")
            .arg("")
            .arg(log_path)
            .spawn()
            .map(|_| ())
    }

    #[cfg(not(windows))]
    {
        Command::new("xdg-open").arg(log_path).spawn().map(|_| ())
    }
}

fn prompt_value(title: &str, message: &str, default: &str) -> Option<String> {
    input_box(title, message, default).map(|v| v.trim().to_string())
}

fn make_icon() -> io::Result<Icon> {
    let mut rgba = vec![0_u8; 16 * 16 * 4];
    for y in 0..16 {
        for x in 0..16 {
            let i = ((y * 16) + x) * 4;
            let in_frame = x >= 1 && x <= 14 && y >= 1 && y <= 14;
            let frame = x == 1 || x == 14 || y == 1 || y == 14;

            let bar1 = x == 4 && y >= 9 && y <= 12;
            let bar2 = x == 6 && y >= 7 && y <= 12;
            let bar3 = x == 8 && y >= 5 && y <= 12;
            let bar4 = x == 10 && y >= 3 && y <= 12;
            let baseline = y == 12 && x >= 3 && x <= 11;

            let (r, g, b) = if frame {
                (0x28, 0x33, 0x40)
            } else if bar1 || bar2 || bar3 || bar4 || baseline {
                (0x00, 0xD7, 0xA8)
            } else if in_frame {
                (0x12, 0x17, 0x20)
            } else {
                (0x00, 0x00, 0x00)
            };
            rgba[i] = r;
            rgba[i + 1] = g;
            rgba[i + 2] = b;
            rgba[i + 3] = if in_frame { 0xFF } else { 0x00 };
        }
    }

    Icon::from_rgba(rgba, 16, 16)
        .map_err(|e| io::Error::new(io::ErrorKind::Other, format!("icon build failed: {e}")))
}

fn detect_script_dir() -> io::Result<PathBuf> {
    let exe = std::env::current_exe()?;
    if let Some(parent) = exe.parent() {
        return Ok(parent.to_path_buf());
    }

    std::env::current_dir()
}

fn materialize_embedded_worker() -> io::Result<Option<PathBuf>> {
    let bytes = match EMBEDDED_WORKER {
        Some(data) => data,
        None => return Ok(None),
    };

    let mut hasher = DefaultHasher::new();
    bytes.hash(&mut hasher);
    let hash = hasher.finish();

    #[cfg(windows)]
    let worker_name = format!("rave_worker_{hash:016x}.exe");
    #[cfg(not(windows))]
    let worker_name = format!("rave_worker_{hash:016x}");

    let base = std::env::temp_dir().join("ghostesp_rave");
    fs::create_dir_all(&base)?;
    let worker_path = base.join(worker_name);

    let needs_write = match fs::metadata(&worker_path) {
        Ok(meta) => meta.len() != bytes.len() as u64,
        Err(_) => true,
    };

    if needs_write {
        let mut file = OpenOptions::new()
            .create(true)
            .truncate(true)
            .write(true)
            .open(&worker_path)?;
        file.write_all(bytes)?;
        file.flush()?;
    }

    Ok(Some(worker_path))
}

fn find_python_command() -> io::Result<(String, Vec<String>)> {
    let py3 = Command::new("py")
        .arg("-3")
        .arg("-V")
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status();
    if matches!(py3, Ok(status) if status.success()) {
        return Ok(("py".to_string(), vec!["-3".to_string()]));
    }

    let python = Command::new("python")
        .arg("-V")
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status();
    if matches!(python, Ok(status) if status.success()) {
        return Ok(("python".to_string(), Vec::new()));
    }

    Err(io::Error::new(
        io::ErrorKind::NotFound,
        "Python 3 not found on PATH (tried py -3 and python)",
    ))
}

fn spawn_logged(mut cmd: Command, log_path: &Path) -> io::Result<Child> {
    let log_out = OpenOptions::new()
        .create(true)
        .append(true)
        .open(log_path)?;
    let log_err = log_out.try_clone()?;

    cmd.stdin(Stdio::null());
    cmd.stdout(Stdio::from(log_out));
    cmd.stderr(Stdio::from(log_err));

    #[cfg(windows)]
    {
        cmd.creation_flags(CREATE_NO_WINDOW);
    }

    cmd.spawn()
}

fn spawn_visualizer(script_dir: &Path, args: &[String]) -> io::Result<Child> {
    let log_dir = std::env::temp_dir().join("ghostesp_rave");
    fs::create_dir_all(&log_dir)?;
    let log_path = status_log_path();

    if let Some(worker) = materialize_embedded_worker()? {
        let mut cmd = Command::new(worker);
        cmd.current_dir(script_dir);
        for arg in args {
            cmd.arg(arg);
        }
        return spawn_logged(cmd, &log_path);
    }

    #[cfg(windows)]
    let worker = script_dir.join("rave_worker.exe");
    #[cfg(not(windows))]
    let worker = script_dir.join("rave_worker");
    if worker.exists() {
        let mut cmd = Command::new(worker);
        cmd.current_dir(script_dir);
        for arg in args {
            cmd.arg(arg);
        }
        return spawn_logged(cmd, &log_path);
    }

    let (python_exe, python_args) = find_python_command()?;
    let script = script_dir.join("_internal").join("Display_Visualizer.py");
    let mut cmd = Command::new(python_exe);
    for arg in python_args {
        cmd.arg(arg);
    }
    cmd.arg(script);
    for arg in args {
        cmd.arg(arg);
    }
    cmd.current_dir(script_dir);
    spawn_logged(cmd, &log_path)
}

fn open_interactive_launcher(script_dir: &Path) -> io::Result<()> {
    #[cfg(windows)]
    {
        let launcher = script_dir.join("rave_helper.bat");
        if !launcher.exists() {
            return Err(io::Error::new(
                io::ErrorKind::NotFound,
                "rave_helper.bat not found",
            ));
        }

        Command::new("cmd")
            .arg("/C")
            .arg("start")
            .arg("")
            .arg(launcher)
            .spawn()
            .map(|_| ())
    }

    #[cfg(not(windows))]
    {
        let launcher = script_dir.join("rave_helper.sh");
        if launcher.exists() {
            return Command::new("sh").arg(launcher).spawn().map(|_| ());
        }

        Err(io::Error::new(
            io::ErrorKind::NotFound,
            "No interactive launcher found for this platform",
        ))
    }
}

fn stop_visualizer(state: &mut AppState) {
    if let Some(mut child) = state.child.take() {
        let _ = child.kill();
        let _ = child.wait();
    }
}

fn append_common_args(state: &AppState, args: &mut Vec<String>) {
    args.push("--fps".to_string());
    args.push(state.fps.to_string());

    if let Some(source) = state.source_index {
        args.push("--source".to_string());
        args.push(source.to_string());
    }
}

fn start_with_args(
    state: &mut AppState,
    args: Vec<String>,
    mode_label: &str,
    status_label: &str,
    status_item: &MenuItem,
    tray: &tray_icon::TrayIcon,
) {
    state.stop_requested = true;
    stop_visualizer(state);
    state.stop_requested = false;

    set_status(status_item, tray, &format!("connecting {status_label}"));
    match spawn_visualizer(&state.script_dir, &args) {
        Ok(child) => {
            state.child = Some(child);
            state.active_mode = status_label.to_string();
            set_status(status_item, tray, &format!("streaming {status_label}"));
            show_info("GhostESP Visualizer", mode_label);
        }
        Err(err) => {
            set_status(status_item, tray, "start failed");
            show_error(
                "Failed to start stream",
                &format!("{err}\n\nLog: {}", status_log_path().display()),
            );
        }
    }
}

fn main() -> io::Result<()> {
    let script_dir = detect_script_dir()?;
    let mut state = AppState {
        child: None,
        script_dir,
        source_index: None,
        fps: 60,
        usb_baud: 115200,
        wifi_target: String::new(),
        usb_port: String::new(),
        stop_requested: false,
        active_mode: "idle".to_string(),
    };

    let menu = Menu::new();
    let status_item = MenuItem::new("Status: idle", false, None);
    let show_status = MenuItem::new("Show Status", true, None);
    let open_log = MenuItem::new("Open Log", true, None);
    let start_wifi_auto = MenuItem::new("Start Wi-Fi (Auto)", true, None);
    let start_wifi_target = MenuItem::new("Start Wi-Fi (Set IP...)", true, None);
    let start_usb_auto = MenuItem::new("Start USB (Auto Port)", true, None);
    let start_usb_manual = MenuItem::new("Start USB (Manual Port...)", true, None);
    let set_source = MenuItem::new("Set Audio Source...", true, None);
    let set_fps = MenuItem::new("Set FPS...", true, None);
    let set_baud = MenuItem::new("Set USB Baud...", true, None);
    let open_helper = MenuItem::new("Open Interactive Helper", true, None);
    let stop_item = MenuItem::new("Stop Stream", true, None);
    let sep = PredefinedMenuItem::separator();
    let quit_item = MenuItem::new("Quit", true, None);

    menu.append(&start_wifi_auto)
        .and_then(|_| menu.append(&status_item))
        .and_then(|_| menu.append(&show_status))
        .and_then(|_| menu.append(&open_log))
        .and_then(|_| menu.append(&sep))
        .and_then(|_| menu.append(&start_wifi_target))
        .and_then(|_| menu.append(&start_usb_auto))
        .and_then(|_| menu.append(&start_usb_manual))
        .and_then(|_| menu.append(&sep))
        .and_then(|_| menu.append(&set_source))
        .and_then(|_| menu.append(&set_fps))
        .and_then(|_| menu.append(&set_baud))
        .and_then(|_| menu.append(&open_helper))
        .and_then(|_| menu.append(&stop_item))
        .and_then(|_| menu.append(&sep))
        .and_then(|_| menu.append(&quit_item))
        .map_err(|e| io::Error::new(io::ErrorKind::Other, format!("menu setup failed: {e}")))?;

    let tray = TrayIconBuilder::new()
        .with_tooltip("GhostESP Visualizer")
        .with_icon(make_icon()?)
        .with_menu_on_left_click(false)
        .with_menu(Box::new(menu))
        .build()
        .map_err(|e| io::Error::new(io::ErrorKind::Other, format!("tray init failed: {e}")))?;

    set_status(&status_item, &tray, "idle");

    let menu_events = MenuEvent::receiver();

    loop {
        pump_windows_messages();

        while let Ok(event) = menu_events.try_recv() {
            if event.id == start_wifi_auto.id() {
                let mut args = Vec::new();
                append_common_args(&state, &mut args);
                start_with_args(
                    &mut state,
                    args,
                    "Streaming over Wi-Fi (auto-discovery)",
                    "wifi auto",
                    &status_item,
                    &tray,
                );
            } else if event.id == show_status.id() {
                let msg = if state.child.is_some() {
                    format!("Currently streaming: {}", state.active_mode)
                } else {
                    "Currently stopped".to_string()
                };
                show_info("GhostESP Visualizer Status", &msg);
            } else if event.id == open_log.id() {
                if let Err(err) = open_log_file() {
                    show_error("Open Log Failed", &err.to_string());
                }
            } else if event.id == start_wifi_target.id() {
                let current = state.wifi_target.clone();
                if let Some(target) = prompt_value(
                    "Wi-Fi Target",
                    "Enter device IP (leave blank for auto-discovery):",
                    &current,
                ) {
                    state.wifi_target = target;
                    let mut args = Vec::new();
                    if !state.wifi_target.is_empty() {
                        args.push(state.wifi_target.clone());
                    }
                    append_common_args(&state, &mut args);
                    start_with_args(
                        &mut state,
                        args,
                        "Streaming over Wi-Fi (configured target)",
                        "wifi target",
                        &status_item,
                        &tray,
                    );
                }
            } else if event.id == start_usb_auto.id() {
                let mut args = vec![
                    "--auto-port".to_string(),
                    "--baud".to_string(),
                    state.usb_baud.to_string(),
                ];
                append_common_args(&state, &mut args);
                start_with_args(
                    &mut state,
                    args,
                    "Streaming over USB (auto port)",
                    "usb auto",
                    &status_item,
                    &tray,
                );
            } else if event.id == start_usb_manual.id() {
                let current = state.usb_port.clone();
                if let Some(port) =
                    prompt_value("USB Port", "Enter serial port (e.g. COM3):", &current)
                {
                    if port.is_empty() {
                        show_error("Invalid Port", "Serial port cannot be empty.");
                        continue;
                    }

                    state.usb_port = port;
                    let mut args = vec![
                        "--serial".to_string(),
                        state.usb_port.clone(),
                        "--baud".to_string(),
                        state.usb_baud.to_string(),
                    ];
                    append_common_args(&state, &mut args);
                    start_with_args(
                        &mut state,
                        args,
                        "Streaming over USB (manual port)",
                        "usb manual",
                        &status_item,
                        &tray,
                    );
                }
            } else if event.id == set_source.id() {
                let default = state
                    .source_index
                    .map(|v| v.to_string())
                    .unwrap_or_default();
                if let Some(value) = prompt_value(
                    "Audio Source",
                    "Playback source index (blank = default):",
                    &default,
                ) {
                    if value.is_empty() {
                        state.source_index = None;
                        show_info("Source Updated", "Using default playback source.");
                    } else if let Ok(index) = value.parse::<i32>() {
                        state.source_index = Some(index);
                        show_info("Source Updated", "Using custom playback source index.");
                    } else {
                        show_error("Invalid Source", "Source index must be a number.");
                    }
                }
            } else if event.id == set_fps.id() {
                let default = state.fps.to_string();
                if let Some(value) = prompt_value("Frame Rate", "Set FPS (15-120):", &default) {
                    if let Ok(fps) = value.parse::<u32>() {
                        if (15..=120).contains(&fps) {
                            state.fps = fps;
                            show_info("FPS Updated", &format!("FPS set to {fps}."));
                        } else {
                            show_error("Invalid FPS", "FPS must be between 15 and 120.");
                        }
                    } else {
                        show_error("Invalid FPS", "FPS must be a number.");
                    }
                }
            } else if event.id == set_baud.id() {
                let default = state.usb_baud.to_string();
                if let Some(value) = prompt_value("USB Baud", "Set USB baud rate:", &default) {
                    if let Ok(baud) = value.parse::<u32>() {
                        if baud >= 9600 {
                            state.usb_baud = baud;
                            show_info("USB Baud Updated", &format!("USB baud set to {baud}."));
                        } else {
                            show_error("Invalid Baud", "Baud must be at least 9600.");
                        }
                    } else {
                        show_error("Invalid Baud", "Baud must be a number.");
                    }
                }
            } else if event.id == open_helper.id() {
                if let Err(err) = open_interactive_launcher(&state.script_dir) {
                    show_error("Launcher Error", &err.to_string());
                }
            } else if event.id == stop_item.id() {
                state.stop_requested = true;
                stop_visualizer(&mut state);
                set_status(&status_item, &tray, "stopped");
            } else if event.id == quit_item.id() {
                state.stop_requested = true;
                stop_visualizer(&mut state);
                return Ok(());
            }
        }

        if let Some(child) = state.child.as_mut() {
            match child.try_wait() {
                Ok(Some(status)) => {
                    let expected_stop = state.stop_requested;
                    state.child = None;
                    state.stop_requested = false;
                    state.active_mode = "idle".to_string();

                    if expected_stop {
                        set_status(&status_item, &tray, "stopped");
                    } else if status.success() {
                        set_status(&status_item, &tray, "stopped");
                    } else {
                        let code = status
                            .code()
                            .map(|c| c.to_string())
                            .unwrap_or_else(|| "terminated".to_string());
                        set_status(&status_item, &tray, "stream failed");
                        show_error(
                            "Stream Stopped",
                            &format!(
                                "Worker exited with status {code}.\n\nOpen log for details:\n{}",
                                status_log_path().display()
                            ),
                        );
                    }
                }
                Ok(None) => {}
                Err(_) => {
                    state.child = None;
                    state.stop_requested = false;
                    state.active_mode = "idle".to_string();
                    set_status(&status_item, &tray, "state check failed");
                }
            }
        }

        std::thread::sleep(Duration::from_millis(120));
    }
}
