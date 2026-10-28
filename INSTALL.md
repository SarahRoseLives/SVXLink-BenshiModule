# Installing svxlink-benshi with SvxLink

This guide installs the Benshi radio bridge and SvxLink on the same Linux
machine. The example layout is:

| Thing | Path |
| --- | --- |
| Bridge binary | `/usr/bin/svxlink-benshi` |
| Bridge config | `/etc/svxlink-benshi.conf` |
| Bridge service | `/usr/lib/systemd/system/svxlink-benshi.service` |
| SvxLink binary | `/usr/bin/svxlink` |
| SvxLink config | `/etc/svxlink/svxlink.conf` |
| SvxLink sounds | `/usr/share/svxlink/sounds/en_US` |

If you use `/usr/local` instead, adjust the paths accordingly.

## 1. Create the SvxLink user

Both services can run as an unprivileged `svxlink` user:

```sh
sudo groupadd --system svxlink 2>/dev/null || true
sudo useradd --system --no-create-home --shell /usr/bin/nologin \
  --gid svxlink svxlink 2>/dev/null || true
```

RFCOMM access normally does not require any extra group. If your distribution
restricts Bluetooth sockets to a `bluetooth` group, add
`SupplementaryGroups=bluetooth` to the bridge unit.

## 2. Install bridge build dependencies

| Distribution | Packages |
| --- | --- |
| Arch | `sudo pacman -S --needed cmake gcc pkgconf bluez-libs openssl` |
| Debian/Ubuntu | `sudo apt install cmake g++ pkg-config libbluetooth-dev libssl-dev` |
| Fedora | `sudo dnf install cmake gcc-c++ pkgconf bluez-libs-devel openssl-devel` |

## 3. Build and install the bridge

From this repository:

```sh
cmake -S . -B build \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_INSTALL_SYSCONFDIR=/etc
cmake --build build -j
ctest --test-dir build --output-on-failure
sudo cmake --install build
```

This installs the binary, `/etc/svxlink-benshi.conf`, the systemd unit, and
documentation. The bundled SBC codec is built from `third_party/sbc`; no system
SBC package is needed.

## 4. Pair the radio

The radio exposes two Bluetooth Classic RFCOMM services:

- Control / SPP: `00001101-0000-1000-8000-00805F9B34FB`
- Audio / vendor "BS AOC": `39144315-32FA-40DB-85ED-FBFEBA2D86E6`
  (falls back to generic audio `00001203-0000-1000-8000-00805F9B34FB`)

Find the radio:

```sh
svxlink-benshi --list
```

Put the radio in Bluetooth pairing mode, then:

```sh
sudo btmgmt bondable on
sudo btmgmt connectable on

bluetoothctl
power on
agent on
default-agent
scan on
pair AA:BB:CC:DD:EE:FF
trust AA:BB:CC:DD:EE:FF
connect AA:BB:CC:DD:EE:FF
scan off
quit
```

Replace `AA:BB:CC:DD:EE:FF` with the radio's address.

Verify:

```sh
bluetoothctl info AA:BB:CC:DD:EE:FF
```

You want `Paired: yes`, `Bonded: yes`, `Trusted: yes`, and `Connected: yes`.

Test the radio connection before starting the services:

```sh
svxlink-benshi --config /etc/svxlink-benshi.conf --probe
```

You should see the product ID, hardware/software versions, and radio status.

### If re-pairing fails

`br-connection-key-missing` or `org.bluez.Error.AuthenticationFailed` normally
means the radio still remembers an old bond for this computer.

1. On the radio, open its Bluetooth/paired-device menu and delete the entry for
   this computer. If the radio has a Bluetooth reset option, use it.
2. Make sure no phone is connected to the radio.
3. Put the radio back into pairing mode.
4. On Linux:

```sh
sudo btmgmt bondable on
sudo btmgmt connectable on
bluetoothctl remove AA:BB:CC:DD:EE:FF
bluetoothctl --timeout 10 scan on
bluetoothctl pair AA:BB:CC:DD:EE:FF
bluetoothctl trust AA:BB:CC:DD:EE:FF
bluetoothctl connect AA:BB:CC:DD:EE:FF
```

## 5. Configure the bridge

Edit `/etc/svxlink-benshi.conf`:

```ini
[RADIO]
MAC=AA:BB:CC:DD:EE:FF
# DUAL_WATCH=0
# APRS=0
TX_LEAD_MS=100
SBC_BITPOOL=18

[SVXLINK]
BIND=127.0.0.1
PORT=5210
AUTH_KEY=
SAMPLE_RATE=16000
```

Notes:

- `DUAL_WATCH` and `APRS` are optional. Leave them commented to keep the
  radio's current settings.
- `CHANNEL` is 0-based; the radio display is normally one higher. Leave it
  unset to keep the current channel.
- `FREQUENCY` optionally tunes the VFO in MHz, for example `145.100`.
- `SAMPLE_RATE` must match SvxLink's compile-time `INTERNAL_SAMPLE_RATE`
  (see step 6).
- NetTrx is unencrypted. If `BIND` is not loopback, set a long random
  `AUTH_KEY` and use the same key in both SvxLink transceiver sections.

The bridge is now installable as a service:

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now svxlink-benshi
journalctl -u svxlink-benshi -f
```

You should see `Radio link established` and `SVXLink NetTrx server listening`.

## 6. Install SvxLink

### Option A: distribution package

Install SvxLink with your package manager if a suitable version is available.
You need NetTrx protocol 2.8 (SvxLink 1.10.x/current master). Check:

```sh
svxlink --version
```

If your package was built with the old `INTERNAL_SAMPLE_RATE=8000`, set
`SAMPLE_RATE=8000` in `/etc/svxlink-benshi.conf`. Current SvxLink defaults to
16000.

### Option B: build from source

Install the SvxLink build dependencies. For Arch:

```sh
sudo pacman -S --needed cmake gcc pkgconf git groff \
  libsigc++ libgcrypt jsoncpp alsa-lib gsm popt tcl \
  libpulse speexdsp opus libsndfile
```

For Debian/Ubuntu:

```sh
sudo apt install cmake g++ pkg-config git groff \
  libsigc++-2.0-dev libgcrypt20-dev libjsoncpp-dev libasound2-dev \
  libgsm1-dev libpopt-dev tcl-dev libpulse-dev libspeexdsp-dev \
  libopus-dev libsndfile1-dev
```

Build and install:

```sh
git clone --depth 1 https://github.com/sm0svx/svxlink.git
cmake -S svxlink/src -B svxlink/build \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DSYSCONF_INSTALL_DIR=/etc \
  -DLOCAL_STATE_DIR=/var \
  -DUSE_QT=NO \
  -DINTERNAL_SAMPLE_RATE=16000
cmake --build svxlink/build -j
sudo cmake --install svxlink/build
```

The `INTERNAL_SAMPLE_RATE` value must match `SAMPLE_RATE` in the bridge
configuration.

### Install the English sound pack

SvxLink's source tree does not include the voice prompt files. Without them,
module announcements fail and module activation can appear broken.

```sh
curl -L -o /tmp/svxlink-sounds.tar.bz2 \
  https://github.com/sm0svx/svxlink-sounds-en_US-heather/releases/download/25.05/svxlink-sounds-en_US-heather-16k-25.05.tar.bz2
rm -rf /tmp/en_US-heather-16k
tar -xjf /tmp/svxlink-sounds.tar.bz2 -C /tmp
sudo mkdir -p /usr/share/svxlink/sounds/en_US
sudo cp -a /tmp/en_US-heather-16k/. /usr/share/svxlink/sounds/en_US/
sudo chown -R root:root /usr/share/svxlink/sounds/en_US
```

Check the release page for newer versions:
<https://github.com/sm0svx/svxlink-sounds-en_US-heather/releases>.

### Configure SvxLink

Start from `config/svxlink.conf.example` or merge the relevant sections into
`/etc/svxlink/svxlink.conf`:

```ini
[GLOBAL]
LOGIC_CORE_PATH=/usr/lib/svxlink
LOGICS=SimplexLogic
CFG_DIR=svxlink.d
TIMESTAMP_FORMAT="%c"

[SimplexLogic]
TYPE=Simplex
RX=RxBenshi
TX=TxBenshi
MODULES=ModuleHelp,ModuleParrot
CALLSIGN=MYCALL
SHORT_IDENT_INTERVAL=60
LONG_IDENT_INTERVAL=60
DEFAULT_LANG=en_US
RGR_SOUND_DELAY=0
FX_GAIN_NORMAL=0
FX_GAIN_LOW=-12

[RxBenshi]
TYPE=Net
HOST=127.0.0.1
TCP_PORT=5210
CODEC=S16
AUTH_KEY=

[TxBenshi]
TYPE=Net
HOST=127.0.0.1
TCP_PORT=5210
CODEC=S16
AUTH_KEY=
```

Set `CALLSIGN` to your call sign. Both `Rx` and `Tx` must use the same
`HOST`/`TCP_PORT` so SvxLink shares one NetTrx connection. `CODEC=S16` is the
efficient choice; `RAW` also works.

### Install a SvxLink service

If your SvxLink package already provides a systemd unit, use it. Otherwise
copy the included example:

```sh
sudo cp systemd/svxlink.service.example /etc/systemd/system/svxlink.service
sudo systemctl daemon-reload
```

Adjust `ExecStart` paths in the unit if your SvxLink is not installed under
`/usr`.

## 7. Start and test

```sh
sudo systemctl enable --now svxlink-benshi svxlink
systemctl status svxlink-benshi svxlink
```

Check the logs:

```sh
journalctl -u svxlink-benshi -f
journalctl -u svxlink -f
```

You should see:

```text
RemoteTrx protocol version 2.8
Connected to remote receiver at 127.0.0.1:5210
Connected to remote transmitter at 127.0.0.1:5210
```

Key the radio and try:

| DTMF | Action |
| --- | --- |
| `0#` | Help |
| `1#` | Parrot |
| `*` | Manual identification |

If Help is active, `1#` is handled by Help (it shows module 1 help). Send just
`#` to leave Help, or wait for the module timeout, then send `1#`.

## Sample rate

SvxLink's NetTrx audio rate is its compile-time `INTERNAL_SAMPLE_RATE`. The
bridge must use the same rate:

| SvxLink build | Bridge config |
| --- | --- |
| current master/default | `SAMPLE_RATE=16000` |
| older/some distro packages | `SAMPLE_RATE=8000` |

A mismatch changes the speed/pitch of the audio. If the audio is too fast or
too slow, try the other value.

## Upgrade

```sh
git pull
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_SYSCONFDIR=/etc
cmake --build build -j
sudo cmake --install build
sudo systemctl restart svxlink-benshi
```

`cmake --install` reinstalls the example `/etc/svxlink-benshi.conf`. Keep a
backup or re-apply your MAC and settings after upgrading.

## Uninstall

```sh
sudo systemctl disable --now svxlink-benshi
sudo rm -f /usr/bin/svxlink-benshi
sudo rm -f /etc/svxlink-benshi.conf
sudo rm -f /usr/lib/systemd/system/svxlink-benshi.service
sudo systemctl daemon-reload
```

This does not remove SvxLink or the Bluetooth pairing.

## Troubleshooting checklist

1. `bluetoothctl info <MAC>` shows `Paired`, `Bonded`, `Trusted`, `Connected`.
2. `svxlink-benshi --probe` succeeds and prints radio status.
3. `journalctl -u svxlink-benshi` shows `Radio link established`.
4. `journalctl -u svxlink` shows `RemoteTrx protocol version 2.8` and both
   `Connected to remote receiver/transmitter`.
5. DTMF `0#` produces the Help prompt.
6. If module audio is missing, install the sound pack.
7. If TX audio stutters, check system load, keep `TX_LEAD_MS=100`, and make
   sure only one program is controlling the radio.
