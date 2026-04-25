#pragma once

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/socket/socket.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace esphome {

namespace microphone {
class MicrophoneSource;
}
namespace speaker {
class Speaker;
}

namespace voicebuddy_satellite {

// Wire constants (mirror docs/PROTOCOLS.md §5).
static constexpr uint8_t FRAME_HELLO = 0x01;
static constexpr uint8_t FRAME_WELCOME = 0x02;
static constexpr uint8_t FRAME_AUDIO = 0x10;
static constexpr uint8_t FRAME_WAKE = 0x11;
static constexpr uint8_t FRAME_VAD = 0x12;
static constexpr uint8_t FRAME_TTS_AUDIO = 0x20;
static constexpr uint8_t FRAME_STOP_TTS = 0x21;
static constexpr uint8_t FRAME_LED = 0x30;
static constexpr uint8_t FRAME_PING = 0xF0;
static constexpr uint8_t FRAME_PONG = 0xF1;

static constexpr uint8_t TTS_FLAG_START = 0x01;
static constexpr uint8_t TTS_FLAG_CONT = 0x02;
static constexpr uint8_t TTS_FLAG_END = 0x04;

// Wire format: 16 kHz mono PCM-16. 20 ms = 320 samples = 640 bytes per AUDIO frame.
static constexpr size_t AUDIO_FRAME_BYTES = 640;
static constexpr size_t AUDIO_FRAME_SAMPLES = AUDIO_FRAME_BYTES / 2;

// XVF3800 native I2S output is 48 kHz; we decimate 3:1 in firmware to keep
// the wire format stable. v0.2 will negotiate the rate via HELLO.
static constexpr uint8_t MIC_DECIMATION_FACTOR = 3;

static constexpr uint32_t PING_INTERVAL_MS = 10000;
// Hub blocks for 1-3 s on STT+TTS during process_wav_turn(); a 31 s watchdog
// is too tight when several utterances stack up. 60 s gives us six PING
// windows to recover before declaring the link dead.
static constexpr uint32_t WATCHDOG_TIMEOUT_MS = 60000;
static constexpr uint32_t RECONNECT_BACKOFF_MIN_MS = 500;
static constexpr uint32_t RECONNECT_BACKOFF_MAX_MS = 15000;

class VoicebuddySatellite : public Component {
 public:
  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }
  void dump_config() override;

  void set_hub(const std::string &host, uint16_t port) {
    hub_host_ = host;
    hub_port_ = port;
  }
  void set_room_id(const std::string &r) { room_id_ = r; }
  void set_satellite_id(const std::string &s) { satellite_id_ = s; }
  void set_microphone_source(microphone::MicrophoneSource *s) { mic_source_ = s; }
  void set_speaker(speaker::Speaker *s) { speaker_ = s; }

  // Driven from YAML automations on the wake-word callback.
  void on_wake(uint8_t wake_id, uint8_t confidence);
  void start_listening();
  void stop_listening();

  void add_on_connected_callback(std::function<void()> &&cb) {
    on_connected_callbacks_.add(std::move(cb));
  }
  void add_on_disconnected_callback(std::function<void()> &&cb) {
    on_disconnected_callbacks_.add(std::move(cb));
  }
  void add_on_tts_start_callback(std::function<void()> &&cb) {
    on_tts_start_callbacks_.add(std::move(cb));
  }
  void add_on_tts_end_callback(std::function<void()> &&cb) {
    on_tts_end_callbacks_.add(std::move(cb));
  }

 protected:
  enum class State : uint8_t {
    DISCONNECTED,
    CONNECTING,
    HANDSHAKING,
    READY,
  };

  void try_connect_();
  void disconnect_(const char *reason);
  void send_hello_();
  bool send_frame_(uint8_t typ, const uint8_t *payload, uint16_t len);
  void flush_tx_pending_();
  void process_rx_();
  void handle_frame_(uint8_t typ, const uint8_t *payload, uint16_t len);
  void handle_tts_audio_(const uint8_t *payload, uint16_t len);
  void on_mic_data_(const std::vector<uint8_t> &data);
  void resolve_satellite_id_();

  std::string hub_host_;
  uint16_t hub_port_{9102};
  std::string room_id_;
  std::string satellite_id_;  // up to 16 ASCII chars; auto-derived if empty

  microphone::MicrophoneSource *mic_source_{nullptr};
  speaker::Speaker *speaker_{nullptr};

  std::unique_ptr<socket::Socket> sock_;
  State state_{State::DISCONNECTED};
  bool listening_{false};
  bool mic_subscribed_{false};
  uint32_t session_id_{0};

  uint32_t last_attempt_ms_{0};
  uint32_t reconnect_delay_ms_{RECONNECT_BACKOFF_MIN_MS};
  uint32_t last_ping_ms_{0};
  uint32_t last_recv_ms_{0};

  std::vector<uint8_t> rx_buf_;          // accumulating partial frames
  std::vector<uint8_t> tx_pending_;      // bytes deferred from a back-pressured / partial write
  std::vector<int16_t> mic_pcm_buf_;     // 16 kHz samples awaiting a 320-sample frame
  uint8_t mic_decim_phase_{0};           // round-robin counter for 3:1 decimation

  CallbackManager<void()> on_connected_callbacks_;
  CallbackManager<void()> on_disconnected_callbacks_;
  CallbackManager<void()> on_tts_start_callbacks_;
  CallbackManager<void()> on_tts_end_callbacks_;
};

// --- Automation actions ----------------------------------------------

template<typename... Ts> class WakeAction : public Action<Ts...> {
 public:
  explicit WakeAction(VoicebuddySatellite *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(uint8_t, wake_id)
  TEMPLATABLE_VALUE(uint8_t, confidence)
  void play(Ts... x) override {
    parent_->on_wake(this->wake_id_.value(x...), this->confidence_.value(x...));
  }

 protected:
  VoicebuddySatellite *parent_;
};

template<typename... Ts> class StartListeningAction : public Action<Ts...> {
 public:
  explicit StartListeningAction(VoicebuddySatellite *parent) : parent_(parent) {}
  void play(Ts... x) override { parent_->start_listening(); }

 protected:
  VoicebuddySatellite *parent_;
};

template<typename... Ts> class StopListeningAction : public Action<Ts...> {
 public:
  explicit StopListeningAction(VoicebuddySatellite *parent) : parent_(parent) {}
  void play(Ts... x) override { parent_->stop_listening(); }

 protected:
  VoicebuddySatellite *parent_;
};

class OnConnectedTrigger : public Trigger<> {
 public:
  explicit OnConnectedTrigger(VoicebuddySatellite *parent) {
    parent->add_on_connected_callback([this]() { this->trigger(); });
  }
};

class OnDisconnectedTrigger : public Trigger<> {
 public:
  explicit OnDisconnectedTrigger(VoicebuddySatellite *parent) {
    parent->add_on_disconnected_callback([this]() { this->trigger(); });
  }
};

class OnTtsStartTrigger : public Trigger<> {
 public:
  explicit OnTtsStartTrigger(VoicebuddySatellite *parent) {
    parent->add_on_tts_start_callback([this]() { this->trigger(); });
  }
};

class OnTtsEndTrigger : public Trigger<> {
 public:
  explicit OnTtsEndTrigger(VoicebuddySatellite *parent) {
    parent->add_on_tts_end_callback([this]() { this->trigger(); });
  }
};

}  // namespace voicebuddy_satellite
}  // namespace esphome
