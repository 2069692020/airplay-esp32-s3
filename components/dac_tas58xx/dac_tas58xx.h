#pragma once

#include "dac.h"

/**
 * TAS58xx (TAS5825M) DAC driver ops — register with dac_register() before
 * calling dac_init().
 */
extern const dac_ops_t dac_tas58xx_ops;

/** Sub level-trim limits (dB), relative to the master volume. */
#define TAS58XX_SUB_OFFSET_MIN_DB (-15.0f)
#define TAS58XX_SUB_OFFSET_MAX_DB (15.0f)

/**
 * Set the sub (PBTL mono) volume offset in dB, relative to the master volume.
 * Positive raises the bass level, negative lowers it. Clamped to
 * [TAS58XX_SUB_OFFSET_MIN_DB, TAS58XX_SUB_OFFSET_MAX_DB]. Safe to call before
 * dac_init(); the value is applied on the next volume update.
 */
void dac_tas58xx_set_sub_offset_db(float offset_db);

/** Get the current sub volume offset in dB. */
float dac_tas58xx_get_sub_offset_db(void);

/** Crossover frequency limits for the PBTL sub low-pass (Hz). */
#define TAS58XX_XOVER_MIN_HZ 40.0f
#define TAS58XX_XOVER_MAX_HZ 400.0f

/**
 * Set the low-pass crossover frequency applied to the PBTL mono sub, so it
 * only reproduces bass rather than the full-range feed. Anything below
 * TAS58XX_XOVER_MIN_HZ disables the filter; higher values are clamped to
 * TAS58XX_XOVER_MAX_HZ. Moving the corner relayouts the per-way EQ bands, so
 * any stored gains are reset to flat.
 */
void dac_tas58xx_set_sub_crossover_hz(float hz);

/** Get the sub crossover frequency in Hz (0 when full range). */
float dac_tas58xx_get_sub_crossover_hz(void);

/** Role of the second amplifier on dual-DAC boards. */
typedef enum {
  /** Secondary drives a bridged (PBTL) mono subwoofer; primary stays stereo. */
  TAS58XX_DUAL_SUB = 0,
  /** Primary carries the left channel, secondary the right — one speaker
   *  per amplifier, both of its outputs driven from the same channel. */
  TAS58XX_DUAL_BIAMP = 1,
} tas58xx_dual_mode_t;

/**
 * Bi-amp is implemented but has not been validated on hardware, and selecting
 * it with PBTL sub wiring still attached shorts the two outputs together. Set
 * to 1 to expose it in the web UI and accept it over the API.
 */
#define TAS58XX_BIAMP_SUPPORTED 0

/**
 * Number of TAS58xx chips found on the I2C bus. Returns 0 before dac_init();
 * >1 means the board is a dual-DAC variant.
 */
int dac_tas58xx_get_device_count(void);

/** Get the configured second-amplifier role, including a pending change. */
tas58xx_dual_mode_t dac_tas58xx_get_dual_mode(void);

/** Get the role the chips were actually brought up in. */
tas58xx_dual_mode_t dac_tas58xx_get_active_dual_mode(void);

/**
 * Set the second-amplifier role. PBTL is a control-port setting that can only
 * be changed while the output stage is idle, so the new role is stored and
 * applied by the next dac_init() — the caller must restart the device.
 */
void dac_tas58xx_set_dual_mode(tas58xx_dual_mode_t mode);

/* ---------- Per-way EQ (2.1 and bi-amp) ----------
 *
 * Whenever a crossover splits the signal, each side gets its own 12-band
 * EQ whose centre frequencies are spread across that side's passband and
 * therefore move with the crossover frequency.
 */

/** Bands in each per-way EQ curve. */
#define TAS58XX_WAY_BANDS 12

/** The two sides of a crossover. */
typedef enum {
  TAS58XX_WAY_LOW = 0,  /**< sub / woofer — low-passed at the crossover */
  TAS58XX_WAY_HIGH = 1, /**< satellites / tweeter — high-passed */
} tas58xx_way_t;

/** True when a 2.1 sub crossover is engaged, so the per-way EQ is in use. */
bool dac_tas58xx_sub_eq_active(void);

/** Centre frequencies of a 2.1 way's EQ bands at the current crossover. */
void dac_tas58xx_sub_band_freqs(tas58xx_way_t way,
                                float out[TAS58XX_WAY_BANDS]);

/** 2.1 per-way EQ. Way LOW is the sub, way HIGH the satellites. */
esp_err_t dac_tas58xx_sub_eq_set_gains(tas58xx_way_t way,
                                       const float gains_db[TAS58XX_WAY_BANDS]);
void dac_tas58xx_sub_eq_get_gains(tas58xx_way_t way,
                                  float gains_db[TAS58XX_WAY_BANDS]);

/* ---------- Bi-amp (two-way active crossover per speaker) ---------- */

#define TAS58XX_BIAMP_XOVER_MIN_HZ 1500.0f
#define TAS58XX_BIAMP_XOVER_MAX_HZ 4000.0f

/** True when two chips are present and configured as bi-amped speakers. */
bool dac_tas58xx_biamp_active(void);

/**
 * Set the woofer/tweeter crossover, clamped to the bi-amp range. Moving it
 * relayouts the per-way EQ bands, so any stored gains are reset to flat.
 */
void dac_tas58xx_set_biamp_crossover_hz(float hz);
float dac_tas58xx_get_biamp_crossover_hz(void);

/** Select which amplifier output of each chip drives the woofer. */
void dac_tas58xx_set_biamp_swap(bool low_on_second_output);
bool dac_tas58xx_get_biamp_swap(void);

/** Centre frequencies of a bi-amp way's EQ bands at the current crossover. */
void dac_tas58xx_biamp_band_freqs(tas58xx_way_t way,
                                  float out[TAS58XX_WAY_BANDS]);

/** Per-speaker, per-way EQ. @p dev 0 = left speaker, 1 = right. */
esp_err_t dac_tas58xx_biamp_set_gains(int dev, tas58xx_way_t way,
                                      const float gains_db[TAS58XX_WAY_BANDS]);
void dac_tas58xx_biamp_get_gains(int dev, tas58xx_way_t way,
                                 float gains_db[TAS58XX_WAY_BANDS]);
