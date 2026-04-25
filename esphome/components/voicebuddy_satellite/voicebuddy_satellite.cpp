#include "voicebuddy_satellite.h"

#include "esphome/components/microphone/microphone.h"
#include "esphome/components/microphone/microphone_source.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/components/socket/socket.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <cerrno>
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

void VoicebuddySatellite::setup() {
  this->resolve_satellite_id_();
  this->rx_buf_.reserve(2048);
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

  ESP_LOGI(TAG, "voicebuddy_satellite room=%s hub=%s:%u",
           this->room_id_.c_str(), this->hub_host_.c_str(), this->hub_port_);
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

void VoicebuddySatellite::try_connect_() {
  this->last_attempt_ms_ = millis();

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

// Cap on how many PCM bytes we're willing to hold back when the speaker
// chain stalls. 16 kHz / 16-bit / mono = 32 kB/s, so 32 KiB ≈ 1 s of audio.
// Past that the user-visible lag would be worse than dropping samples.
static constexpr size_t TTS_PENDING_SOFT_CAP = 32 * 1024;

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
      // Already backed up — append (subject to the soft cap) instead of
      // calling play() and risking out-of-order chunks.
      if (this->tts_pending_.size() + pcm_len <= TTS_PENDING_SOFT_CAP) {
        this->tts_pending_.insert(this->tts_pending_.end(), pcm, pcm + pcm_len);
      } else {
        ESP_LOGW(TAG, "tts pending cap %u exceeded, dropping %u bytes",
                 (unsigned) this->tts_pending_.size(), (unsigned) pcm_len);
      }
    } else {
      size_t accepted = this->speaker_->play(pcm, pcm_len);
      if (accepted < pcm_len) {
        size_t remainder = pcm_len - accepted;
        if (remainder <= TTS_PENDING_SOFT_CAP) {
          this->tts_pending_.assign(pcm + accepted, pcm + pcm_len);
        } else {
          ESP_LOGW(TAG, "tts pending cap %u exceeded, dropping %u bytes",
                   (unsigned) TTS_PENDING_SOFT_CAP, (unsigned) remainder);
        }
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
  this->start_listening();
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
