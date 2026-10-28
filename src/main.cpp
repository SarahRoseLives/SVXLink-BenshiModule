// SPDX-License-Identifier: GPL-2.0-or-later
#include "Bridge.h"
#include "Config.h"
#include "Logger.h"
#include "bluetooth/RfcommChannel.h"
#include "radio/RadioSession.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

#ifndef SVXLINK_BENSHI_VERSION
#define SVXLINK_BENSHI_VERSION "0.1.0"
#endif

namespace {

volatile std::sig_atomic_t g_stop = 0;

void handleSignal(int) { g_stop = 1; }

std::string defaultConfigPath() {
  const char *env = std::getenv("SVXLINK_BENSHI_CONFIG");
  if (env != nullptr && env[0] != '\0') {
    return env;
  }
  if (access("svxlink-benshi.conf", R_OK) == 0) {
    return "svxlink-benshi.conf";
  }
  if (access("/etc/svxlink-benshi.conf", R_OK) == 0) {
    return "/etc/svxlink-benshi.conf";
  }
  return "/usr/local/etc/svxlink-benshi.conf";
}

void printUsage(const char *program) {
  std::cout
      << "Usage: " << program << " [options]\n\n"
      << "Bridge a Benshi/VR-N76 Bluetooth radio into SvxLink using SvxLink's\n"
      << "native NetTrx (RemoteTrx) protocol.\n\n"
      << "Options:\n"
      << "  -c, --config <file>  Configuration file (default: "
         "./svxlink-benshi.conf,\n"
      << "                       /etc/svxlink-benshi.conf, or "
         "/usr/local/etc/svxlink-benshi.conf)\n"
      << "  -v, --verbose        Enable debug logging\n"
      << "      --probe          Connect to the radio and print status\n"
      << "      --dump           Print the radio's channel/settings dump\n"
      << "      --unkey          Force the radio out of transmit\n"
      << "      --test-tx        Transmit a two second 1 kHz test tone\n"
      << "      --list           Scan for Bluetooth devices and exit\n"
      << "  -h, --help           Show this help\n"
      << "      --version        Show version\n";
}

void printRadioStatus(const benshi::RadioSession &radio) {
  const auto status = radio.status();
  const auto info = radio.deviceInfo();
  std::cout << "Radio: " << radio.macAddress() << "\n"
            << "  power: " << (status.powerOn ? "on" : "off") << "\n"
            << "  transmit: " << (status.inTx ? "yes" : "no") << "\n"
            << "  squelch: " << (status.squelchOpen ? "open" : "closed")
            << "\n"
            << "  receive: " << (status.inRx ? "yes" : "no") << "\n"
            << "  RSSI: " << status.rssi << "\n"
            << "  channel: " << status.channelId << "\n"
            << "  product: " << info.productId << "\n"
            << "  hardware: " << static_cast<unsigned>(info.hardwareVersion)
            << "\n"
            << "  software: " << info.softwareVersion << "\n";
}

} // namespace

int main(int argc, char **argv) {
  std::string configPath = defaultConfigPath();
  bool verbose = false;
  bool listDevices = false;
  bool probe = false;
  bool dump = false;
  bool unkey = false;
  bool testTx = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      printUsage(argv[0]);
      return 0;
    }
    if (arg == "--version") {
      std::cout << "svxlink-benshi " << SVXLINK_BENSHI_VERSION << "\n";
      return 0;
    }
    if (arg == "-v" || arg == "--verbose") {
      verbose = true;
      continue;
    }
    if (arg == "--list") {
      listDevices = true;
      continue;
    }
    if (arg == "--probe") {
      probe = true;
      continue;
    }
    if (arg == "--dump") {
      dump = true;
      continue;
    }
    if (arg == "--unkey") {
      unkey = true;
      continue;
    }
    if (arg == "--test-tx") {
      testTx = true;
      continue;
    }
    if (arg == "-c" || arg == "--config") {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for " << arg << "\n";
        return 2;
      }
      configPath = argv[++i];
      continue;
    }
    std::cerr << "Unknown option: " << arg << "\n";
    printUsage(argv[0]);
    return 2;
  }

  benshi::Logger logger(verbose ? benshi::LogLevel::Debug
                                : benshi::LogLevel::Info);

  if (listDevices) {
    std::string error;
    const auto devices = benshi::discoverBluetoothDevices(logger, error);
    if (!error.empty()) {
      std::cerr << error << "\n";
      return 1;
    }
    for (const auto &device : devices) {
      std::string name = device.name.empty() ? "(unknown)" : device.name;
      const std::string lower = [&]() {
        std::string value;
        for (char c : name) {
          value.push_back(
              static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        return value;
      }();
      const bool candidate =
          lower.find("vr-n") != std::string::npos ||
          lower.find("n7600") != std::string::npos ||
          lower.find("benshi") != std::string::npos;
      std::cout << device.address << "  " << name
                << (candidate ? "  [radio candidate]" : "") << "\n";
    }
    return 0;
  }

  benshi::Config config;
  std::string loadError;
  if (!config.load(configPath, loadError)) {
    std::cerr << loadError << "\n";
    return 1;
  }

  benshi::LogLevel level = verbose ? benshi::LogLevel::Debug
                                   : benshi::LogLevel::Info;
  const std::string configuredLevel = config.getString("LOG", "LEVEL", "");
  if (!configuredLevel.empty()) {
    benshi::parseLogLevel(configuredLevel, level);
  }
  logger.setLevel(level);
  const std::string logFile = config.getString("LOG", "FILE", "");
  if (!logFile.empty() && !logger.openFile(logFile)) {
    std::cerr << "Cannot open log file: " << logFile << "\n";
  }

  if (probe || dump || unkey || testTx) {
    benshi::RadioSession radio(logger, config);
    std::string error;
    if (!radio.connect(error)) {
      std::cerr << "Radio connection failed: " << error << "\n";
      return 1;
    }
    int result = 0;
    if (probe) {
      printRadioStatus(radio);
    }
    if (dump) {
      std::string text;
      if (radio.dumpConfiguration(text, error)) {
        std::cout << text;
      } else {
        std::cerr << "Dump failed: " << error << "\n";
        result = 1;
      }
    }
    if (unkey) {
      if (radio.forceUnkey(error)) {
        std::cout << "Radio unkeyed\n";
      } else {
        std::cerr << "Unkey failed: " << error << "\n";
        result = 1;
      }
    }
    if (testTx) {
      std::cout << "Transmitting 1 kHz test tone for 2 seconds...\n";
      if (!radio.transmitTestTone(1000.0, 2000, error)) {
        std::cerr << "Test transmission failed: " << error << "\n";
        result = 1;
      }
    }
    radio.disconnect();
    return result;
  }

  std::signal(SIGINT, handleSignal);
  std::signal(SIGTERM, handleSignal);

  benshi::Bridge bridge(logger, config);
  std::string startError;
  if (!bridge.start(startError)) {
    std::cerr << "Cannot start bridge: " << startError << "\n";
    return 1;
  }

  logger.info("svxlink-benshi " SVXLINK_BENSHI_VERSION " running; press "
              "Ctrl+C to stop");
  while (g_stop == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  bridge.stop();
  logger.info("Stopped");
  return 0;
}
