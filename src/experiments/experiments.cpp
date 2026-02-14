
#include "experiments.h"
#include "../core/gate_engine.h"
#include <Arduino.h>
#include <Preferences.h>
#include <vector>
#include <string>
#include <math.h>
#include <string.h>
#include "../intl/i18n.h" // ← added for tr()

// Experiment State Management
static ExperimentState s_experiment_state = ExperimentState::IDLE;

ExperimentState experiment_get_state() {
  return s_experiment_state;
}

void experiment_set_state(ExperimentState s) {
  if (s_experiment_state != s) {
    s_experiment_state = s;
    
    switch (s) {
      case ExperimentState::IDLE:
        Serial.println("[Experiment] State → IDLE (gate polling OFF)");
        break;
      case ExperimentState::ARMED:
        Serial.println("[Experiment] State → ARMED (gate polling ON)");
        break;
      case ExperimentState::RUNNING:
        Serial.println("[Experiment] State → RUNNING (gate polling ON)");
        break;
      case ExperimentState::FINISHED:
        Serial.println("[Experiment] State → FINISHED (gate polling OFF)");
        break;
    }
  }
}

bool experiment_should_poll_gates() {
  return (s_experiment_state == ExperimentState::ARMED || 
          s_experiment_state == ExperimentState::RUNNING);
}

// ================= Settings =================
static uint16_t s_distance_mm = 500; // CV
static uint16_t s_flag_mm = 50;      // Photogate
static uint16_t s_obj_len_mm = 50;   // UA (L only; D removed)
static uint16_t s_posA = 0, s_posB = 500, s_posC = 1000; // legacy UA positions
static uint16_t s_ff_len_mm = 50;    // FreeFall
static uint16_t s_ff_drop_mm = 500;  // FreeFall
static uint16_t s_in_len_mm = 50;    // Inclined Plane (object edge blocking length)
static uint16_t s_in_dist_mm = 500;  // Inclined Plane (distance between gates)
static uint16_t s_in_angle_deg= 10;// Inclined Plane (angle for theory/compare)
static uint16_t s_tacho_slots = 1;   // Tachometer

// ================= Storage =================
struct Run {
  std::string mode;
  uint32_t    run;
  double      d_mm;        // generic numeric column; UA: L; Incline: gate distance D
  double      time_ms;     // for UA/Incline: Δt_front (front-edge time between A and B)
  double      speed_mps;   // main speed value (use v2 for Incline; mid speed for UA)
  double      acc_mps2;
  double      rpm;
  double      v1_mps;      // NEW: instantaneous speed at Gate A
  double      v2_mps;      // NEW: instantaneous speed at Gate B
  double      sigma_speed;
  double      sigma_acc;
  std::string timestamp;
};
static std::vector<Run> runs_cv;
static std::vector<Run> runs_pg;
static std::vector<Run> runs_ua;
static std::vector<Run> runs_ff;
static std::vector<Run> runs_incl;
static std::vector<Run> runs_tacho;
static uint32_t s_run_cv=0, s_run_pg=0, s_run_ua=0, s_run_ff=0, s_run_incl=0, s_run_ta=0;

static std::string human_time() {
  unsigned long ms = millis();
  unsigned long sec = ms / 1000;
  unsigned long min = sec / 60;
  unsigned long hr  = min / 60;
  sec %= 60; min %= 60;
  unsigned long ms_part = ms % 1000;
  char buf[32];
  snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu.%03lu", hr, min, sec, ms_part);
  return std::string(buf);
}

static double sample_stddev_speed(const std::vector<Run>& v) {
  if (v.size() < 2) return 0.0;
  double mean = 0.0; for (auto &r : v) mean += r.speed_mps; mean /= v.size();
  double ss = 0.0; for (auto &r : v) { double d = r.speed_mps - mean; ss += d*d; }
  return sqrt(ss / (v.size()-1));
}
static double sample_stddev_acc(const std::vector<Run>& v) {
  if (v.size() < 2) return 0.0;
  double mean = 0.0; for (auto &r : v) mean += r.acc_mps2; mean /= v.size();
  double ss = 0.0; for (auto &r : v) { double d = r.acc_mps2 - mean; ss += d*d; }
  return sqrt(ss / (v.size()-1));
}
static void stamp_sigma(std::vector<Run>& v, bool speed, bool acc) {
  double sS = speed ? sample_stddev_speed(v) : 0.0;
  double sA = acc   ? sample_stddev_acc  (v) : 0.0;
  for (auto &r : v) { r.sigma_speed = sS; r.sigma_acc = sA; }
}
static void push_limited(std::vector<Run>& v, const Run& r) {
  if (v.size() >= 50) v.erase(v.begin());
  v.push_back(r);
}

// ================= Persistence =================
static void load_settings() {
  Preferences prefs;
  if (prefs.begin("experiments", true)) {
    s_distance_mm   = prefs.getUShort("cv_d_mm", 500);
    s_flag_mm       = prefs.getUShort("pg_flag_mm", 50);
    s_obj_len_mm    = prefs.getUShort("ua_len_mm", 50);
    s_posA          = prefs.getUShort("ua_p0", 0);
    s_posB          = prefs.getUShort("ua_p1", 500);
    s_posC          = prefs.getUShort("ua_p2", 1000);
    s_ff_len_mm     = prefs.getUShort("ff_len_mm", 50);
    s_ff_drop_mm    = prefs.getUShort("ff_drop_mm", 500);
    s_in_len_mm     = prefs.getUShort("in_len_mm", 50);
    s_in_dist_mm    = prefs.getUShort("in_dist_mm", 500);
    s_in_angle_deg  = prefs.getUShort("in_ang_deg", 10);
    s_tacho_slots   = prefs.getUShort("tacho_slots", 1);
    prefs.end();
  }
}
static void save_settings() {
  Preferences prefs;
  if (prefs.begin("experiments", false)) {
    prefs.putUShort("cv_d_mm", s_distance_mm);
    prefs.putUShort("pg_flag_mm", s_flag_mm);
    prefs.putUShort("ua_len_mm", s_obj_len_mm);
    prefs.putUShort("ua_p0", s_posA);
    prefs.putUShort("ua_p1", s_posB);
    prefs.putUShort("ua_p2", s_posC);
    prefs.putUShort("ff_len_mm", s_ff_len_mm);
    prefs.putUShort("ff_drop_mm", s_ff_drop_mm);
    prefs.putUShort("in_len_mm", s_in_len_mm);
    prefs.putUShort("in_dist_mm", s_in_dist_mm);
    prefs.putUShort("in_ang_deg", s_in_angle_deg);
    prefs.putUShort("tacho_slots", s_tacho_slots);
    prefs.end();
  }
}
void experiments_set_distance_mm(uint16_t mm) { s_distance_mm = mm; save_settings(); }
uint16_t experiments_get_distance_mm()       { return s_distance_mm; }
void experiments_set_flag_mm(uint16_t mm)    { s_flag_mm = mm; save_settings(); }
uint16_t experiments_get_flag_mm()           { return s_flag_mm; }
// UA: L only
void experiments_set_ua_len_mm(uint16_t object_len_mm) { s_obj_len_mm = object_len_mm; save_settings(); }
uint16_t experiments_get_ua_len_mm()                    { return s_obj_len_mm; }
void experiments_set_ua_positions(uint16_t p0, uint16_t p1, uint16_t p2)
{ s_posA = p0; s_posB = p1; s_posC = p2; save_settings(); }
void experiments_get_ua_positions(uint16_t& p0, uint16_t& p1, uint16_t& p2)
{ p0 = s_posA; p1 = s_posB; p2 = s_posC; }
void experiments_set_freefall_params(uint16_t object_len_mm, uint16_t drop_height_mm)
{ s_ff_len_mm = object_len_mm; s_ff_drop_mm = drop_height_mm; save_settings(); }
void experiments_get_freefall_params(uint16_t& object_len_mm, uint16_t& drop_height_mm)
{ object_len_mm = s_ff_len_mm; drop_height_mm = s_ff_drop_mm; }

void experiments_set_incline_params(uint16_t object_len_mm, uint16_t gate_dist_mm, float angle_deg)
{
    s_in_len_mm = object_len_mm;
    s_in_dist_mm = gate_dist_mm;
    s_in_angle_deg = (uint16_t)angle_deg;  // round down
    save_settings();
}

void experiments_get_incline_params(uint16_t& object_len_mm, uint16_t& gate_dist_mm, float& angle_deg)
{
    object_len_mm = s_in_len_mm;
    gate_dist_mm = s_in_dist_mm;
    angle_deg = (float)s_in_angle_deg;  // for formula use
}

void experiments_set_tacho_params(uint16_t slots) { s_tacho_slots = slots ? slots : 1; save_settings(); }
uint16_t experiments_get_tacho_slots()            { return s_tacho_slots; }

// ================= Lifecycle =================
void experiments_init() {
  runs_cv.clear(); runs_pg.clear(); runs_ua.clear();
  runs_ff.clear(); runs_incl.clear(); runs_tacho.clear();
  s_run_cv = s_run_pg = s_run_ua = s_run_ff = s_run_incl = s_run_ta = 0;
  load_settings();
}
void experiments_clear_timestamps() { gate_engine_init(); }

// ================= CV =================
bool experiments_record_cv(double& speed_mps, double& time_ms, std::string& formula)
{
  uint64_t tA = gate_timestamp(GateID::GATE_A);
  uint64_t tB = gate_timestamp(GateID::GATE_B);
  if (tA == 0 || tB == 0 || tB <= tA) return false;
  time_ms   = (tB - tA) / 1000.0;
  speed_mps = (double)s_distance_mm / time_ms; // mm/ms == m/s
  char buf[192];
  snprintf(buf, sizeof(buf),
    "%s\n%s = %u mm\n%s = %d %s\n%s = %.3f %s",
    tr("Speed = Distance / Time"),
    tr("Distance"), (unsigned)s_distance_mm,
    tr("Time"), (int)time_ms, tr("ms"),
    tr("Speed"), speed_mps, tr("m/s"));
  formula.assign(buf);

  Run r = {"CV", ++s_run_cv, (double)s_distance_mm, time_ms, speed_mps, 0.0, 0.0,
           0.0, 0.0, 0.0, 0.0, human_time()};
  push_limited(runs_cv, r);
  stamp_sigma(runs_cv, true, false);
  return true;
}

// ================= Photogate =================
// [Updated: 2026-02-14] Changed to use Gate A block range only (single gate)
bool experiments_record_photogate(double& speed_mps, double& time_ms, std::string& formula)
{
  // Get block start and end times from Gate A only
  uint64_t t_start, t_end;
  if (!gate_get_last_block_range_us(GateID::GATE_A, t_start, t_end)) return false;
  
  time_ms = (t_end - t_start) / 1000.0;
  if (time_ms <= 0) return false;
  
  speed_mps = (double)s_flag_mm / time_ms; // mm/ms == m/s
  
  char buf[192];
  snprintf(buf, sizeof(buf),
    "%s\n%s = %u mm\n%s = %d %s\n%s = %.3f %s",
    tr("Speed = Distance / Time"),
    tr("Flag length"), (unsigned)s_flag_mm,
    tr("Block time"), (int)time_ms, tr("ms"),
    tr("Speed"), speed_mps, tr("m/s"));
  formula.assign(buf);

  Run r = {"Photogate", ++s_run_pg, (double)s_flag_mm, time_ms, speed_mps, 0.0, 0.0,
           0.0, 0.0, 0.0, 0.0, human_time()};
  push_limited(runs_pg, r);
  stamp_sigma(runs_pg, true, false);
  return true;
}

// ================= UA — two-gate only, D removed =================
bool experiments_record_ua(double& acc_mps2,
                           double& speed_mid_mps,
                           double& total_ms,
                           std::string& formula,
                           double* v1_out, double* v2_out)
{
  uint64_t a_start, a_end, b_start, b_end;
  if (!gate_get_last_block_range_us(GateID::GATE_A, a_start, a_end)) return false;
  if (!gate_get_last_block_range_us(GateID::GATE_B, b_start, b_end)) return false;

  const double tau1_ms = (a_end - a_start) / 1000.0;
  const double tau2_ms = (b_end - b_start) / 1000.0;
  if (tau1_ms <= 0 || tau2_ms <= 0) return false;

  uint64_t a_front, b_front;
  if (!gate_get_last_block_start_us(GateID::GATE_A, a_front)) return false;
  if (!gate_get_last_block_start_us(GateID::GATE_B, b_front)) return false;
  if (b_front <= a_front) return false;

  const double dtFront_ms = (b_front - a_front) / 1000.0;
  const double dtMid_ms   = dtFront_ms + (tau2_ms - tau1_ms) / 2.0;
  const double L_mm = (double)s_obj_len_mm;

  const double v1_mps = L_mm / tau1_ms;
  const double v2_mps = L_mm / tau2_ms;
  if (v1_out) *v1_out = v1_mps;
  if (v2_out) *v2_out = v2_mps;

  acc_mps2      = (v2_mps - v1_mps) / (dtMid_ms / 1000.0);
  speed_mid_mps = 0.5 * (v1_mps + v2_mps);
  total_ms      = dtFront_ms;

  char header[64]; snprintf(header, sizeof(header), tr("UA (two-gate): L=%u mm"), (unsigned)s_obj_len_mm);
  char v1line [64]; snprintf(v1line , sizeof(v1line ), tr("v1 = L / τA = %.3f"), v1_mps);
  char v2line [64]; snprintf(v2line , sizeof(v2line ), tr("v2 = L / τB = %.3f"), v2_mps);
  char aline  [64]; snprintf(aline  , sizeof(aline  ), tr("a = (v2 - v1) / Δt_mid = %.3f m/s²"), acc_mps2);

  char fbuf[384];
  snprintf(fbuf, sizeof(fbuf),
    "%s\n"
    "τA=%.3f %s, τB=%.3f %s, Δt_front=%.3f %s, Δt_mid=%.3f %s\n"
    "%s\n%s\n%s, v(mid)=%.3f %s",
    header,
    tau1_ms, tr("ms"), tau2_ms, tr("ms"),
    dtFront_ms, tr("ms"), dtMid_ms, tr("ms"),
    v1line, v2line, aline, speed_mid_mps, tr("m/s"));
  formula.assign(fbuf);

  Run r = {"UA", ++s_run_ua, (double)s_obj_len_mm, total_ms, speed_mid_mps, acc_mps2, 0.0,
           v1_mps, v2_mps, 0.0, 0.0, human_time()};
  push_limited(runs_ua, r);
  stamp_sigma(runs_ua, true, true);
  return true;
}

// ================= Free Fall =================

// [Updated: 2026-01-17 16:22:00 CET] Reason: Use explicit UTF-8 escape for “≈” to avoid encoding issues
bool experiments_record_freefall(double& v_mps, double& g_mps2, double& tau_ms, std::string& formula)
{
    // Get block start and end times from Gate A only
    uint64_t t_start, t_end;
    if (!gate_get_last_block_range_us(GateID::GATE_A, t_start, t_end)) return false;

    tau_ms = (t_end - t_start) / 1000.0;
    if (tau_ms <= 0) return false;

    const double L_mm = (double)s_ff_len_mm;
    v_mps = L_mm / tau_ms; // mm/ms == m/s

    const double h_mm = (double)s_ff_drop_mm;
    if (h_mm <= 0) return false;

    const double h_eff_m = (h_mm - L_mm / 2.0) / 1000.0;  // effective drop to gate midpoint
    if (h_eff_m <= 0) return false;                          // guard against invalid geometry
    g_mps2 = (v_mps * v_mps) / (2.0 * h_eff_m);

    char buf[320];
    // NOTE: "\xE2\x89\x88" is U+2248 (ALMOST EQUAL TO "≈") in UTF-8
    // NOTE: "\xCF\x84" is U+03C4 (Greek lowercase tau "τ") in UTF-8
    // NOTE: "\xC2\xB2" is U+00B2 (superscript two "²") in UTF-8
    snprintf(buf, sizeof(buf),
             "%s\nL=%u mm, h=%u mm, h_eff=%.1f mm\n\xCF\x84=%d %s\nv=%.3f %s, g=%.3f %s",
             tr("Free Fall: v = L / \xCF\x84, g \xE2\x89\x88 v\xC2\xB2 / (2 h_eff)"),
             (unsigned)s_ff_len_mm, (unsigned)s_ff_drop_mm,
             h_eff_m * 1000.0,
             (int)tau_ms, tr("ms"),
             v_mps, tr("m/s"),
             g_mps2, tr("m/s\xC2\xB2"));
    formula.assign(buf);

    Run r = {"FreeFall", ++s_run_ff, (double)s_ff_drop_mm, tau_ms, v_mps, g_mps2, 0.0,
             0.0, 0.0, 0.0, 0.0, human_time()};
    push_limited(runs_ff, r);
    stamp_sigma(runs_ff, true, true);
    return true;
}


// ================= Inclined Plane =================

// [Updated: 2026-01-17 16:22:00 CET] Reason: Use explicit UTF-8 escape for “≈” to avoid encoding issues
bool experiments_record_incline(double& a_mps2,
                                double& v1_mps,
                                double& v2_mps,
                                double& total_ms,
                                std::string& formula)
{
    // Use the same robust method as UA: blocking durations + front edges
    uint64_t a_start, a_end, b_start, b_end;
    if (!gate_get_last_block_range_us(GateID::GATE_A, a_start, a_end)) return false;
    if (!gate_get_last_block_range_us(GateID::GATE_B, b_start, b_end)) return false;

    const double tauA_ms = (a_end - a_start) / 1000.0;
    const double tauB_ms = (b_end - b_start) / 1000.0;
    if (tauA_ms <= 0 || tauB_ms <= 0) return false;

    uint64_t a_front_us, b_front_us;
    if (!gate_get_last_block_start_us(GateID::GATE_A, a_front_us)) return false;
    if (!gate_get_last_block_start_us(GateID::GATE_B, b_front_us)) return false;
    if (b_front_us <= a_front_us) return false;

    const double dtFront_ms = (b_front_us - a_front_us) / 1000.0;   // requested time between gates
    const double dtMid_ms   = dtFront_ms + (tauB_ms - tauA_ms) / 2.0;

    // Instantaneous speeds
    const double L_mm = (double)s_in_len_mm;
    v1_mps = L_mm / tauA_ms;    // mm/ms == m/s
    v2_mps = L_mm / tauB_ms;

    // Measured acceleration (from two speeds and Δt_mid)
    a_mps2  = (v2_mps - v1_mps) / (dtMid_ms / 1000.0);

    // Provide Δt_front as 'Time' for GUI/history
    total_ms = dtFront_ms;

    // Theoretical acceleration (frictionless): a ≈ g sin(θ)
    const double g = 9.81; // m/s²
    const double theta_rad = s_in_angle_deg * M_PI / 180.0;
    const double a_theory  = g * sin(theta_rad);

    // Build localized formula text
    char header[128];
    snprintf(header, sizeof(header), tr("Inclined Plane: L=%u mm, D=%u mm, %s=%u°"),
             (unsigned)s_in_len_mm, (unsigned)s_in_dist_mm,
             tr("Incline angle (deg):"), (unsigned)s_in_angle_deg);

    char v1line[64]; snprintf(v1line, sizeof(v1line), tr("v1 = L / τA = %.3f"), v1_mps);
    char v2line[64]; snprintf(v2line, sizeof(v2line), tr("v2 = L / τB = %.3f"), v2_mps);
    char aline [80]; snprintf(aline , sizeof(aline ), tr("a = (v2 - v1) / Δt_mid = %.3f m/s²"), a_mps2);

    char tline [96];
    // NOTE: "\xE2\x89\x88" is U+2248 (ALMOST EQUAL TO “≈”) in UTF-8
    snprintf(tline, sizeof(tline), tr("Theory: a \xE2\x89\x88 g·sin(θ) = %.3f m/s² (g=9.81)"), a_theory);

    char fbuf[512];
    snprintf(fbuf, sizeof(fbuf),
             "%s\n"
             "τA=%.3f %s, τB=%.3f %s, Δt_front=%.3f %s, Δt_mid=%.3f %s\n"
             "%s\n%s\n%s\n%s",
             header,
             tauA_ms, tr("ms"), tauB_ms, tr("ms"),
             dtFront_ms, tr("ms"), dtMid_ms, tr("ms"),
             v1line, v2line, aline, tline);
    formula.assign(fbuf);

    // Log run: store D in d_mm, Δt_front in time_ms, v2 as main speed; keep v1/v2
    Run r = {"Incline", ++s_run_incl,
             (double)s_in_dist_mm, dtFront_ms, v2_mps, a_mps2, 0.0,
             v1_mps, v2_mps, 0.0, 0.0, human_time()};
    push_limited(runs_incl, r);
    stamp_sigma(runs_incl, true, true);
    return true;
}


// ================= Tachometer =================
bool experiments_record_tacho(double& rpm, double& period_ms, std::string& formula)
{
  static uint64_t prevA = 0;
  uint64_t tA = gate_timestamp(GateID::GATE_A);
  if (tA == 0) return false;
  if (prevA == 0) { prevA = tA; return false; }
  uint64_t dt = tA - prevA; prevA = tA;

  period_ms = dt / 1000.0;
  if (period_ms <= 0) return false;

  uint16_t slots = s_tacho_slots ? s_tacho_slots : 1;
  rpm = (60.0 * 1000.0) / (period_ms * slots);

  char tbuf[256];
  snprintf(tbuf, sizeof(tbuf),
    "%s\n%s=%u, %s=%d %s",
    tr("Tachometer: RPM = 60,000 / (Period_ms × Slots)"),
    tr("Slots (per revolution):"), (unsigned)slots,
    tr("Time"), (int)period_ms, tr("ms"));
  formula.assign(tbuf);

  Run r = {"Tachometer", ++s_run_ta, (double)slots, period_ms, 0.0, 0.0, rpm,
           0.0, 0.0, 0.0, 0.0, human_time()};
  push_limited(runs_tacho, r);
  return true;
}

// ================= History =================
std::vector<std::string> experiments_get_last10(const char* mode)
{
  std::vector<std::string> out;
  const std::vector<Run>* src = nullptr;
  bool isUA=false, isFF=false, isIncl=false, isTa=false;
  if (strcmp(mode, "CV") == 0) src = &runs_cv;
  else if (strcmp(mode, "Photogate") == 0) src = &runs_pg;
  else if (strcmp(mode, "UA") == 0) { src = &runs_ua;  isUA = true; }
  else if (strcmp(mode, "FreeFall") == 0) { src = &runs_ff; isFF = true; }
  else if (strcmp(mode, "Incline") == 0) { src = &runs_incl; isIncl = true; }
  else if (strcmp(mode, "Tachometer") == 0) { src = &runs_tacho; isTa = true; }
  if (!src) return out;

  int start = (int)src->size() - 10;
  if (start < 0) start = 0;
  for (int i = start; i < (int)src->size(); ++i) {
    char buf[200];
    if (isUA) {
      snprintf(buf, sizeof(buf), "%u %s=%.3f %s %s=%.0f %s",
               src->at(i).run,
               tr("Acceleration"), src->at(i).acc_mps2, tr("m/s²"),
               tr("Time"),         src->at(i).time_ms,  tr("ms"));
    } else if (isIncl) {
      // Show v1, v2, Δt_front and a (requested)
      snprintf(buf, sizeof(buf), "%u v1=%.3f %s v2=%.3f %s %s=%.0f %s %s=%.3f %s",
               src->at(i).run,
               src->at(i).v1_mps, tr("m/s"),
               src->at(i).v2_mps, tr("m/s"),
               tr("Time"), src->at(i).time_ms, tr("ms"),
               tr("Acceleration"), src->at(i).acc_mps2, tr("m/s²"));
    } else if (isFF) {
      snprintf(buf, sizeof(buf), "%u v=%.3f %s g=%.3f %s τ=%.0f %s",
               src->at(i).run,
               src->at(i).speed_mps, tr("m/s"),
               src->at(i).acc_mps2,  tr("m/s²"),
               src->at(i).time_ms,   tr("ms"));
    } else if (isTa) {
      snprintf(buf, sizeof(buf), "%u %s=%.1f",
               src->at(i).run, tr("RPM"), src->at(i).rpm);
    } else {
      snprintf(buf, sizeof(buf), "%u %s=%.3f %s %s=%.0f %s",
               src->at(i).run,
               tr("Speed"), src->at(i).speed_mps, tr("m/s"),
               tr("Time"),  src->at(i).time_ms,   tr("ms"));
    }
    out.push_back(std::string(buf));
  }
  return out;
}

// Clear buffers per mode + RESET numbering counters
void experiments_clear_history(const char* mode)
{
  if (!mode) return;
  if (strcmp(mode, "CV") == 0) { runs_cv.clear(); s_run_cv = 0; }
  else if (strcmp(mode, "Photogate") == 0){ runs_pg.clear(); s_run_pg = 0; }
  else if (strcmp(mode, "UA") == 0)       { runs_ua.clear(); s_run_ua = 0; }
  else if (strcmp(mode, "FreeFall") == 0) { runs_ff.clear(); s_run_ff = 0; }
  else if (strcmp(mode, "Incline") == 0)  { runs_incl.clear(); s_run_incl = 0; }
  else if (strcmp(mode, "Tachometer") == 0){runs_tacho.clear(); s_run_ta = 0; }
}


// ================= CSV Export =================
// [Updated: 2026-01-17 19:00 CET] Reason: add Print& streaming exporter; keep Serial wrapper

// New universal emitter: writes CSV to any Print target (Serial, File, WiFiClient, ...)
void experiments_emit_csv(Print& out)
{
  out.println("mode,run,d_mm,time_ms,speed_mps,acc_mps2,rpm,v1_mps,v2_mps,sigma_speed,sigma_acc,timestamp");

  auto emit_line = [&](const Run& r) {
    // ESP32 Arduino core supports Print::printf (your profile shows core 3.3.5)
    out.printf("%s,%u,%.1f,%.3f,%.6f,%.6f,%.2f,%.6f,%.6f,%.6f,%.6f,%s\n",
               r.mode.c_str(), r.run, r.d_mm, r.time_ms, r.speed_mps, r.acc_mps2,
               r.rpm, r.v1_mps, r.v2_mps, r.sigma_speed, r.sigma_acc, r.timestamp.c_str());
  };

  for (const auto& r : runs_cv )  emit_line(r);
  for (const auto& r : runs_pg )  emit_line(r);
  for (const auto& r : runs_ua )  emit_line(r);
  for (const auto& r : runs_ff )  emit_line(r);
  for (const auto& r : runs_incl) emit_line(r);
  for (const auto& r : runs_tacho)emit_line(r);
}

// Backward-compatible wrapper: preserves today’s behavior for GUI “Export CSV” buttons
void experiments_export_csv()
{
  experiments_emit_csv(Serial);
}


