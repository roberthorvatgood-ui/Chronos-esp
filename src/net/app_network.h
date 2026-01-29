
/*******************************************************
 * app_network.h — AP-only networking stubs
 * [2026-01-24 21:35 CET] UPDATE (Copilot):
 *   - Added network_portal_running() stub (returns false).
 *   - Clarified comments and grouped related APIs.
 *
 * [2026-01-20 23:15 CET] RH:
 *   Public API preserved; all STA functions are inert (no-ops).
 *   Safe for GUI and other call-sites: nothing will connect/scan.
 *******************************************************/
#pragma once
#include <stdint.h>

/*-------------------------------------------------------------------------
  Connection/portal state enum
---------------------------------------------------------------------------*/
enum class NetworkState : uint8_t {
  DISCONNECTED = 0,
  CONNECTING   = 1,
  CONNECTED    = 2,
  AUTH_ERROR   = 3,
  FAILED       = 4,
};

typedef void (*NetworkStateCallback)(NetworkState st, const char* ssid, const char* ip);

/*-------------------------------------------------------------------------
  Global enable/disable “Wi‑Fi feature” switch (AP-only stubs)
---------------------------------------------------------------------------*/
inline void network_set_enabled(bool) {}
inline bool network_get_enabled() { return false; }

/*-------------------------------------------------------------------------
  Lifecycle
---------------------------------------------------------------------------*/
inline void network_init() {}
inline void network_process_portal() {}

/*-------------------------------------------------------------------------
  Query whether a portal/connect FSM is active
  AP-only: never runs -> false
---------------------------------------------------------------------------*/
inline bool network_portal_running() { return false; }

/*-------------------------------------------------------------------------
  State callbacks (unused in AP-only)
---------------------------------------------------------------------------*/
inline void network_set_state_callback(NetworkStateCallback) {}

/*-------------------------------------------------------------------------
  STA controls – all no-ops in AP-only build
---------------------------------------------------------------------------*/
inline bool        network_connect_sta(const char*, const char*) { return false; }
inline void        network_disconnect() {}
inline void        network_reset_credentials() {}
inline void        network_request_user_disconnect() {}

/*-------------------------------------------------------------------------
  Status helpers – AP-only stubs
---------------------------------------------------------------------------*/
inline bool        network_is_connected()  { return false; }
inline bool        network_is_connecting() { return false; }
inline bool        network_is_busy()       { return false; }
inline const char* network_current_ssid()  { return ""; }
