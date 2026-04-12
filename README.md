# kidle

KDE Plasma Wayland idle screen lock & DPMS daemon.

## The Problem

On KDE Plasma 6 running on Wayland, screens don't turn off before the first lock. The built-in DPMS (Display Power Management Signaling) never triggers from idle. This is particularly damaging for OLED displays, which suffer burn-in when left on with static content.

The bug manifests in two scenarios:

1. **Before first lock** — Plasma's idle timeout never turns off the display. The screen stays on indefinitely until you manually lock.
2. **At the login/greeter screen** — plasmalogin shows the greeter, but no idle timeout turns off the display.

This is a known Plasma Wayland bug. KWin's `org.freedesktop.ScreenSaver.GetSessionIdleTime` returns 0 on Wayland (unsupported), and logind's `IdleHint` is never set by Plasma. The standard DPMS pipeline is broken.

## How kidle Works

kidle works around the bug by implementing its own idle detection and screen control:

1. **Input device monitoring** — Reads `/dev/input/event*` devices directly to track keyboard and mouse activity. Tracks last activity time and triggers the lock + DPMS off after the configured timeout (default: 300s / 5 minutes).

2. **KWin ScreenSaver integration** — When running under an active user session, connects to `org.kde.KWin /ScreenSaver` on the session bus to detect when the screen locker activates. Immediately forces `kscreen-doctor --dpms off` to work around the broken DPMS pipeline. Also detects unlock to restore the display.

3. **Session-aware operation** — Runs as a system service (root). Automatically discovers the active display session (greeter or user) by scanning `/run/user/` for session buses. Sets `DBUS_SESSION_BUS_ADDRESS`, `WAYLAND_DISPLAY`, `XDG_RUNTIME_DIR`, and `QT_QPA_PLATFORM=wayland` environment variables so `kscreen-doctor` can reach KWin regardless of whether the greeter or user session is active.

4. **logind monitoring** — Watches for session changes via `org.freedesktop.login1.Manager` properties. When a user logs in or the session switches, kidle reconnects to the appropriate bus.

## Installation

### Arch Linux (AUR)

```
paru -S kidle
```

### From source

```
make
makepkg -cf
sudo pacman -U kidle-*.pkg.tar.zst
```

## Usage

```
sudo systemctl enable --now kidle
```

The default idle timeout is 300 seconds (5 minutes). To customize:

Edit `/usr/lib/systemd/system/kidle.service` and modify the `ExecStart` line:

```
ExecStart=/usr/bin/kidle --timeout 600
```

Then reload and restart:

```
sudo systemctl daemon-reload
sudo systemctl restart kidle
```

## Command Line Options

| Option | Default | Description |
|--------|---------|-------------|
| `-t, --timeout=SECS` | 300 | Idle timeout in seconds (minimum: 10) |
| `-d, --debug` | off | Enable debug output |

## How It Differs from Plasma's Built-in DPMS

| | Plasma DPMS | kidle |
|---|---|---|
| Works before first lock | No (broken on Wayland) | Yes |
| Works at login screen | No | Yes |
| Idle detection method | KWin (broken on Wayland) | `/dev/input` + logind |
| Screen off mechanism | KWin DPMS (broken) | `kscreen-doctor --dpms off` |
| Runs as | User session | System service |

## Requirements

- KDE Plasma 6 (Wayland)
- `kscreen` (provides `kscreen-doctor`)
- `kwin`
- `glib2`

## License

MIT — Copyright (c) 2026 Danilo Falcão