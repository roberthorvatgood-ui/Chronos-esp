
#pragma once
#include <stdint.h>
#include <vector>
#include <string>
#include <Print.h>

// Lifecycle
void experiments_init();
void experiments_clear_timestamps(); // resets timing state in gate engine

// ------------------------- Settings -------------------------
// CV
void experiments_set_distance_mm(uint16_t mm);
uint16_t experiments_get_distance_mm();

// Photogate
void experiments_set_flag_mm(uint16_t mm);
uint16_t experiments_get_flag_mm();

// UA (two‑gate) — D REMOVED. Only object length L remains.
void experiments_set_ua_len_mm(uint16_t object_len_mm);
uint16_t experiments_get_ua_len_mm();

// UA legacy (3‑gate) positions kept for compatibility; unused by two‑gate math
void experiments_set_ua_positions(uint16_t p0, uint16_t p1, uint16_t p2);
void experiments_get_ua_positions(uint16_t& p0, uint16_t& p1, uint16_t& p2);

// Free Fall (1 gate, object length + drop height)
void experiments_set_freefall_params(uint16_t object_len_mm, uint16_t drop_height_mm);
void experiments_get_freefall_params(uint16_t& object_len_mm, uint16_t& drop_height_mm);

// Inclined Plane (2 gates, object length + gate distance + angle)
void experiments_set_incline_params(uint16_t object_len_mm, uint16_t gate_dist_mm, float angle_deg);
void experiments_get_incline_params(uint16_t& object_len_mm, uint16_t& gate_dist_mm, float& angle_deg);

// Tachometer (slotted disk)
void experiments_set_tacho_params(uint16_t slots);
uint16_t experiments_get_tacho_slots();

// ------------------------- Record APIs -------------------------
// GUI uses these to compute/store a measurement and to get a human‑readable formula.
// CV
bool experiments_record_cv(double& speed_mps, double& time_ms, std::string& formula);

// Photogate
bool experiments_record_photogate(double& speed_mps, double& time_ms, std::string& formula);

// UA (two‑gate only). Returns:
// - acc_mps2, speed_mid_mps, total_ms, formula
// - OPTIONAL: v1_out and v2_out for raw gate speeds (m/s)
bool experiments_record_ua(double& acc_mps2,
                           double& speed_mid_mps,
                           double& total_ms,
                           std::string& formula,
                           double* v1_out = nullptr,
                           double* v2_out = nullptr);

// New experiments
bool experiments_record_freefall(double& v_mps, double& g_mps2, double& tau_ms, std::string& formula);
bool experiments_record_incline(double& a_mps2, double& v1_mps, double& v2_mps, double& total_ms, std::string& formula);
bool experiments_record_tacho(double& rpm, double& period_ms, std::string& formula);

// ------------------------- History -------------------------
std::vector<std::string> experiments_get_last10(const char* mode);
void experiments_clear_history(const char* mode);


// Writes the full experiments CSV to any Print (Serial, File, WiFiClient, etc.)
void experiments_emit_csv(Print& out);

// Existing convenience wrapper preserved (prints to Serial)
void experiments_export_csv();

// ------------------------- CSV Export -------------------------
void experiments_export_csv();
