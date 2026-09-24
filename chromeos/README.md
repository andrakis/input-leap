# Input Leap client for the ChromeOS host (developer mode)

`ilc-uinput.c` is a small, dependency-free Input Leap/Barrier **client** that runs on
the ChromeOS host OS itself (not inside the Crostini VM), so the Windows keyboard and
mouse control the *whole* Chromebook UI — Chrome, the shelf, Android apps, everything.

It works by speaking the Input Leap wire protocol (v1.6) over plain TCP and injecting
the events through `/dev/uinput` as three virtual devices (keyboard, mouse buttons +
wheel, and a pen tablet for exact cursor placement). Root is required for `/dev/uinput`,
which is why this needs developer mode.

Protocol handling was verified against the Windows `input-leaps.exe` in this repo and a
scripted fake server (`fake-server.py`). The uinput side has **not** been tested on a
real Chromebook yet — see "If something is off" below.

## 1. Build it (in the Chromebook's Linux/Crostini terminal)

Building in Crostini guarantees the binary matches the Chromebook's CPU (x86-64 or ARM).

```sh
sudo apt install -y gcc libc6-dev            # once
gcc -O2 -Wall -static -o ilc-uinput ilc-uinput.c
cp ilc-uinput ~/                             # ~/ is visible in the Files app as "Linux files"
```

Get `ilc-uinput.c` onto the Chromebook however is convenient (Files app, `curl` from a
share, git clone of this repo, etc.).

## 2. Install it on the host

In the ChromeOS Files app, drag `ilc-uinput` from **Linux files** to **Downloads**. Then
open crosh (`Ctrl+Alt+T`) and:

```
shell
sudo cp /home/chronos/user/MyFiles/Downloads/ilc-uinput /usr/local/bin/
sudo chmod +x /usr/local/bin/ilc-uinput
```

`/usr/local` is writable in developer mode without touching rootfs verification.

## 3. Configure the Windows server (once)

In the Input Leap GUI on the PC that owns the keyboard/mouse:

1. **Edit → Settings → uncheck "Use SSL encryption"** — the client speaks plain TCP.
   (LAN only; don't expose port 24800 to the internet.)
2. **Configure Server…** — drag a new screen next to your PC and name it, e.g. `chromebook`.
3. Start the server. Allow it through Windows Firewall if prompted (TCP 24800).

## 4. Run the client (ChromeOS host shell, as root)

```
sudo ilc-uinput -s 192.168.1.10 -n chromebook -v
```

- `-s host[:port]` server address (default port 24800)
- `-n name` screen name, must match the server's layout exactly (default: hostname)
- `-w WxH` screen size; auto-detected from `/sys/class/drm` (first connected output)
- `--rel` use a relative mouse for cursor motion instead of the pen tablet
- `-v` verbose, `--dry-run` no uinput (protocol test only)

You should see `handshake done` and, on the server, `client "chromebook" has connected`.
Move the mouse off the edge of the PC screen and it should appear on the Chromebook.
Ctrl+C stops it; it reconnects automatically if the server restarts.

To keep it running after closing crosh: `sudo sh -c 'nohup ilc-uinput -s IP -n chromebook >/var/log/ilc.log 2>&1 &'`.

## If something is off

- **Cursor doesn't move but clicks/keys work** → try `--rel`. Chrome then treats it as a
  plain mouse; ChromeOS pointer acceleration can make the position drift, so turn off
  *Settings → Mouse → Mouse acceleration* on the Chromebook.
- **Cursor moves but the screen size looks wrong / edges don't line up** → pass `-w` with
  the size you want. Any size works; it just sets how far the mouse travels.
- **`open /dev/uinput: No such file`** → `sudo modprobe uinput`.
- **Wrong characters** → the key mapping assumes a US layout on the Chromebook (letters,
  digits and punctuation are mapped by the character the server sends, so a non-US
  *server* layout is fine).
- **`unexpected hello length`** → SSL is still enabled on the server.
- **`unknown screen`** → the `-n` name doesn't match a screen in the server's layout.
- Clipboard sharing is not implemented.
