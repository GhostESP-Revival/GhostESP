import argparse
import socket
import struct
import subprocess
import sys
import time
import warnings


def install_and_import(package, import_name=None):
    if import_name is None:
        import_name = package
    try:
        return __import__(import_name)
    except ImportError:
        print(f"Installing {package}...")
        subprocess.check_call([sys.executable, "-m", "pip", "install", package])
        return __import__(import_name)


np = install_and_import("numpy")


def patch_numpy_fromstring_for_soundcard():
    original_fromstring = np.fromstring

    def compat_fromstring(string, dtype=float, count=-1, sep="", *, like=None):
        if sep == "":
            view = memoryview(string)
            return np.frombuffer(view, dtype=dtype, count=count)
        return original_fromstring(string, dtype=dtype, count=count, sep=sep, like=like)

    np.fromstring = compat_fromstring


patch_numpy_fromstring_for_soundcard()
soundcard = install_and_import("soundcard")
serial = install_and_import("pyserial", "serial")
serial_tools = install_and_import("pyserial", "serial.tools.list_ports")


UDP_PORT = 6677
DISCOVERY_PORT = 6678
DISCOVERY_MAGIC = b"GHOSTESP_RAVE_DISCOVER_V1"
DISCOVERY_REPLY = b"GHOSTESP_RAVE_HELPER_V1"
NUM_BARS = 15
TRACK_LEN = 32
ARTIST_LEN = 32
FRAME_TIME = 1.0 / 60.0
DEFAULT_TRACK = "Windows Media Ghost"
DEFAULT_ARTIST = "Desktop Audio"
SERIAL_MARKER = bytes((0xA5, 0x5A, 0xC3, 0x3C))
PROBE_TIMEOUT = 1.8
READY_TIMEOUT = 8.0
OPEN_RETRY_COUNT = 20
OPEN_RETRY_DELAY = 0.25


def is_render_loopback(device):
    device_id = getattr(device, "id", "") or ""
    return device_id.startswith("{0.0.0.")


def get_playback_sources():
    microphones = soundcard.all_microphones(include_loopback=True)
    return [mic for mic in microphones if is_render_loopback(mic)]


def list_loopback_sources():
    microphones = get_playback_sources()
    rows = []
    for idx, mic in enumerate(microphones):
        name = getattr(mic, "name", f"Source {idx}")
        rows.append((idx, name, True))
    return rows


def choose_source(index=None):
    microphones = get_playback_sources()
    if not microphones:
        microphones = soundcard.all_microphones(include_loopback=True)

    if index is not None:
        if index < 0 or index >= len(microphones):
            raise ValueError(f"Playback source index {index} is out of range")
        return microphones[index]

    try:
        default_speaker = soundcard.default_speaker()
        default_name = getattr(default_speaker, "name", "")
    except Exception:
        default_name = ""

    if default_name:
        for mic in microphones:
            if getattr(mic, "name", "") == default_name:
                return mic
        for mic in microphones:
            mic_name = getattr(mic, "name", "")
            if default_name and default_name in mic_name:
                return mic

    for mic in microphones:
        mic_name = getattr(mic, "name", "").lower()
        if "speaker" in mic_name or "headphone" in mic_name or "output" in mic_name:
            return mic

    if microphones:
        return microphones[0]

    return soundcard.default_microphone()


def normalize_text(value, fallback):
    text = (value or fallback).strip() or fallback
    encoded = text.encode("utf-8", errors="ignore")[:TRACK_LEN]
    return encoded.ljust(TRACK_LEN, b"\0")


def build_band_edges(sample_rate):
    upper = min(12000.0, sample_rate / 2)
    return np.geomspace(20.0, upper, NUM_BARS + 1)


def compute_bands(samples, sample_rate, previous):
    if samples.ndim > 1:
        mono = samples.mean(axis=1)
    else:
        mono = samples

    mono = mono.astype(np.float32)
    if mono.size == 0:
        return previous

    window = np.hanning(mono.size)
    spectrum = np.abs(np.fft.rfft(mono * window))
    freqs = np.fft.rfftfreq(mono.size, 1.0 / sample_rate)
    edges = build_band_edges(sample_rate)

    bands = np.zeros(NUM_BARS, dtype=np.float32)
    for i in range(NUM_BARS):
        mask = (freqs >= edges[i]) & (freqs < edges[i + 1])
        if np.any(mask):
            bands[i] = float(np.mean(spectrum[mask]))

    bands = np.sqrt(np.maximum(bands, 0.0))

    smoothed_bands = bands.copy()
    for i in range(NUM_BARS):
        left = bands[i - 1] if i > 0 else bands[i]
        right = bands[i + 1] if i < (NUM_BARS - 1) else bands[i]
        smoothed_bands[i] = (bands[i] * 0.6) + (left * 0.2) + (right * 0.2)
    bands = smoothed_bands

    display_bands = bands.copy()
    if NUM_BARS >= 4:
        display_bands[0] = (bands[0] * 0.7) + (bands[1] * 0.3)
        display_bands[1] = (bands[0] * 0.30) + (bands[1] * 0.45) + (bands[2] * 0.25)
        display_bands[-2] = (bands[-3] * 0.25) + (bands[-2] * 0.45) + (bands[-1] * 0.30)
        display_bands[-1] = (bands[-2] * 0.3) + (bands[-1] * 0.7)
    bands = display_bands

    peak = float(np.max(bands))
    if peak > 0.0:
        bands = bands / peak

    weights = np.linspace(1.3, 0.95, NUM_BARS, dtype=np.float32)
    bands *= weights
    bands = np.clip(bands, 0.0, 1.0)

    rise = 0.72
    fall = 0.28
    result = previous.copy()
    for i in range(NUM_BARS):
        factor = rise if bands[i] > result[i] else fall
        result[i] = result[i] + ((bands[i] - result[i]) * factor)

    return np.clip(result * 255.0, 0, 255).astype(np.uint8)


def compute_input_level_percent(samples):
    if samples is None:
        return 0
    if samples.ndim > 1:
        mono = samples.mean(axis=1)
    else:
        mono = samples
    mono = mono.astype(np.float32)
    if mono.size == 0:
        return 0
    rms = float(np.sqrt(np.mean(np.square(mono))))
    if rms <= 0.0:
        return 0
    db = 20.0 * np.log10(max(rms, 1e-7))
    if db <= -70.0:
        return 0
    if db >= -10.0:
        return 100
    return int(((db + 70.0) / 60.0) * 100.0)


def build_packet(track_name, artist_name, amplitudes):
    packet = normalize_text(track_name, DEFAULT_TRACK)
    packet += normalize_text(artist_name, DEFAULT_ARTIST)
    packet += struct.pack(f"{NUM_BARS}B", *amplitudes)
    return packet


def parse_args():
    parser = argparse.ArgumentParser(description="Windows-first GhostESP Rave helper")
    parser.add_argument("target", nargs="?", help="Target device IP or broadcast address")
    parser.add_argument("--serial", help="Serial port instead of UDP, e.g. COM3")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate (default: 115200)")
    parser.add_argument("--list-ports", action="store_true", help="List serial ports")
    parser.add_argument("--list-usable-ports", action="store_true", help="Probe serial ports and show GhostESP-capable ones")
    parser.add_argument("--pick-port", action="store_true", help="Probe GhostESP serial ports and choose one interactively")
    parser.add_argument("--auto-port", action="store_true", help="Auto-select first detected GhostESP serial port")
    parser.add_argument("--port", type=int, default=UDP_PORT, help="UDP port (default: 6677)")
    parser.add_argument("--fps", type=int, default=60, help="Frame rate (default: 60)")
    parser.add_argument("--no-discovery", action="store_true", help="Disable UDP auto-discovery mode")
    parser.add_argument("--track", default=DEFAULT_TRACK, help="Track label shown on device")
    parser.add_argument("--artist", default=DEFAULT_ARTIST, help="Artist/source label shown on device")
    parser.add_argument("--source", type=int, help="Playback output source index")
    parser.add_argument("--list-sources", action="store_true", help="List available Windows playback output capture devices")
    parser.add_argument("--show-audio-warnings", action="store_true", help="Show soundcard discontinuity warnings")
    return parser.parse_args()


def print_banner(target, source_name, fps):
    print("GhostESP Rave Helper")
    print("-" * 24)
    print(f"Target : {target}")
    print(f"Source : {source_name}")
    print(f"FPS    : {fps}")
    print("Press Ctrl+C to stop.\n")


def list_serial_ports():
    for port in serial.tools.list_ports.comports():
        if not getattr(port, "device", None):
            continue
        desc = f" - {port.description}" if port.description else ""
        print(f"{port.device}{desc}")


def create_serial_handle(device, baud, timeout=0.2, write_timeout=0.2):
    last_error = None
    for attempt in range(OPEN_RETRY_COUNT):
        handle = serial.Serial()
        handle.port = device
        handle.baudrate = baud
        handle.timeout = timeout
        handle.write_timeout = write_timeout
        handle.dtr = False
        handle.rts = False
        try:
            handle.open()
            handle.setDTR(False)
            handle.setRTS(False)
            return handle
        except Exception as exc:
            last_error = exc
            try:
                handle.close()
            except Exception:
                pass
            if attempt < OPEN_RETRY_COUNT - 1:
                time.sleep(OPEN_RETRY_DELAY)
    raise last_error


def probe_serial_port(device, baud, timeout=PROBE_TIMEOUT):
    handle = None
    try:
        handle = create_serial_handle(device, baud, timeout=0.15, write_timeout=0.5)
        time.sleep(0.35)
        handle.reset_input_buffer()
        handle.reset_output_buffer()
        handle.write(b"\nidentify\nraveport\n")

        deadline = time.time() + timeout
        response = bytearray()
        while time.time() < deadline:
            chunk = handle.read(512)
            if chunk:
                response.extend(chunk)
                text = response.decode("utf-8", errors="ignore")
                if "GHOSTESP_OK" in text and "RAVE_SERIAL " in text:
                    rave_line = ""
                    for line in text.splitlines():
                        if line.startswith("RAVE_SERIAL "):
                            rave_line = line.strip()
                            break
                    return {
                        "device": device,
                        "ok": True,
                        "rave_line": rave_line,
                        "raw": text,
                    }
            time.sleep(0.05)
        return {"device": device, "ok": False, "reason": "No GhostESP response"}
    except Exception as exc:
        return {"device": device, "ok": False, "reason": str(exc)}
    finally:
        if handle is not None:
            try:
                handle.close()
            except Exception:
                pass


def detect_usable_ports(baud):
    usable = []
    rejected = []
    for port in serial.tools.list_ports.comports():
        result = probe_serial_port(port.device, baud)
        result["description"] = port.description or ""
        result["hwid"] = getattr(port, "hwid", "")
        if result.get("ok"):
            usable.append(result)
        else:
            rejected.append(result)
    return usable, rejected


def print_usable_ports(ports):
    if not ports:
        print("No usable GhostESP ports detected.")
        return
    print("Usable GhostESP ports:")
    for idx, port in enumerate(ports, start=1):
        desc = f" - {port['description']}" if port.get("description") else ""
        extra = f" [{port['rave_line']}]" if port.get("rave_line") else ""
        print(f"{idx}. {port['device']}{desc}{extra}")


def choose_detected_port(baud):
    usable, rejected = detect_usable_ports(baud)
    print_usable_ports(usable)
    if usable:
        if len(usable) == 1:
            print(f"\nUsing detected GhostESP port: {usable[0]['device']}")
            return usable[0]["device"]

        print()
        choice = input(f"Choose a port [1-{len(usable)}]: ").strip()
        if not choice.isdigit():
            raise ValueError("Invalid port selection")
        index = int(choice) - 1
        if index < 0 or index >= len(usable):
            raise ValueError("Selected port is out of range")
        return usable[index]["device"]

    print("\nAll detected serial ports:")
    list_serial_ports()
    if rejected:
        print("\nNone replied to GhostESP probe. The device may still be booting or using a different baud rate.")
    manual = input("Enter a serial port manually (or leave blank to cancel): ").strip()
    if not manual:
        raise ValueError("No serial port selected")
    print(f"Waiting briefly before opening {manual}...")
    time.sleep(1.0)
    return manual


class UdpTransport:
    def __init__(self, target, port, enable_discovery=False):
        self.target = target
        self.port = port
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        self.frames_sent = 0
        self.discovery_sock = None
        self.discovered_target = None

        if enable_discovery:
            try:
                self.discovery_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                self.discovery_sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
                self.discovery_sock.bind(("0.0.0.0", DISCOVERY_PORT))
                self.discovery_sock.setblocking(False)
                print(f"UDP discovery service listening on {DISCOVERY_PORT}")
            except Exception as exc:
                self.discovery_sock = None
                print(f"UDP discovery unavailable ({exc}), falling back to target routing.")

    def _poll_discovery(self):
        if self.discovery_sock is None:
            return

        while True:
            try:
                data, source = self.discovery_sock.recvfrom(256)
            except (BlockingIOError, InterruptedError):
                break
            except Exception:
                break

            if not data.startswith(DISCOVERY_MAGIC):
                continue

            source_ip = source[0]
            if source_ip != self.discovered_target:
                self.discovered_target = source_ip
                print(f"Discovered GhostESP at {source_ip}")

            try:
                self.discovery_sock.sendto(DISCOVERY_REPLY, (source_ip, DISCOVERY_PORT))
            except Exception:
                pass

    def send(self, packet):
        self._poll_discovery()
        destination = self.discovered_target or self.target
        if not destination:
            return
        self.sock.sendto(packet, (destination, self.port))
        self.frames_sent += 1

    def close(self):
        if self.discovery_sock is not None:
            self.discovery_sock.close()
        self.sock.close()


class SerialTransport:
    def __init__(self, port, baud):
        self.device = create_serial_handle(port, baud, timeout=0.2, write_timeout=0.2)
        self.device.reset_input_buffer()
        self.device.reset_output_buffer()
        time.sleep(0.2)
        self.frames_sent = 0

    def read_available_text(self, duration=0.25):
        deadline = time.time() + duration
        response = bytearray()
        while time.time() < deadline:
            waiting = self.device.in_waiting
            if waiting:
                response.extend(self.device.read(waiting))
            time.sleep(0.02)
        return response.decode("utf-8", errors="ignore")

    def send_command(self, command, settle=0.2):
        self.device.write((command.strip() + "\n").encode("utf-8"))
        self.device.flush()
        if settle > 0:
            time.sleep(settle)

    def wait_for_ready(self, timeout=READY_TIMEOUT):
        deadline = time.time() + timeout
        response = bytearray()
        while time.time() < deadline:
            self.send_command("identify", settle=0.1)
            self.send_command("raveport", settle=0.1)
            waiting = time.time() + 0.35
            while time.time() < waiting:
                chunk = self.device.read(256)
                if chunk:
                    response.extend(chunk)
                    text = response.decode("utf-8", errors="ignore")
                    if "GHOSTESP_OK" in text and "RAVE_SERIAL " in text:
                        return text
                time.sleep(0.02)
        return response.decode("utf-8", errors="ignore")

    def send(self, packet):
        amplitudes = packet[64:64 + NUM_BARS]
        binary = SERIAL_MARKER + struct.pack("<H", NUM_BARS) + bytes(amplitudes)
        self.device.write(binary)
        self.frames_sent += 1

    def print_stats(self):
        print(f"Frames sent over serial: {self.frames_sent}")

    def close(self):
        self.device.close()


def close_recorder_safely(recorder):
    if recorder is None:
        return
    try:
        recorder.__exit__(None, None, None)
    except Exception:
        pass


def main():
    args = parse_args()

    if not args.show_audio_warnings:
        warnings.filterwarnings(
            "ignore",
            message="data discontinuity in recording",
            category=getattr(soundcard, "SoundcardRuntimeWarning", Warning),
        )

    if args.list_ports:
        list_serial_ports()
        return 0

    if args.list_usable_ports:
        usable, _ = detect_usable_ports(args.baud)
        print_usable_ports(usable)
        return 0 if usable else 1

    if args.list_sources:
        for idx, name, is_loopback in list_loopback_sources():
            suffix = " [loopback]" if is_loopback else ""
            print(f"{idx}: {name}{suffix}")
        return 0

    if args.pick_port:
        try:
            args.serial = choose_detected_port(args.baud)
        except ValueError as exc:
            print(f"Port selection cancelled: {exc}")
            return 2

    if args.auto_port:
        usable, _ = detect_usable_ports(args.baud)
        if not usable:
            print("No usable GhostESP serial ports detected.")
            print("Open Visualizer on device, check cable/baud, or use manual USB port mode.")
            return 2
        args.serial = usable[0]["device"]
        print(f"Using detected GhostESP port: {args.serial}")

    discovery_enabled = (not args.serial) and (args.target is None) and (not args.no_discovery)
    target = args.target or "255.255.255.255"
    speaker = choose_source(args.source)
    sample_rate = 48000
    samples_per_frame = max(256, int(sample_rate / max(1, args.fps)))
    frame_time = 1.0 / max(1, args.fps)

    transport = SerialTransport(args.serial, args.baud) if args.serial else UdpTransport(
        target,
        args.port,
        enable_discovery=discovery_enabled,
    )
    target_label = f"serial {args.serial} @ {args.baud}" if args.serial else (
        f"auto-discovery + {target}:{args.port}" if discovery_enabled else f"{target}:{args.port}"
    )

    previous = np.zeros(NUM_BARS, dtype=np.float32)
    print_banner(target_label, getattr(speaker, "name", "Desktop Audio"), args.fps)

    if args.serial:
        print("Waiting for device to settle...")
        time.sleep(2.2)
        ready_text = transport.wait_for_ready()
        if "GHOSTESP_OK" not in ready_text:
            print("Warning: device did not confirm ready state over serial.")
            if ready_text.strip():
                print(ready_text.strip())
        transport.send_command("rave on", settle=0.35)
        post_open = transport.read_available_text(0.4)
        if post_open.strip():
            print(post_open.strip())
        print("Opened Rave Mode on device.")
        print("If Live level stays at 0%%, desktop audio capture is not coming through.")

    try:
        last_status_time = time.time()
        last_peak_percent = 0
        last_input_percent = 0
        next_frame_time = time.perf_counter()
        recorder = None
        recovering_capture = False
        while True:
            if recorder is None:
                try:
                    recorder = speaker.recorder(samplerate=sample_rate, channels=2)
                    recorder.__enter__()
                    if recovering_capture:
                        print("Audio capture recovered.")
                    recovering_capture = False
                    next_frame_time = time.perf_counter()
                except Exception as exc:
                    print(f"Audio capture unavailable ({exc}). Retrying...")
                    recovering_capture = True
                    time.sleep(0.5)
                    continue

            try:
                data = recorder.record(numframes=samples_per_frame)
            except RuntimeError as exc:
                print(f"Audio capture error ({exc}). Reinitializing capture...")
                close_recorder_safely(recorder)
                recorder = None
                recovering_capture = True
                time.sleep(0.35)
                continue

            try:
                amplitudes = compute_bands(data, sample_rate, previous)
                previous = amplitudes.astype(np.float32) / 255.0
                packet = build_packet(args.track, args.artist, amplitudes)
                transport.send(packet)
                last_peak_percent = (int(np.max(amplitudes)) * 100) // 255
                last_input_percent = compute_input_level_percent(data)

                now = time.time()
                if now - last_status_time >= 1.0:
                    print(
                        f"Input level: {last_input_percent:3d}% | Spectrum peak: {last_peak_percent:3d}% | Frames sent: {getattr(transport, 'frames_sent', 0)}"
                    )
                    last_status_time = now

                next_frame_time += frame_time
                sleep_for = next_frame_time - time.perf_counter()
                if sleep_for > 0:
                    time.sleep(sleep_for)
                else:
                    next_frame_time = time.perf_counter()
            except Exception as exc:
                print(f"Processing error ({exc}). Reinitializing capture...")
                close_recorder_safely(recorder)
                recorder = None
                recovering_capture = True
                time.sleep(0.35)
    except KeyboardInterrupt:
        print("\nStopping helper.")
    finally:
        close_recorder_safely(locals().get("recorder"))
        if args.serial and hasattr(transport, "print_stats"):
            transport.print_stats()
        transport.close()

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\nStopping helper.")
        raise SystemExit(130)
    except Exception as exc:
        print(f"Fatal error: {exc}")
        raise SystemExit(1)
