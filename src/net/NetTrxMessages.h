// SPDX-License-Identifier: GPL-2.0-or-later
//
// Wire-compatible subset of SvxLink's RemoteTrx protocol (NetTrxMsg.h).
// SvxLink is Copyright (C) Tobias Blomberg / SM0SVX and is licensed under the
// GNU General Public License version 2 or later.
#pragma once

#include <cstdint>
#include <cstddef>

namespace benshi::nettrx {

enum class MessageType : uint32_t {
  ProtoVer = 0,
  Heartbeat = 1,
  AuthChallenge = 10,
  AuthResponse = 11,
  AuthOk = 12,
  RxAudioCodecSelect = 100,
  TxAudioCodecSelect = 101,
  Audio = 102,
  SetMuteState = 200,
  AddToneDetector = 201,
  Reset = 202,
  SetRxFq = 203,
  SetRxModulation = 204,
  Squelch = 250,
  Dtmf = 251,
  Tone = 252,
  Sel5 = 253,
  SiglevUpdate = 254,
  SetTxCtrlMode = 300,
  EnableCtcss = 301,
  SendDtmf = 302,
  Flush = 303,
  TransmittedSignalStrength = 304,
  SetTxFq = 305,
  SetTxModulation = 306,
  TxTimeout = 350,
  TransmitterStateChange = 351,
  AllSamplesFlushed = 352
};

// Mirrors Rx::MuteState and Tx::TxCtrlMode.
enum MuteState : int32_t { MuteNone = 0, MuteContent = 1, MuteAll = 2 };
enum TxCtrlMode : int32_t { TxOff = 0, TxOn = 1, TxAuto = 2 };

constexpr uint16_t kProtocolMajor = 2;
constexpr uint16_t kProtocolMinor = 8;
constexpr size_t kAuthChallengeLen = 20;
constexpr size_t kAuthDigestLen = 20;
constexpr size_t kMaxAudioBytes = sizeof(float) * 512;

#pragma pack(push, 1)

struct Header {
  uint32_t type = 0;
  uint32_t size = 0;
};

struct ProtoVer {
  Header header;
  uint16_t major = kProtocolMajor;
  uint16_t minor = kProtocolMinor;
};

struct Heartbeat {
  Header header;
};

struct AuthChallenge {
  Header header;
  uint8_t challenge[kAuthChallengeLen];
};

struct AuthResponse {
  Header header;
  uint8_t digest[kAuthDigestLen];
};

struct AuthOk {
  Header header;
};

struct AudioCodecSelect {
  Header header;
  char codec_name[32];
  uint8_t option_count;
  char options[256];
};

struct Audio {
  Header header;
  int32_t payload_size;
  uint8_t data[kMaxAudioBytes];
};

struct SetMuteState {
  Header header;
  int32_t mute_state;
};

struct AddToneDetector {
  Header header;
  float frequency;
  int32_t bandwidth;
  float threshold;
  int32_t required_duration;
};

struct Reset {
  Header header;
};

struct SetRxFq {
  Header header;
  uint32_t frequency;
};

struct SetRxModulation {
  Header header;
  int32_t modulation;
};

struct Squelch {
  Header header;
  bool is_open;
  float signal_strength;
  char sql_rx_id;
  char activity_info[128];
};

struct Dtmf {
  Header header;
  char digit;
  int32_t duration;
};

struct SiglevUpdate {
  Header header;
  float signal_strength;
  char sql_rx_id;
};

struct SetTxCtrlMode {
  Header header;
  int32_t mode;
};

struct EnableCtcss {
  Header header;
  bool enable;
};

struct Flush {
  Header header;
};

struct TransmittedSignalStrength {
  Header header;
  float signal_strength;
  char sql_rx_id;
};

struct SetTxFq {
  Header header;
  uint32_t frequency;
};

struct SetTxModulation {
  Header header;
  int32_t modulation;
};

struct TxTimeout {
  Header header;
};

struct TransmitterStateChange {
  Header header;
  bool is_transmitting;
};

struct AllSamplesFlushed {
  Header header;
};

#pragma pack(pop)

static_assert(sizeof(Header) == 8, "NetTrx header layout");
static_assert(sizeof(ProtoVer) == 12, "NetTrx ProtoVer layout");
static_assert(sizeof(AuthChallenge) == 28, "NetTrx AuthChallenge layout");
static_assert(sizeof(AuthResponse) == 28, "NetTrx AuthResponse layout");
static_assert(sizeof(AudioCodecSelect) == 297, "NetTrx codec select layout");
static_assert(sizeof(Audio) == 2060, "NetTrx audio layout");
static_assert(sizeof(Squelch) == 142, "NetTrx squelch layout");
static_assert(sizeof(Dtmf) == 13, "NetTrx DTMF layout");
static_assert(sizeof(SiglevUpdate) == 13, "NetTrx siglev layout");
static_assert(sizeof(TransmitterStateChange) == 9,
              "NetTrx transmitter state layout");
static_assert(offsetof(Audio, payload_size) == 8, "NetTrx audio payload offset");
static_assert(offsetof(Audio, data) == 12, "NetTrx audio data offset");

} // namespace benshi::nettrx
