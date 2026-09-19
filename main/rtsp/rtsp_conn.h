#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hap.h"
#include "rtsp_server.h"

/**
 * RTSP Connection State Management
 * Consolidates all session state for an AirPlay connection
 */

// Forward declaration
typedef struct rtsp_conn rtsp_conn_t;

/**
 * Connection state struct - consolidates all session state
 */
struct rtsp_conn {
  // HAP session for pairing/encryption
  hap_session_t *hap_session;
  bool encrypted_mode;

  // 音量**不在这里**：它是设备级状态 (一个 DAC、一个 NVS 键)，放在 conn 里会
  // 逼着每帧的 apply_volume() 跨任务解引用这个指针。见 rtsp_server.h 的
  // airplay_apply_volume_db() / airplay_get_volume_q15()。

  // Audio streaming state
  bool stream_active;
  bool stream_paused;
  int64_t stream_type;    // 96=UDP realtime, 103=TCP buffered
  uint16_t data_port;     // UDP port for audio data (type 96)
  uint16_t control_port;  // UDP port for control (retransmit requests)
  uint16_t timing_port;   // UDP port for timing (our local port)
  uint16_t event_port;    // TCP port for server->client events
  uint16_t buffered_port; // TCP port for buffered audio (type 103)
  int data_socket;
  int control_socket;
  int event_socket; // TCP listener for event port

  // Client address for AirPlay 1 timing requests
  uint32_t client_ip;           // Client IP (network byte order)
  uint16_t client_timing_port;  // Client's timing port (for sending requests)
  uint16_t client_control_port; // Client's control port

  // Codec info from ANNOUNCE/SETUP
  char codec[32];
  int sample_rate;
  int channels;
  int bits_per_sample;

  // DACP identifiers for sending commands back to the client
  char dacp_id[32];       // DACP-ID header (hex string)
  char active_remote[32]; // Active-Remote header (token string)

  // AirPlay protocol version detected from request shape:
  //   0 = unknown (handshake not complete)
  //   1 = classic RAOP (Apple-Challenge / rsaaeskey / Transport: header)
  //   2 = AirPlay 2 (HAP / bplist streams)
  uint8_t protocol_version;
};

/**
 * Create a new connection state
 * @return Allocated connection state, or NULL on failure
 */
rtsp_conn_t *rtsp_conn_create(void);

/**
 * Free connection state and associated resources
 */
void rtsp_conn_free(rtsp_conn_t *conn);

/**
 * Reset stream-related state (called on stream teardown)
 * Keeps session alive but clears audio stream state
 */
void rtsp_conn_reset_stream(rtsp_conn_t *conn);

/**
 * Full cleanup when connection closes
 * Stops audio, closes sockets, clears PTP
 */
void rtsp_conn_cleanup(rtsp_conn_t *conn);
