#include "voicebuddy_satellite.h"

#include "esphome/components/microphone/microphone.h"
#include "esphome/components/microphone/microphone_source.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/components/socket/socket.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <cerrno>
#include <cmath>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace esphome {
namespace voicebuddy_satellite {

static const char *const TAG = "voicebuddy";

// Frame header layout (see docs/PROTOCOLS.md §5):
//   typ (u8) | length (u16-be) | payload
static constexpr size_t HEADER_LEN = 3;

namespace {
// RAII guard around the per-instance socket mutex. send_frame_ runs from
// both the main loop and the i2s mic task; without serialising they race
// on sock_ and tx_pending_, and a disconnect from one task crashes the
// other mid-write.
struct SockLock {
  SemaphoreHandle_t s;
  explicit SockLock(SemaphoreHandle_t sem) : s(sem) {
    if (s != nullptr) xSemaphoreTake(s, portMAX_DELAY);
  }
  ~SockLock() {
    if (s != nullptr) xSemaphoreGive(s);
  }
};
}  // namespace

// HELLO payload: sat_id(16) | room_id(16) | fw(u32-be) | caps(u32-be)
static constexpr size_t HELLO_LEN = 16 + 16 + 4 + 4;

// rx_buf_ sizing. Reserve is what we grab up front in setup() so vector
// growth doesn't fight a fragmented heap mid-stream. Hard cap is the
// disconnect threshold — if the buffer grows past this the link is
// pathologically backed up and reconnect is the safer recovery than
// letting it eat heap until OOM. ESP-IDF's exception stubs abort() on
// bad_alloc rather than throwing, so we have to *prevent* the failing
// allocation, not catch it.
static constexpr size_t RX_BUF_RESERVE = 16 * 1024;
static constexpr size_t RX_BUF_HARD_CAP = 48 * 1024;

// tts_pending_ holds PCM bytes the speaker chain rejected so flush_tts_pending_
// can retry next tick. Same heap-fragmentation hazard as rx_buf_: every realloc
// during a back-pressure stall risks bad_alloc → abort. We reserve the full cap
// at setup() so capacity is fixed for the device's lifetime, then refuse any
// incoming bytes that would push size past it (disconnect → hub re-establishes
// → cleaner failure than dropping samples and gradually collapsing).
// 16 kHz / 16-bit / mono = 32 kB/s, so 32 KiB ≈ 1 s of audio — past that
// user-visible lag would be worse than the disconnect anyway.
static constexpr size_t TTS_PENDING_HARD_CAP = 32 * 1024;

void VoicebuddySatellite::setup() {
  this->resolve_satellite_id_();
  // Pre-reserve generous buffer headroom up front, before mWW / Wi-Fi / BLE /
  // speaker chain fragment internal heap. Without this, the first TTS
  // burst forces vector realloc-doubling at exactly the moment heap is
  // tightest, and ESP-IDF's exception stubs abort() on bad_alloc rather
  // than throwing. tts_pending_ is reserved at the same value as its hard
  // cap so capacity is fixed for the device's lifetime — overflow is then
  // a clean disconnect rather than an allocation that may not fit.
  this->rx_buf_.reserve(RX_BUF_RESERVE);
  this->tts_pending_.reserve(TTS_PENDING_HARD_CAP);
  this->mic_pcm_buf_.reserve(AUDIO_FRAME_SAMPLES);
  this->sock_mutex_ = xSemaphoreCreateMutex();

  if (this->mic_source_ != nullptr) {
    // MicrophoneSource picks our channel and converts to int16 mono at the
    // wire-format rate (16 kHz). The patched i2s_audio from
    // formatBCE/esphome:respeaker_microphone resamples 48 kHz → 16 kHz at
    // the source layer so each consumer (us, micro_wake_word) gets 16 kHz
    // directly. Earlier v0.1 firmware did its own 3:1 decimation here
    // because the mic source ran at the raw 48 kHz; that's now gone.
    this->mic_source_->add_data_callback([this](const std::vector<uint8_t> &data) {
      this->on_mic_data_(data);
    });
    this->mic_subscribed_ = true;
  }

  if (this->has_runtime_config()) {
    ESP_LOGI(TAG, "voicebuddy_satellite room=%s hub=%s:%u",
             this->room_id_.c_str(), this->hub_host_.c_str(), this->hub_port_);
  } else {
    // Provisioning path: nothing baked in. Wait for set_config_runtime()
    // from the captive-portal YAML.
    ESP_LOGI(TAG, "voicebuddy_satellite awaiting runtime config (hub/room not provisioned)");
  }
}

void VoicebuddySatellite::dump_config() {
  ESP_LOGCONFIG(TAG, "VoiceBuddy satellite:");
  ESP_LOGCONFIG(TAG, "  Hub: %s:%u", this->hub_host_.c_str(), this->hub_port_);
  ESP_LOGCONFIG(TAG, "  Room ID: %s", this->room_id_.c_str());
  ESP_LOGCONFIG(TAG, "  Satellite ID: %s", this->satellite_id_.c_str());
  ESP_LOGCONFIG(TAG, "  Microphone: %s", this->mic_source_ ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  Speaker: %s", this->speaker_ ? "yes" : "no");
}

void VoicebuddySatellite::resolve_satellite_id_() {
  if (!this->satellite_id_.empty()) {
    return;
  }
  // Default to "vb-" + last 6 hex chars of MAC. Stable across reboots,
  // unique per device. Padded/truncated to ≤16 chars by the wire layer.
  uint8_t mac[6] = {0};
  esphome::get_mac_address_raw(mac);
  char buf[20];
  snprintf(buf, sizeof(buf), "vb-%02x%02x%02x", mac[3], mac[4], mac[5]);
  this->satellite_id_ = buf;
}

void VoicebuddySatellite::loop() {
  const uint32_t now = millis();

  switch (this->state_) {
    case State::DISCONNECTED: {
      if (now - this->last_attempt_ms_ >= this->reconnect_delay_ms_) {
        this->try_connect_();
      }
      return;
    }
    case State::CONNECTING:
    case State::HANDSHAKING:
    case State::READY:
      break;
  }

  // Flush any bytes left over from a back-pressured / partial write before
  // we touch the rx side or queue more sends.
  this->flush_tx_pending_();
  // Same for TTS: drain whatever the speaker chain rejected last tick before
  // we ingest more from process_rx_, otherwise we stack new chunks on top of
  // an already-full ring buffer and silently drop most samples.
  this->flush_tts_pending_();

  // Drain anything pending on the socket. process_rx_ may update
  // last_recv_ms_ to a fresher millis() than the `now` snapshot above,
  // so refresh `now` before the timer comparisons or the unsigned
  // subtraction underflows and falsely fires the watchdog.
  this->process_rx_();
  const uint32_t now_post_rx = millis();

  if (this->state_ == State::READY) {
    if (now_post_rx - this->last_ping_ms_ >= PING_INTERVAL_MS) {
      this->last_ping_ms_ = now_post_rx;
      this->send_frame_(FRAME_PING, nullptr, 0);
    }
    // Liveness watchdog — see WATCHDOG_TIMEOUT_MS for sizing rationale.
    if (now_post_rx - this->last_recv_ms_ > WATCHDOG_TIMEOUT_MS) {
      this->disconnect_("watchdog");
    }
  }
}

void VoicebuddySatellite::set_config_runtime(const std::string &host, uint16_t port,
                                             const std::string &room, const std::string &sat) {
  // Detect a meaningful change. The provisioning YAML calls this every
  // boot from on_boot, plus whenever the user edits a text/number entity
  // from the captive-portal page; on a steady-state reboot the values
  // are identical and there's nothing to do.
  bool host_changed = host != this->hub_host_;
  bool port_changed = port != this->hub_port_;
  bool room_changed = room != this->room_id_;
  bool sat_changed = !sat.empty() && sat != this->satellite_id_;

  if (!host_changed && !port_changed && !room_changed && !sat_changed) {
    return;
  }

  ESP_LOGI(TAG, "runtime config: hub=%s:%u room=%s sat=%s",
           host.c_str(), port, room.c_str(), sat.empty() ? "(auto)" : sat.c_str());

  this->hub_host_ = host;
  this->hub_port_ = port;
  this->room_id_ = room;
  if (!sat.empty()) {
    this->satellite_id_ = sat;
  } else if (this->satellite_id_.empty()) {
    // Re-derive from MAC if the YAML never gave us one and the user
    // didn't override via the UI.
    this->resolve_satellite_id_();
  }

  // If we're already mid-session and the hub/room moved, drop the link
  // so the next try_connect_ picks up the new target. Don't tear down on
  // pure-port-only changes that match the existing session.
  if (this->state_ != State::DISCONNECTED && (host_changed || port_changed || room_changed)) {
    this->disconnect_("config-changed");
  }

  // Re-arm reconnect immediately; try_connect_() will run on the next
  // loop tick now that has_runtime_config() is true.
  this->last_attempt_ms_ = 0;
  this->reconnect_delay_ms_ = RECONNECT_BACKOFF_MIN_MS;
}

void VoicebuddySatellite::factory_reset_runtime() {
  ESP_LOGW(TAG, "factory reset: dropping live config");
  if (this->state_ != State::DISCONNECTED) {
    this->disconnect_("factory-reset");
  }
  this->hub_host_.clear();
  this->room_id_.clear();
  // Keep satellite_id_ — the MAC-derived default is still valid and
  // saves us a step the next time the user provisions.
}

void VoicebuddySatellite::try_connect_() {
  this->last_attempt_ms_ = millis();

  // First boot before the captive-portal flow has supplied a hub: stay
  // parked in DISCONNECTED. Once set_config_runtime() pushes a real
  // host/room in we'll be invoked again the next loop tick.
  if (!this->has_runtime_config()) {
    ESP_LOGD(TAG, "no hub configured yet, waiting for provisioning");
    this->reconnect_delay_ms_ = RECONNECT_BACKOFF_MAX_MS;
    return;
  }

  struct addrinfo hints {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo *res = nullptr;
  char port_str[8];
  snprintf(port_str, sizeof(port_str), "%u", this->hub_port_);
  int err = ::getaddrinfo(this->hub_host_.c_str(), port_str, &hints, &res);
  if (err != 0 || res == nullptr) {
    ESP_LOGW(TAG, "DNS resolve failed for %s (err=%d)", this->hub_host_.c_str(), err);
    this->reconnect_delay_ms_ = std::min<uint32_t>(this->reconnect_delay_ms_ * 2, RECONNECT_BACKOFF_MAX_MS);
    return;
  }

  this->sock_ = socket::socket(res->ai_family, SOCK_STREAM, 0);
  if (this->sock_ == nullptr) {
    ::freeaddrinfo(res);
    ESP_LOGW(TAG, "socket() failed");
    this->reconnect_delay_ms_ = std::min<uint32_t>(this->reconnect_delay_ms_ * 2, RECONNECT_BACKOFF_MAX_MS);
    return;
  }

  this->sock_->setblocking(true);
  int rc = this->sock_->connect(res->ai_addr, res->ai_addrlen);
  ::freeaddrinfo(res);
  if (rc != 0) {
    ESP_LOGW(TAG, "connect to %s:%u failed", this->hub_host_.c_str(), this->hub_port_);
    this->sock_.reset();
    this->reconnect_delay_ms_ = std::min<uint32_t>(this->reconnect_delay_ms_ * 2, RECONNECT_BACKOFF_MAX_MS);
    return;
  }

  this->sock_->setblocking(false);
  this->state_ = State::HANDSHAKING;
  this->rx_buf_.clear();
  this->last_recv_ms_ = millis();
  this->last_ping_ms_ = millis();
  this->reconnect_delay_ms_ = RECONNECT_BACKOFF_MIN_MS;
  ESP_LOGI(TAG, "connected to hub %s:%u", this->hub_host_.c_str(), this->hub_port_);
  this->send_hello_();
}

void VoicebuddySatellite::disconnect_(const char *reason) {
  ESP_LOGW(TAG, "disconnecting: %s", reason);
  {
    SockLock g(this->sock_mutex_);
    if (this->sock_) {
      this->sock_->close();
      this->sock_.reset();
    }
    this->tx_pending_.clear();
  }
  bool was_ready = (this->state_ == State::READY);
  this->state_ = State::DISCONNECTED;
  this->listening_ = false;
  this->mic_pcm_buf_.clear();
  this->rx_buf_.clear();
  this->tts_pending_.clear();
  this->last_attempt_ms_ = millis();
  this->reconnect_delay_ms_ = std::min<uint32_t>(this->reconnect_delay_ms_ * 2, RECONNECT_BACKOFF_MAX_MS);
  if (was_ready) {
    this->on_disconnected_callbacks_.call();
  }
}

void VoicebuddySatellite::send_hello_() {
  uint8_t payload[HELLO_LEN] = {0};

  // satellite_id (16 ASCII bytes, NUL-padded)
  size_t n = std::min<size_t>(this->satellite_id_.size(), 16);
  std::memcpy(payload, this->satellite_id_.data(), n);

  // room_id (16 ASCII bytes, NUL-padded)
  n = std::min<size_t>(this->room_id_.size(), 16);
  std::memcpy(payload + 16, this->room_id_.data(), n);

  // fw_version (u32 BE) — pulled from PROJECT_VERSION when ESPHome compiles,
  // but for v0.1 we ship a single byte version marker.
  payload[32] = 0; payload[33] = 0; payload[34] = 0; payload[35] = 1;

  // caps (u32 BE) — bit 0 reserved for "always-on streaming"; v0.1 sends 0.
  payload[36] = 0; payload[37] = 0; payload[38] = 0; payload[39] = 0;

  this->send_frame_(FRAME_HELLO, payload, HELLO_LEN);
}

// Cap on how many bytes we're willing to hold back on a stalled socket
// before we start dropping new AUDIO frames. ~2× a TTS-sized chunk leaves
// headroom for a couple of stalled control frames without unbounded growth.
static constexpr size_t TX_PENDING_SOFT_CAP = 4096;

void VoicebuddySatellite::flush_tx_pending_() {
  bool need_disconnect = false;
  {
    SockLock g(this->sock_mutex_);
    if (this->tx_pending_.empty() || !this->sock_) return;
    ssize_t w = this->sock_->write(this->tx_pending_.data(), this->tx_pending_.size());
    if (w == static_cast<ssize_t>(this->tx_pending_.size())) {
      this->tx_pending_.clear();
      return;
    }
    if (w < 0) {
      int e = errno;
      if (e == EAGAIN || e == EWOULDBLOCK) return;  // kernel still full, retry next tick
      need_disconnect = true;
    } else {
      // Partial flush — drop the bytes that made it onto the wire, keep the rest.
      this->tx_pending_.erase(this->tx_pending_.begin(), this->tx_pending_.begin() + w);
    }
  }
  if (need_disconnect) this->disconnect_("flush err");
}

bool VoicebuddySatellite::send_frame_(uint8_t typ, const uint8_t *payload, uint16_t len) {
  // Coalesce header + payload so a frame is never split across two syscalls.
  std::vector<uint8_t> frame(HEADER_LEN + len);
  frame[0] = typ;
  frame[1] = (len >> 8) & 0xFF;
  frame[2] = len & 0xFF;
  if (len > 0 && payload != nullptr) {
    std::memcpy(frame.data() + HEADER_LEN, payload, len);
  }

  bool need_disconnect = false;
  bool ok = false;
  {
    SockLock g(this->sock_mutex_);
    if (!this->sock_) return false;

    // If we already have a backlog from a previous partial / blocked write,
    // try to drain it inline before queueing more behind it. (Inline drain
    // because flush_tx_pending_ would deadlock on the same mutex.)
    if (!this->tx_pending_.empty()) {
      ssize_t fw = this->sock_->write(this->tx_pending_.data(), this->tx_pending_.size());
      if (fw == static_cast<ssize_t>(this->tx_pending_.size())) {
        this->tx_pending_.clear();
      } else if (fw > 0) {
        this->tx_pending_.erase(this->tx_pending_.begin(),
                                this->tx_pending_.begin() + fw);
      }
      // Negative fw with EWOULDBLOCK is fine — we'll just queue this frame.

      if (!this->tx_pending_.empty()) {
        // Still backed up. AUDIO is real-time and lossy by design; dropping
        // a frame is cheaper than ballooning the queue with stale samples.
        if (typ == FRAME_AUDIO &&
            this->tx_pending_.size() + frame.size() > TX_PENDING_SOFT_CAP) {
          return false;
        }
        this->tx_pending_.insert(this->tx_pending_.end(), frame.begin(), frame.end());
        return true;
      }
    }

    ssize_t w = this->sock_->write(frame.data(), frame.size());
    if (w == static_cast<ssize_t>(frame.size())) {
      return true;
    }
    if (w < 0) {
      int e = errno;
      if (e == EAGAIN || e == EWOULDBLOCK) {
        if (typ == FRAME_AUDIO) {
          return false;  // drop, don't queue stale audio
        }
        this->tx_pending_.assign(frame.begin(), frame.end());
        return true;
      }
      need_disconnect = true;
      ok = false;
    } else {
      // Short write — queue the tail and try again next loop tick.
      this->tx_pending_.assign(frame.begin() + w, frame.end());
      return true;
    }
  }
  if (need_disconnect) this->disconnect_("send err");
  return ok;
}

void VoicebuddySatellite::process_rx_() {
  if (!this->sock_) return;

  uint8_t buf[1024];
  for (;;) {
    ssize_t n = this->sock_->read(buf, sizeof(buf));
    if (n <= 0) {
      if (n == 0) {
        this->disconnect_("eof");
      }
      // n<0 with EWOULDBLOCK is the normal "no data right now" case.
      break;
    }
    this->last_recv_ms_ = millis();
    if (this->rx_buf_.size() + static_cast<size_t>(n) > RX_BUF_HARD_CAP) {
      // Buffer is pathologically backed up — frame parser must be stuck
      // (e.g. a corrupt length pulled an absurd payload size). Disconnect
      // before the next insert tries to grow the vector and OOMs.
      ESP_LOGE(TAG, "rx_buf overflow (%u + %u > %u), forcing reconnect",
               (unsigned) this->rx_buf_.size(), (unsigned) n,
               (unsigned) RX_BUF_HARD_CAP);
      this->disconnect_("rx overflow");
      return;
    }
    this->rx_buf_.insert(this->rx_buf_.end(), buf, buf + n);
  }

  // Pull as many complete frames as the buffer holds.
  for (;;) {
    if (this->rx_buf_.size() < HEADER_LEN) return;
    uint8_t typ = this->rx_buf_[0];
    uint16_t len = (uint16_t(this->rx_buf_[1]) << 8) | uint16_t(this->rx_buf_[2]);
    if (this->rx_buf_.size() < HEADER_LEN + len) return;
    this->handle_frame_(typ, this->rx_buf_.data() + HEADER_LEN, len);
    this->rx_buf_.erase(this->rx_buf_.begin(), this->rx_buf_.begin() + HEADER_LEN + len);
  }
}

void VoicebuddySatellite::handle_frame_(uint8_t typ, const uint8_t *payload, uint16_t len) {
  switch (typ) {
    case FRAME_WELCOME: {
      if (this->state_ != State::HANDSHAKING) {
        ESP_LOGW(TAG, "stray WELCOME (state=%d)", static_cast<int>(this->state_));
        return;
      }
      if (len >= 12) {
        this->session_id_ = (uint32_t(payload[8]) << 24) | (uint32_t(payload[9]) << 16) |
                            (uint32_t(payload[10]) << 8) | uint32_t(payload[11]);
      }
      this->state_ = State::READY;
      ESP_LOGI(TAG, "session opened id=%u", this->session_id_);
      this->on_connected_callbacks_.call();
      return;
    }
    case FRAME_TTS_AUDIO:
      this->handle_tts_audio_(payload, len);
      return;
    case FRAME_STOP_TTS:
      ESP_LOGI(TAG, "stop_tts received");
      // Speaker barge-in handling lands in v0.2.
      this->on_tts_end_callbacks_.call();
      return;
    case FRAME_PING:
      this->send_frame_(FRAME_PONG, nullptr, 0);
      return;
    case FRAME_PONG:
      return;
    case FRAME_LED:
      // v0.2: route into the LED ring driver.
      return;
    default:
      ESP_LOGD(TAG, "ignored frame typ=0x%02x len=%u", typ, len);
      return;
  }
}

void VoicebuddySatellite::flush_tts_pending_() {
  if (this->tts_pending_.empty() || this->speaker_ == nullptr) return;
  size_t accepted = this->speaker_->play(this->tts_pending_.data(), this->tts_pending_.size());
  if (accepted == 0) return;  // chain still full, retry next tick
  if (accepted >= this->tts_pending_.size()) {
    this->tts_pending_.clear();
    return;
  }
  this->tts_pending_.erase(this->tts_pending_.begin(),
                           this->tts_pending_.begin() + accepted);
}

void VoicebuddySatellite::handle_tts_audio_(const uint8_t *payload, uint16_t len) {
  if (len < 1) return;
  const uint8_t flags = payload[0];
  const uint8_t *pcm = payload + 1;
  const uint16_t pcm_len = len - 1;

  if (flags & TTS_FLAG_START) {
    this->on_tts_start_callbacks_.call();
  }

  if (this->speaker_ != nullptr && pcm_len > 0) {
    // ESPHome speaker::Speaker accepts raw PCM. The hub guarantees 16 kHz
    // mono PCM-16 little-endian (see hub/.../bark/server.py); the receiver
    // YAML routes this through a resampler+mixer chain to land at the
    // hardware rate. play() returns the number of bytes actually queued —
    // anything above that exceeds the chain's ring buffer and would be
    // dropped, so park the tail in tts_pending_ and feed it from loop().
    if (!this->tts_pending_.empty()) {
      // Already backed up — append instead of calling play() and risking
      // out-of-order chunks. Cap check is against the reserved capacity:
      // overflowing here means the speaker chain has been stalled long
      // enough that we'd rather reset the session than keep accumulating.
      if (this->tts_pending_.size() + pcm_len > TTS_PENDING_HARD_CAP) {
        ESP_LOGE(TAG, "tts_pending overflow (%u + %u > %u), forcing reconnect",
                 (unsigned) this->tts_pending_.size(), (unsigned) pcm_len,
                 (unsigned) TTS_PENDING_HARD_CAP);
        this->disconnect_("tts overflow");
        return;
      }
      this->tts_pending_.insert(this->tts_pending_.end(), pcm, pcm + pcm_len);
    } else {
      size_t accepted = this->speaker_->play(pcm, pcm_len);
      if (accepted < pcm_len) {
        size_t remainder = pcm_len - accepted;
        if (remainder > TTS_PENDING_HARD_CAP) {
          ESP_LOGE(TAG, "tts_pending overflow (assign %u > %u), forcing reconnect",
                   (unsigned) remainder, (unsigned) TTS_PENDING_HARD_CAP);
          this->disconnect_("tts overflow");
          return;
        }
        this->tts_pending_.assign(pcm + accepted, pcm + pcm_len);
      }
    }
  }

  if (flags & TTS_FLAG_END) {
    this->on_tts_end_callbacks_.call();
  }
}

void VoicebuddySatellite::on_wake(uint8_t wake_id, uint8_t confidence) {
  if (this->state_ != State::READY) {
    ESP_LOGD(TAG, "wake ignored, not ready (state=%d)", static_cast<int>(this->state_));
    return;
  }
  uint8_t payload[2] = {wake_id, confidence};
  this->send_frame_(FRAME_WAKE, payload, sizeof(payload));
  this->play_wake_beep_();
  this->start_listening();
}

void VoicebuddySatellite::play_wake_beep_() {
  if (this->speaker_ == nullptr) return;

  // Two short rising tones with quick fades — pleasant, ~210 ms total.
  // The configured speaker is `tts_speaker` (resampler), which accepts
  // 16 kHz / 16-bit / mono PCM; the rest of the chain widens to 48 kHz /
  // 32-bit / stereo for the AIC3104 DAC. Channel 0 of the mic is
  // AEC-processed, so the beep won't bleed into the audio sent uplink.
  constexpr uint32_t SAMPLE_RATE = 16000;
  constexpr float TONE1_HZ = 1000.0f;
  constexpr float TONE2_HZ = 1320.0f;  // major third + perfect fourth-ish
  constexpr uint32_t TONE_MS = 60;
  constexpr uint32_t GAP_MS = 30;
  constexpr uint32_t FADE_MS = 8;
  constexpr float AMPLITUDE = 0.20f;  // -14 dBFS, deliberately gentle
  constexpr float TWO_PI = 6.28318530718f;

  const size_t tone_samples = (SAMPLE_RATE * TONE_MS) / 1000;
  const size_t gap_samples = (SAMPLE_RATE * GAP_MS) / 1000;
  const size_t fade_samples = (SAMPLE_RATE * FADE_MS) / 1000;
  const size_t total_samples = tone_samples * 2 + gap_samples;

  std::vector<int16_t> pcm(total_samples, 0);

  auto write_tone = [&](size_t offset, float freq) {
    const float w = TWO_PI * freq / static_cast<float>(SAMPLE_RATE);
    for (size_t i = 0; i < tone_samples; i++) {
      float env = AMPLITUDE;
      if (i < fade_samples) {
        env *= static_cast<float>(i) / static_cast<float>(fade_samples);
      } else if (i >= tone_samples - fade_samples) {
        env *= static_cast<float>(tone_samples - i) / static_cast<float>(fade_samples);
      }
      float s = sinf(w * static_cast<float>(i));
      pcm[offset + i] = static_cast<int16_t>(s * env * 32767.0f);
    }
  };

  write_tone(0, TONE1_HZ);
  // gap is already zero
  write_tone(tone_samples + gap_samples, TONE2_HZ);

  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(pcm.data());
  const size_t total_bytes = pcm.size() * sizeof(int16_t);
  size_t accepted = this->speaker_->play(bytes, total_bytes);
  if (accepted < total_bytes) {
    // Park the rest in tts_pending_ so loop()'s flush_tts_pending_ drains it.
    size_t remainder = total_bytes - accepted;
    // Wake beep is cosmetic feedback; if tts_pending_ is already near cap
    // (unlikely between turns), drop the tail silently rather than disconnect.
    if (this->tts_pending_.size() + remainder <= TTS_PENDING_HARD_CAP) {
      this->tts_pending_.insert(this->tts_pending_.end(),
                                 bytes + accepted, bytes + total_bytes);
    }
  }
}

void VoicebuddySatellite::start_listening() {
  if (this->listening_) return;
  this->listening_ = true;
  this->mic_pcm_buf_.clear();
  if (this->mic_source_ != nullptr) {
    this->mic_source_->start();
  }
  ESP_LOGI(TAG, "listening");
}

void VoicebuddySatellite::stop_listening() {
  if (!this->listening_) return;
  this->listening_ = false;
  this->mic_pcm_buf_.clear();
  if (this->mic_source_ != nullptr) {
    this->mic_source_->stop();
  }
  ESP_LOGI(TAG, "stopped listening");
}

void VoicebuddySatellite::on_mic_data_(const std::vector<uint8_t> &data) {
  if (!this->listening_ || this->state_ != State::READY) return;

  // MicrophoneSource hands us 16 kHz / 16-bit / mono PCM, already framed at
  // the wire-format rate by the patched i2s_audio. Buffer samples until we
  // have a full 320-sample (640-byte) AUDIO frame, then ship.
  const int16_t *samples = reinterpret_cast<const int16_t *>(data.data());
  const size_t sample_count = data.size() / 2;
  this->mic_pcm_buf_.insert(this->mic_pcm_buf_.end(), samples, samples + sample_count);

  while (this->mic_pcm_buf_.size() >= AUDIO_FRAME_SAMPLES) {
    this->send_frame_(FRAME_AUDIO,
                      reinterpret_cast<const uint8_t *>(this->mic_pcm_buf_.data()),
                      AUDIO_FRAME_BYTES);
    this->mic_pcm_buf_.erase(this->mic_pcm_buf_.begin(),
                             this->mic_pcm_buf_.begin() + AUDIO_FRAME_SAMPLES);
  }
}

}  // namespace voicebuddy_satellite
}  // namespace esphome
