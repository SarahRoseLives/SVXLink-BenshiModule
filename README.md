# svxlink-benshi

Use a Benshi-compatible Bluetooth radio (Vero VR-N76, VR-N7600, and similar)
as the transceiver for [SvxLink](https://github.com/sm0svx/svxlink).

This is a Linux port of the radio layer from
[BenshiNode](https://github.com/SarahRoseLives/BenshiNode). It connects to the
radio's Bluetooth Classic control and audio services, speaks the Gaia control
protocol, exchanges SBC audio, and exposes the radio to SvxLink through
SvxLink's native **NetTrx / RemoteTrx** protocol.

The result is a normal SvxLink simplex node or repeater with the Benshi radio
as its RF front end. SvxLink still provides DTMF command handling, parrot,
identification, EchoLink, Reflector, AllStar, and the other modules you
configure.

```
Benshi radio  <-- Bluetooth RFCOMM (Gaia + SBC) -->  svxlink-benshi
                                                             |
                                            TCP NetTrx (2.8) |
                                                             v
                                                       svxlink
```

See **[INSTALL.md](INSTALL.md)** for the complete step-by-step installation.

## Why NetTrx instead of a `.so` plugin?

SvxLink modules loaded with `MODULES=` are voice-service plugins. They sit in
the audio chain of an already existing transceiver; they cannot *be* the
transceiver and cannot inject receiver audio into the logic core. A radio
backend has to implement SvxLink's `Rx`/`Tx` side.

SvxLink already has a supported mechanism for that: its RemoteTrx protocol,
used by `remotetrx` and the built-in `NetRx`/`NetTx` transceiver types. This
project implements the server side of that protocol, so SvxLink sees a normal
network transceiver. No SvxLink source changes, custom `.so` plugin, ALSA
loopback (`snd-aloop`), or PTY/GPIO PTT wiring is required.

## Features

- Bluetooth Classic RFCOMM control and audio channels (BlueZ)
- SDP discovery of the SPP control service and the vendor "BS AOC" audio
  service, with a generic-audio fallback
- Gaia device status, notifications, channel/VFO configuration, APRS setting
- SBC encode/decode compatible with the radio (32 kHz, mono, 16 blocks,
  8 subbands)
- 32 kHz radio audio <-> SvxLink audio resampling (8000, 16000, or 32000 Hz)
- Radio squelch, VOX squelch, or automatic combination
- RX and TX gain controls
- DTMF decoding from radio audio, with tone muting, forwarded to SvxLink
- SVXLink NetTrx protocol version 2.8 with optional HMAC-SHA1 authentication
- `RAW` (float32) and `S16` audio codecs
- Automatic Bluetooth reconnect
- Diagnostic commands: `--probe`, `--dump`, `--unkey`, `--test-tx`, `--list`

## Requirements

- Linux with BlueZ
- A paired and trusted Benshi-compatible radio
- SvxLink with NetTrx protocol 2.8 (SvxLink 1.10.x / current master)
- Build dependencies: CMake, a C++17 compiler, `pkg-config`, BlueZ development
  headers, OpenSSL development headers

| Distribution | Bridge build packages |
| --- | --- |
| Debian/Ubuntu | `cmake g++ pkg-config libbluetooth-dev libssl-dev` |
| Fedora | `cmake gcc-c++ pkgconf bluez-libs-devel openssl-devel` |
| Arch | `cmake gcc pkgconf bluez-libs openssl` |

## Quick start

```sh
# 1. Build and install the bridge
cmake -S . -B build \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_INSTALL_SYSCONFDIR=/etc
cmake --build build -j
ctest --test-dir build --output-on-failure
sudo cmake --install build

# 2. Pair and trust the radio (see INSTALL.md for bond troubleshooting)
svxlink-benshi --list
bluetoothctl
#   scan on
#   pair AA:BB:CC:DD:EE:FF
#   trust AA:BB:CC:DD:EE:FF

# 3. Edit the bridge config
sudo editor /etc/svxlink-benshi.conf
#   MAC=AA:BB:CC:DD:EE:FF
#   SAMPLE_RATE=16000

# 4. Install/configure SvxLink, then run the bridge and SvxLink
svxlink-benshi --config /etc/svxlink-benshi.conf
# in another terminal:
svxlink --config=/etc/svxlink/svxlink.conf
```

Full details, including how to build SvxLink and install the sound pack, are in
**[INSTALL.md](INSTALL.md)**.

## Configuration

The example bridge configuration is `config/svxlink-benshi.conf`. The key
settings are:

```ini
[RADIO]
MAC=AA:BB:CC:DD:EE:FF
DUAL_WATCH=0
APRS=0
TX_LEAD_MS=100

[SVXLINK]
BIND=127.0.0.1
PORT=5210
AUTH_KEY=
SAMPLE_RATE=16000
```

The matching SvxLink sections are in `config/svxlink.conf.example`:

```ini
[SimplexLogic]
TYPE=Simplex
RX=RxBenshi
TX=TxBenshi
MODULES=ModuleHelp,ModuleParrot
CALLSIGN=MYCALL

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

`SAMPLE_RATE` must match the `INTERNAL_SAMPLE_RATE` that SvxLink was compiled
with. Current SvxLink defaults to `16000`; older builds and some distribution
packages use `8000`. `32000` also works.

If `BIND` is not loopback, set a non-empty `AUTH_KEY` in `[SVXLINK]` and in both
SvxLink transceiver sections. NetTrx traffic is not encrypted; use a VPN or SSH
tunnel for remote links.

## Testing the node

Once the bridge and SvxLink are running, key the radio and use DTMF:

| DTMF | Action |
| --- | --- |
| `0#` | Help |
| `1#` | Parrot |
| `*` | Manual identification |

If Help is active, `1#` is interpreted by Help as "show module 1". Send just
`#` to leave Help first, or wait for the module timeout.

## Diagnostics

```sh
# List nearby Bluetooth devices
svxlink-benshi --list

# Connect, print radio status/device info, disconnect
svxlink-benshi --probe

# Print memory channels and radio settings
svxlink-benshi --dump

# Force the radio out of transmit
svxlink-benshi --unkey

# Transmit a 1 kHz test tone for two seconds (RF on air!)
svxlink-benshi --test-tx
```

Both programs log to the terminal. To keep logs while running in the
background:

```sh
svxlink-benshi --config /etc/svxlink-benshi.conf 2>&1 | tee benshi.log
svxlink --config=/etc/svxlink/svxlink.conf 2>&1 | tee svxlink.log
```

## How audio is handled

- RX: SBC frames from the radio are decoded to 32 kHz mono PCM. Gain and
  squelch are applied, then audio is downsampled to SvxLink's internal rate and
  sent as `S16` or `RAW` NetTrx audio.
- TX: audio from SvxLink is upsampled to 32 kHz, encoded to SBC, and written to
  the radio in batches. Sending audio keys the radio; the end frame unkeys it.
- A short lead silence (`TX_LEAD_MS`) lets the radio's playout buffer settle.
  While keyed, silence is sent between network chunks so brief scheduling
  hiccups do not sound like dropouts.
- DTMF is decoded from the radio's 32 kHz audio and forwarded to SvxLink as
  NetTrx `MsgDtmf` messages. The tones are muted from the forwarded audio.

## Troubleshooting

**`br-connection-key-missing` or `AuthenticationFailed`**  
The Bluetooth bond is stale. Delete the computer/host pairing on the radio,
then re-pair from Linux:

```sh
sudo btmgmt bondable on
bluetoothctl remove AA:BB:CC:DD:EE:FF
bluetoothctl --timeout 10 scan on
bluetoothctl pair AA:BB:CC:DD:EE:FF
bluetoothctl trust AA:BB:CC:DD:EE:FF
bluetoothctl connect AA:BB:CC:DD:EE:FF
```

**No matching RFCOMM service found**  
Pair and trust the radio, power it on, and confirm with
`bluetoothctl info <MAC>`. Some firmware needs a moment after pairing before
SDP is available.

**Radio connects but there is no RX audio**  
Run `--probe` and check squelch/RSSI. Try `SQUELCH_MODE=vox` and adjust
`VOX_THRESHOLD_DB`. Check the radio volume and channel.

**Audio sounds choppy**  
Keep the default `TX_LEAD_MS=100`. Check system load and Bluetooth link
quality. Make sure only one program is controlling the radio. Do not run
SvxLink's `remotetrx` on the same port (`5210`).

**SvxLink says `Could not find audio clip ...`**  
The English sound pack is not installed. See INSTALL.md.

**SvxLink heartbeat timeout**  
Both `Rx` and `Tx` must use the same `HOST`/`TCP_PORT` so SvxLink shares one
NetTrx connection. Keep the bridge running.

## Limitations

- Linux only. The radio transport uses BlueZ RFCOMM; BenshiNode remains the
  Windows implementation.
- One NetTrx client at a time, matching SvxLink's `remotetrx` server behavior.
- Radio audio is resampled with simple filters optimized for voice.
- SVXLink tone detectors requested with `MsgAddToneDetector` are ignored. DTMF
  works, but 1750 Hz tone-burst repeater opening and CTCSS/PL detection are not
  implemented; use DTMF or squelch-based opening instead.
- Configure CTCSS on the radio or with `[RADIO] RX_CTCSS`/`TX_CTCSS` in the
  bridge, not with SvxLink's `TX_CTCSS`/CTCSS settings.
- SVXLink's `MsgSendDtmf` request (used by the DTMF repeater module) is
  ignored.
- The bridge does not bridge BenshiNode's proprietary link/reflector protocol.
  It makes the radio a transceiver for SvxLink instead.

## Credits and licenses

- Radio protocol, SBC framing, and the original Windows implementation:
  [BenshiNode](https://github.com/SarahRoseLives/BenshiNode).
- NetTrx protocol and SvxLink: Copyright Tobias Blomberg / SM0SVX,
  [SvxLink](https://github.com/sm0svx/svxlink), GPL-2.0-or-later.
- SBC codec: Google libsbc, Apache-2.0, see `third_party/sbc/LICENSE`.

This project is licensed under GPL-2.0-or-later; see [LICENSE](LICENSE).
