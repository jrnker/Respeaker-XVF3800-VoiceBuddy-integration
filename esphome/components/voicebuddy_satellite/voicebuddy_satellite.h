#pragma once

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/socket/socket.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

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
// Sat → hub liveness. Carries esp_reset_reason() + uptime_ms + free_heap.
// Sent once on READY (boot reason) and every STATUS_INTERVAL_MS thereafter.
static constexpr uint8_t FRAME_STATUS = 0x03;
static constexpr uint8_t FRAME_AUDIO = 0x10;
static constexpr uint8_t FRAME_WAKE = 0x11;
static constexpr uint8_t FRAME_VAD = 0x12;
static constexpr uint8_t FRAME_TTS_AUDIO = 0x20;
static constexpr uint8_t FRAME_STOP_TTS = 0x21;
static constexpr uint8_t FRAME_LED = 0x30;
// Hub-initiated training capture: open the mic gate without waiting for
// a wake-word fire so the hub can collect satellite-domain audio for
// custom-wake-word training. CAPTURE_START carries u16-be max_duration_ms;
// the satellite auto-stops at that deadline if CAPTURE_STOP never lands.
static constexpr uint8_t FRAME_CAPTURE_START = 0x40;
static constexpr uint8_t FRAME_CAPTURE_STOP = 0x41;
static constexpr uint8_t FRAME_PING = 0xF0;
static constexpr uint8_t FRAME_PONG = 0xF1;

static constexpr uint8_t TTS_FLAG_START = 0x01;
static constexpr uint8_t TTS_FLAG_CONT = 0x02;
static constexpr uint8_t TTS_FLAG_END = 0x04;

// Wire format: 16 kHz mono PCM-16. 20 ms = 320 samples = 640 bytes per AUDIO frame.
// The patched i2s_audio (formatBCE/esphome:respeaker_microphone) resamples
// the XVF3800's native 48 kHz / 32-bit / stereo down to 16 kHz / 16-bit /
// mono inside the microphone source, so consumers see 16 kHz directly and
// no firmware-side decimation is needed.
static constexpr size_t AUDIO_FRAME_BYTES = 640;
static constexpr size_t AUDIO_FRAME_SAMPLES = AUDIO_FRAME_BYTES / 2;

static constexpr uint32_t PING_INTERVAL_MS = 10000;
// Liveness/health frame interval. Slower than PING because it ships
// real payload (boot reason, uptime, free heap) — the hub side sets
// LIVENESS_SILENT_THRESHOLD_S to 3× this interval before warning.
// Sent independently of PING on the same socket; both run from loop().
static constexpr uint32_t STATUS_INTERVAL_MS = 30000;
// Hub blocks for 1-3 s on STT+TTS during process_wav_turn(); a 31 s watchdog
// is too tight when several utterances stack up. 60 s gives us six PING
// windows to recover before declaring the link dead.
static constexpr uint32_t WATCHDOG_TIMEOUT_MS = 60000;
static constexpr uint32_t RECONNECT_BACKOFF_MIN_MS = 500;
static constexpr uint32_t RECONNECT_BACKOFF_MAX_MS = 15000;

// =====================================================================
// VB_DEBUG_TPSTAT — temporary diagnostic logging for TTS chain throughput.
// Set to 0 to compile out entirely. Grep for "VB_DEBUG_TPSTAT" to find every
// participating site (this define, header members, cpp tracking points, the
// periodic log helper, the call from loop()) and rip them out when the
// speaker-chain sizing investigation is settled. Log lines are tagged
// "TPSTAT" so they're greppable in serial output too.
// =====================================================================
#define VB_DEBUG_TPSTAT 0

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

  // Runtime-config push from the provisioning flow. Unlike set_hub /
  // set_room_id (compile-time, called once from to_code()) these are
  // intended to run from `on_boot` after NVS reads. If `host`/`room` are
  // empty the component stays in DISCONNECTED and try_connect_ will keep
  // returning early — that's the "first boot, still in provisioning AP"
  // state. A non-empty push immediately re-arms reconnect.
  void set_config_runtime(const std::string &host, uint16_t port,
                          const std::string &room, const std::string &sat);
  // Drop the active session (if any) and zero out the live config. Used
  // by the factory-reset gesture; the YAML side still has to wipe the
  // NVS-backed text/number entities themselves, this only clears the
  // copies the running component holds.
  void factory_reset_runtime();
  bool has_runtime_config() const {
    return !this->hub_host_.empty() && !this->room_id_.empty();
  }

  // Driven from YAML automations on the wake-word callback.
  void on_wake(uint8_t wake_id, uint8_t confidence);
  void start_listening();
  void stop_listening();

  // True while a hub-initiated training capture is in flight. Exposed
  // so the YAML can suppress `paint_listening_leds` when a fortuitous
  // mWW fire lands during a capture — the LED paint script otherwise
  // leaves the ring lit indefinitely (capture has no on_tts_start to
  // clear it).
  bool is_capturing() const { return this->capturing_; }

  // Procedural two-tone confirmation, played through the configured speaker
  // when a wake fires. Replaces an audio_file:/media_player: stack we'd
  // otherwise need just for one short beep.
  void play_wake_beep_();

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
  // Build + send a STATUS frame. Boot reason is latched at setup() so we
  // can include it even after a reset has been forgotten by ESPHome.
  void send_status_();
  bool send_frame_(uint8_t typ, const uint8_t *payload, uint16_t len);
  void flush_tx_pending_();
  void flush_tts_pending_();
  void process_rx_();
  void handle_frame_(uint8_t typ, const uint8_t *payload, uint16_t len);
  void handle_tts_audio_(const uint8_t *payload, uint16_t len);
  void on_mic_data_(const std::vector<uint8_t> &data);
  void resolve_satellite_id_();
  // Hub-initiated training capture. Opens the mic gate without playing
  // the wake beep and arms an auto-stop timer so a lost CAPTURE_STOP
  // can't pin the mic open forever.
  void handle_capture_start_(const uint8_t *payload, uint16_t len);
  void handle_capture_stop_();

  std::string hub_host_;
  uint16_t hub_port_{9102};
  std::string room_id_;
  std::string satellite_id_;  // up to 16 ASCII chars; auto-derived if empty

  microphone::MicrophoneSource *mic_source_{nullptr};
  speaker::Speaker *speaker_{nullptr};

  std::unique_ptr<socket::Socket> sock_;
  // Mutex serialising all access to sock_ and tx_pending_. The mic task
  // (i2s_audio's mic_task) calls send_frame_ from a different FreeRTOS
  // task than loop()/disconnect_, so without this they race and one
  // task can write to a socket the other just freed.
  SemaphoreHandle_t sock_mutex_{nullptr};
  State state_{State::DISCONNECTED};
  bool listening_{false};
  bool mic_subscribed_{false};
  uint32_t session_id_{0};

  uint32_t last_attempt_ms_{0};
  uint32_t reconnect_delay_ms_{RECONNECT_BACKOFF_MIN_MS};
  uint32_t last_ping_ms_{0};
  uint32_t last_recv_ms_{0};
  // STATUS-send pacing. Reset on each connect so the first STATUS goes
  // out promptly. boot_reason_ is latched at setup() because
  // esp_reset_reason() can be cleared by subsequent calls inside IDF;
  // we want the original value across the whole session.
  uint32_t last_status_ms_{0};
  uint8_t boot_reason_{0};

  std::vector<uint8_t> rx_buf_;          // accumulating partial frames
  std::vector<uint8_t> tx_pending_;      // bytes deferred from a back-pressured / partial write
  std::vector<uint8_t> tts_pending_;     // PCM bytes the speaker chain didn't accept yet
  std::vector<int16_t> mic_pcm_buf_;     // 16 kHz samples awaiting a 320-sample frame

  // Training-capture state. Non-zero deadline = capture mode active;
  // suppresses the wake beep + WAKE frame so a fortuitous mWW fire
  // during training doesn't pollute the clip. loop() force-stops when
  // millis() passes the deadline.
  bool capturing_{false};
  uint32_t capture_deadline_ms_{0};

  CallbackManager<void()> on_connected_callbacks_;
  CallbackManager<void()> on_disconnected_callbacks_;
  CallbackManager<void()> on_tts_start_callbacks_;
  CallbackManager<void()> on_tts_end_callbacks_;

#if VB_DEBUG_TPSTAT
  // VB_DEBUG_TPSTAT — running totals for hub→sat ingress and sat→speaker-chain
  // egress, sampled once per second to derive bytes-per-second + heap and
  // pending-peak snapshots. Disabled by setting VB_DEBUG_TPSTAT to 0 in this
  // header; everything below compiles out cleanly.
  uint32_t debug_last_log_ms_{0};
  uint32_t debug_bytes_in_total_{0};
  uint32_t debug_bytes_played_total_{0};
  uint32_t debug_bytes_in_marker_{0};
  uint32_t debug_bytes_played_marker_{0};
  size_t debug_pending_peak_{0};
  void debug_log_tpstat_(uint32_t now);
#endif
};

// --- Automation actions ----------------------------------------------

// Action<Ts...>::play takes (const Ts &...x). With Ts empty (e.g. a switch
// trigger) `(Ts... x)` and `(const Ts &...x)` both collapse to `(void)` and
// `override` happens to work — but with Ts={std::string} (e.g. mWW's
// on_wake_word_detected, which surfaces `wake_word`), the value-vs-cref
// mismatch breaks `override`. Always use `const Ts &...x`.
template<typename... Ts> class WakeAction : public Action<Ts...> {
 public:
  explicit WakeAction(VoicebuddySatellite *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(uint8_t, wake_id)
  TEMPLATABLE_VALUE(uint8_t, confidence)
  void play(const Ts &...x) override {
    parent_->on_wake(this->wake_id_.value(x...), this->confidence_.value(x...));
  }

 protected:
  VoicebuddySatellite *parent_;
};

template<typename... Ts> class StartListeningAction : public Action<Ts...> {
 public:
  explicit StartListeningAction(VoicebuddySatellite *parent) : parent_(parent) {}
  void play(const Ts &...x) override { parent_->start_listening(); }

 protected:
  VoicebuddySatellite *parent_;
};

template<typename... Ts> class StopListeningAction : public Action<Ts...> {
 public:
  explicit StopListeningAction(VoicebuddySatellite *parent) : parent_(parent) {}
  void play(const Ts &...x) override { parent_->stop_listening(); }

 protected:
  VoicebuddySatellite *parent_;
};

// Push runtime config in from the provisioning flow. Each templatable
// pulls its value (typically from a `text:` / `number:` lambda that
// reads the NVS-backed config entity) on every play(); the runtime
// setter is a no-op when nothing has changed, so re-running this on
// boot every time is cheap.
template<typename... Ts> class SetConfigAction : public Action<Ts...> {
 public:
  explicit SetConfigAction(VoicebuddySatellite *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(std::string, hub_host)
  TEMPLATABLE_VALUE(uint16_t, hub_port)
  TEMPLATABLE_VALUE(std::string, room_id_value)
  TEMPLATABLE_VALUE(std::string, satellite_id_value)
  void play(const Ts &...x) override {
    parent_->set_config_runtime(this->hub_host_.value(x...),
                                this->hub_port_.value(x...),
                                this->room_id_value_.value(x...),
                                this->satellite_id_value_.value(x...));
  }

 protected:
  VoicebuddySatellite *parent_;
};

template<typename... Ts> class FactoryResetAction : public Action<Ts...> {
 public:
  explicit FactoryResetAction(VoicebuddySatellite *parent) : parent_(parent) {}
  void play(const Ts &...x) override { parent_->factory_reset_runtime(); }

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
