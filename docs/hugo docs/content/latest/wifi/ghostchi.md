---
title: "Ghostchi"
description: "An autonomous Wi-Fi hunting tamagotchi that learns, adapts, and has opinions."
weight: 35
---

Ghostchi is a tamagotchi that lives on your device and hunts WPA handshakes. It scans for access points, ranks them by likelihood of a successful capture, listens for EAPOL traffic, and if passive listening doesn't work, it forces the issue with a deauth burst. It remembers what worked and what didn't across sessions and reboots. It also has a name, moods, and things to say about it.

> **Legal note**: Only capture traffic from networks you own or have explicit permission to test. Unauthorized network testing is illegal in most jurisdictions.

## How it hunts

The hunting loop runs continuously once you start a session:

1. **Sweep** -- Scans all channels for visible APs, scores each one, picks the best target.
2. **Lock** -- Tunes to the target's channel and listens passively for EAPOL handshakes (2-5 seconds depending on how crowded the area is).
3. **Stim** -- If passive listening didn't catch anything, fires a short deauth burst to force a client reconnect, then listens again.
4. **Cooldown** -- Pauses briefly before starting over. Successes and failures both affect how the ghost targets APs going forward.

The whole cycle runs on a 250ms tick. Deauth is only used once per target attempt -- if the first push doesn't produce a handshake, the ghost gives up on that target and moves on rather than hammering it.

## The scoring system

Every visible AP gets a weighted score before the ghost decides what to go after:

- **Encryption** -- WPA/WPA2/WPA3 networks get +70 points. Open and WEP networks get -40 because they're a waste of time.
- **Signal strength** -- Strong signals (> -55 dBm) get +34. Weak signals (< -75 dBm) get penalized.
- **Visibility** -- Networks broadcasting their SSID get a small bonus over hidden ones.
- **Past results** -- The ghost remembers which APs it's had success with (up to 32 BSSIDs) and prefers them. APs with repeated failures get avoided.
- **Channel history** -- If the ghost previously captured a handshake on a given channel, APs on that channel get a bonus.

Confidence is calculated alongside the score and displayed on-screen as a percentage. The ghost picks the highest-scoring target that isn't on cooldown.

## Adaptive strategy

Timing adjusts based on how many APs are nearby:

| Environment | APs | Passive listen | Settle time | Deauth burst |
|-------------|-----|----------------|-------------|--------------|
| Crowded | 18+ | 2.2s | 850ms | 450ms |
| Normal | 6-17 | 3.4s | 1.2s | 650ms |
| Sparse | 1-5 | 5.0s | 1.5s | 850ms |

More APs means more natural EAPOL traffic, so the ghost can use shorter windows. Fewer APs means it needs to wait longer and push harder.

## The learning system

Ghostchi persists a learning table (`learn.bin`) with up to 32 BSSID entries across reboots. Each entry tracks:

- The AP's MAC address and best channel seen
- Strongest signal recorded
- Number of successful captures and failed attempts
- Last time the AP was encountered

The table uses LRU eviction when full. APs with 3+ failures in the last 90 seconds get put on a per-target cooldown and skipped entirely during selection. This prevents the ghost from thrashing on uncatchable targets.

Session aggregates (total sessions, handshakes, attempts, failures) are stored separately in `state.bin` and survive reboots.

## Your ghost

Each device generates a unique Ghostchi character from its WiFi MAC address. The name is deterministic -- same MAC, same name, forever. Names use a pronounceable consonant-vowel pattern producing things like "bemira", "kivexa", "lofenu", "tafuri", "zasin".

### Moods

The ghost's mood changes based on what it's been through:

| Mood | When |
|------|------|
| eager | Fresh start, no sessions yet |
| hopeful | Has tried, hasn't caught anything |
| proud | Has at least one handshake to its name |
| thriving | 6+ sessions or 3+ captures (permanent title) |
| drowsy | Left idle for 8+ hours |
| restless | Left idle for 24+ hours |
| hunting | Actively running a session |
| blocked | No SD card |

Each mood has its own sprite: happy for idle, angry when locked onto a target, evil during deauth bursts, love eyes when thriving, tired during cooldowns, confused when scanning or blocked.

### XP and levels

The ghost earns XP and levels up (1-10) based on activity:

```
XP = (sessions * 10) + (attempts * 3) + (captures * 24)
```

Captures are worth the most by far. Level thresholds ramp up from 18 XP at level 2 to 762 XP at max level.

### Speech bubbles

The ghost talks. Messages are context-aware and cycle every few seconds with multiple variations per state so it doesn't repeat. What it says depends on:

- **Hunting phase** -- "scanning.", "waiting on a packet.", "got a feel for it." during lock with high confidence, "making noise." during deauth, "logged it." after a capture, "wasn't the one." on a miss.
- **Idle time** -- Fresh idle gets "probably clear." and "up to you." After 8 hours it shifts to "long shift." and "you left me on." A thriving ghost left idle says "run me already."
- **Environment checks** -- Every ~54 seconds the ghost checks its surroundings and can interrupt with warnings about low battery, detected drones, GPS status, or SD card filling up.

## Pages

Use **Left/Right** to cycle through three screens:

### Page 1: Current State
- **MODE** -- Active, Standby, or Blocked
- **CH** -- Current channel
- **APS** -- Visible access points
- **TARGET** -- Current target SSID
- **PWND** -- Total handshakes captured
- **CONF** -- Confidence percentage

### Page 2: System Stats
- **HEAP** / **IRAM** -- Free memory
- **TRIES** / **MISSES** -- Attempt history
- **IDLE** -- Time since last session
- **SESS** -- Total sessions run

### Page 3: Character Passport
Your ghost's name, mood, level, XP bar, and animated portrait. The sprite bobs gently on a twinkling starfield background and changes expression to match its current state.

## Files on the SD card

Ghostchi keeps its own directory separate from manual captures:

| Path | Contents |
|------|----------|
| `/mnt/ghostesp/ghostchi/pcaps/` | PCAP files (`ghostchi_1.pcap`, `ghostchi_2.pcap`, ...) |
| `/mnt/ghostesp/ghostchi/sessions/` | Timestamped session logs (`ghostchi_20260330_142355.log`) |
| `/mnt/ghostesp/ghostchi/learn.bin` | Persistent AP learning table |
| `/mnt/ghostesp/ghostchi/state.bin` | Cross-session aggregates |

Each session appends to its own PCAP file and flushes it cleanly when stopped. Captures go to `.pcap` for Wireshark, not CSV.

## How it differs from `capture -eapol`

`capture -eapol` is manual -- you pick a target and capture. Ghostchi is autonomous -- it finds targets, scores them, adapts its strategy, remembers results, and operates its own capture loop. Captures land in `/mnt/ghostesp/ghostchi/pcaps/` instead of the shared `/mnt/ghostesp/pcaps/` so automated and manual captures stay separate.

## Quick start

1. Open **Menu -> WiFi -> Ghostchi**.
2. Make sure an SD card is mounted. The ghost will tell you if it's blocked.
3. Start a session. The ghost takes over from there.
4. Leave it running. It will sweep, lock, listen, and adapt.
5. Stop when you're done. Copy the `.pcap` and `.log` files off the SD card.
6. Open the PCAP in Wireshark to inspect EAPOL and association frames.

## Troubleshooting

- **Ghost says SD required** -- Mount the SD card and make sure `/mnt/ghostesp/` is available.
- **No PCAP created** -- Stop Ghostchi cleanly (don't just power off) so it can flush the capture file.
- **No useful packets inside** -- Ghostchi can only work with what's in the air. Move closer to the target or wait for a client to reconnect.
- **Ghost keeps missing the same AP** -- This is the learning system working. After 3 failures in 90 seconds, the ghost puts that AP on cooldown and moves on to better targets.
