// SPDX-License-Identifier: GPL-2.0-or-later
#include "GaiaProtocol.h"

namespace benshi::gaia {

std::vector<uint8_t> buildCommand(uint16_t group, uint16_t command,
                                  const std::vector<uint8_t> &data) {
  std::vector<uint8_t> result;
  result.reserve(4 + data.size());
  result.push_back(static_cast<uint8_t>((group >> 8) & 0xFF));
  result.push_back(static_cast<uint8_t>(group & 0xFF));
  result.push_back(static_cast<uint8_t>((command >> 8) & 0xFF));
  result.push_back(static_cast<uint8_t>(command & 0xFF));
  result.insert(result.end(), data.begin(), data.end());
  return result;
}

std::vector<uint8_t> buildCommandByte(uint16_t group, uint16_t command,
                                      uint8_t value) {
  return buildCommand(group, command, {value});
}

std::vector<uint8_t> buildCommandInt(uint16_t group, uint16_t command,
                                     uint32_t value) {
  return buildCommand(group, command,
                      {static_cast<uint8_t>((value >> 24) & 0xFF),
                       static_cast<uint8_t>((value >> 16) & 0xFF),
                       static_cast<uint8_t>((value >> 8) & 0xFF),
                       static_cast<uint8_t>(value & 0xFF)});
}

std::vector<uint8_t> encode(const std::vector<uint8_t> &command) {
  if (command.size() < 4) {
    return {};
  }
  std::vector<uint8_t> result;
  result.reserve(command.size() + 4);
  result.push_back(0xFF);
  result.push_back(0x01);
  result.push_back(0x00);
  result.push_back(static_cast<uint8_t>((command.size() - 4) & 0xFF));
  result.insert(result.end(), command.begin(), command.end());
  return result;
}

bool decodeCommand(const uint8_t *data, size_t size, Frame &frame) {
  if (data == nullptr || size < 4) {
    return false;
  }
  frame.group = static_cast<uint16_t>((data[0] << 8) | data[1]);
  const uint16_t rawCommand = static_cast<uint16_t>((data[2] << 8) | data[3]);
  frame.isResponse = (rawCommand & 0x8000U) != 0;
  frame.command = static_cast<uint16_t>(rawCommand & 0x7FFFU);
  frame.payload.assign(data + 4, data + size);
  return true;
}

uint8_t getByte(const std::vector<uint8_t> &data, size_t offset) {
  return offset < data.size() ? data[offset] : 0;
}

uint16_t getShort(const std::vector<uint8_t> &data, size_t offset) {
  if (offset + 1 >= data.size()) {
    return 0;
  }
  return static_cast<uint16_t>((data[offset] << 8) | data[offset + 1]);
}

uint32_t getInt(const std::vector<uint8_t> &data, size_t offset) {
  if (offset + 3 >= data.size()) {
    return 0;
  }
  return (static_cast<uint32_t>(data[offset]) << 24) |
         (static_cast<uint32_t>(data[offset + 1]) << 16) |
         (static_cast<uint32_t>(data[offset + 2]) << 8) |
         static_cast<uint32_t>(data[offset + 3]);
}

void Decoder::push(const uint8_t *data, size_t size) {
  if (data != nullptr && size > 0) {
    buffer_.insert(buffer_.end(), data, data + size);
    if (buffer_.size() > 1024 * 1024) {
      buffer_.erase(buffer_.begin(), buffer_.end() - 4096);
    }
  }
}

bool Decoder::next(Frame &frame) {
  for (;;) {
    if (buffer_.size() < 2) {
      return false;
    }

    // Resynchronise on the GAIA start marker.
    if (buffer_[0] != 0xFF || buffer_[1] != 0x01) {
      buffer_.erase(buffer_.begin());
      continue;
    }

    if (buffer_.size() < 4) {
      return false;
    }

    const size_t payloadLength = buffer_[3];
    const size_t checksum = buffer_[2] & 0x01;
    const size_t totalLength = payloadLength + 8 + checksum;
    if (totalLength < 8 || totalLength > 65535) {
      buffer_.erase(buffer_.begin());
      continue;
    }
    if (buffer_.size() < totalLength) {
      return false;
    }

    Frame parsed;
    if (!decodeCommand(buffer_.data() + 4, payloadLength + 4, parsed)) {
      buffer_.erase(buffer_.begin());
      continue;
    }
    buffer_.erase(buffer_.begin(),
                  buffer_.begin() + static_cast<std::ptrdiff_t>(totalLength));
    frame = std::move(parsed);
    return true;
  }
}

} // namespace benshi::gaia
