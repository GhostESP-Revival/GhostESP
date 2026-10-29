---
title: "Cert Scan"
description: "Find out which devices still trust a revoked certificate authority, and which hosts still serve certificates from one"
weight: 57
aliases:
  - "/wifi/trust-scan/"
  - "/wifi/sinkhole-https/"
---

Cert Scan answers two questions:

- **Which devices on my network still accept a certificate from a CA that was
  revoked?** Those devices have an out-of-date trust store and need an update.
- **Which hosts are still serving a certificate from one?** Those services are
  broken for everyone who has updated.

Use it on networks you own or have written permission to test. It does not
decrypt anything and does not relay real traffic — a device under test gets the
board's page, not the real site.

## Before you start

Three files in `/mnt/ghostesp/dns_sinkhole/https/`. The easy way to get all
three is one command:

```
certscan key 4
```

```
CA key ready: EM OV TLS CA - G3C (P-384), wrote ca.crt and ca.key
Leaf key: wrote /mnt/ghostesp/dns_sinkhole/https/server.key (EC P-256)
Ready. Start with 'certscan devices <domain>'
```

Type the CA's private key number and the board finds the
matching certificate, writes both files, and generates the leaf key. It checks
the number before accepting it, so a typo is refused rather than leaving you
with a setup that never works.

`certscan setup` shows what is present and fixes what it can. The individual
pieces are there too if you need them:

| Command | Does |
|---|---|
| `certscan key <number>` | Builds `ca.key` from the scalar, and `ca.crt` too when the certificate is one the board knows |
| `certscan cacert <url>` | Downloads a CA certificate to `ca.crt` |
| `certscan genleaf` | Generates `server.key` on the device |
| `certscan setup` | Shows what is missing and generates the leaf key if needed |

## Testing a device

```
connect
certscan devices
```

```
No hostname given: answering every name with this board
Whatever the device resolves is tested. It will have no internet
until the scan stops, which is expected.
Cert Scan running
Set your device DNS to 192.168.1.50
```

Point the device at the board as its DNS server and open an ordinary website.

### iPhone / iPad

1. Join the same Wi-Fi as the board.
2. **Settings → Wi-Fi → ⓘ** next to the network **→ Configure DNS → Manual**.
3. Remove the existing servers and add the board's IP.
4. Turn off anything that resolves names elsewhere:
   - **Settings → Apple ID → iCloud → Private Relay** → off
   - **Wi-Fi → ⓘ → Limit IP Address Tracking** → off
5. Open Safari and go to `https://example.com`.

### Android

1. Join the same Wi-Fi as the board.
2. **Settings → Network & Internet → Wi-Fi**, long-press the network, **Modify
   network → Advanced options**.
3. Set **IP settings: Static**, then set **DNS 1** to the board's IP.
4. Set **Private DNS** to **Off**, not Automatic.
5. Open Chrome and go to `https://example.com`.

### The result

| What the device does | What it means |
|---|---|
| Page loads, no warning | It accepted the certificate — its trust store is out of date |
| Warning, or the connection fails | It rejected the certificate — patched |

Check on the board:

```
certscan list
```

```
  Status        Running
  CA            EM OV TLS CA - G3C
  Device DNS    192.168.1.50
  DNS           all names
  Certificates  7
  Accepted      1
  Devices
    192.168.1.77 gsp-ssl.ls.apple.com
```

Everything under **Accepted** is a device that still trusts the revoked CA.
Put the device's DNS back to **Automatic** when you are done.

**Test with an ordinary website.** Do not judge the result from background
traffic. Phones and laptops constantly contact their vendor's own services
(iCloud, Google, Microsoft), and those are **pinned** — they reject any
certificate that is not their own, on a fully unpatched device just as readily
as on a patched one.

## Checking hosts

```
certscan hosts
```

```
Checking 21 host(s) on port 443...
  192.168.4.1: no HTTPS
  192.168.6.146: ok
Checked 21 host(s), 0 answered on :443, 0 affected
No host is serving HTTPS on that port, so there is nothing to compare.
```

It finds the hosts on your network first, then connects to each one and checks
the certificate it presents. On a home network most things do not run a web
server on 443, so "no HTTPS" and a zero total are normal.

Add a port to check somewhere else: `certscan hosts 8443`.

## From the display

**Wi-Fi → Cert Scan**

- **Scan Devices** — shows a spinner while it starts, then a live view of the
  run: the CA in use, the **DNS address to set on the device**, certificates
  presented, devices accepted, and one row per accepting device. The view
  refreshes itself while it is on screen.
- **Check Hosts** — discovers hosts and checks their certificates
- **Scan Results** — re-opens the view without restarting anything
- **Add Hostname** — limits the test to one name instead of everything
- **Setup** — CA certificate, CA key number, leaf key, and what is present
- **Clear Results** — resets the counters
- **Stop Cert Scan** — stops everything and restores AP services

While a spinner is showing, **any button cancels it**.

## If nothing happens

- **`Certificates` stays at 0.** The device never reached the board. Its DNS
  still points at the router, or Private Relay / Private DNS is resolving names
  elsewhere. Toggle Wi-Fi off and on to clear the DNS cache.
- **A stale answer.** A name resolved before you changed the DNS is still
  cached. Use a different hostname, or toggle Wi-Fi.
- **You typed the board's IP instead of a hostname.** `https://192.168.1.50/`
  will not work: the board issues a certificate for the name the client asks
  for, and an address is not a name.
- **`Certificates` is higher than the number of devices.** One page load opens
  several connections. `Accepted` is counted once per device.
- **The scan will not start.** `certscan setup` shows which files are missing
  and what to run to fix it.

## Limitations

- **Only devices using the board as their DNS server** can be redirected.
- **Updated browsers will refuse the certificate.** That is the correct result,
  not a fault.
- **A device that accepts today may refuse after an update.** This measures the
  current state of a trust store, not a permanent property.
- **Vendor services are pinned** and will always reject. Judge the result from
  an ordinary website, not from background traffic.
- **The board needs a correct clock.** Certificates are issued with a validity
  window around the current time; the clock is set automatically once the radio
  connects.
