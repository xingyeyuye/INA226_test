#pragma once

#include <Arduino.h>
#include <INA226.h>

#include <math.h>

class Ina226BatteryMonitor
{
public:
  struct SocPoint
  {
    float voltage_v;
    float soc_percent;
  };

  struct Config
  {
    uint8_t i2c_address = 0x40;
    TwoWire *wire = &Wire;
    bool init_wire = true;
    int sda_pin = -1;
    int scl_pin = -1;

    float battery_capacity_mah = 3000.0f;
    float shunt_resistor_ohm = 0.02f;
    float max_current_amps = 4.0f;
    int current_polarity = 1;
    float current_deadzone_ma = 1.0f;
    uint8_t average = INA226_16_SAMPLES;

    const SocPoint *soc_table = nullptr;
    size_t soc_table_len = 0;

    const char *nvs_namespace = "bat";
    const char *nvs_key_state = "state";
    uint32_t save_interval_ms = 10UL * 60UL * 1000UL;
    double min_save_delta_mah = 1.0;

    uint32_t startup_voltage_samples = 5;
    uint32_t startup_voltage_sample_delay_ms = 50;

    float full_charge_voltage_v = 12.5f;
    float full_charge_current_ma = 50.0f;
  };

  struct Sample
  {
    float bus_voltage_v = NAN;
    float shunt_voltage_mv = NAN;
    float current_ma = NAN;
    float power_mw = NAN;
    float power2_mw = NAN;
    double remaining_capacity_mah = NAN;
    float soc_percent = NAN;
  };

  explicit Ina226BatteryMonitor(const Config &config);

  void set_logger(Print *logger);

  bool begin();
  void update(uint32_t now_ms, Stream *serial = nullptr);
  void update(Stream *serial = nullptr);

  const Sample &sample() const;

  void reset_state_from_voltage(float voltage_v);
  void clear_nvs_state();

private:
  struct __attribute__((packed)) PersistedBatteryState
  {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t capacity_mah_x1;
    uint32_t remaining_mah_x100;
    uint32_t crc32;
  };

  static uint32_t calc_crc32_le(const uint8_t *data, size_t length);
  bool is_nvs_enabled() const;
  bool load_remaining_capacity_from_nvs(double &out_remaining_capacity_mah) const;
  bool save_remaining_capacity_to_nvs(double remaining_capacity_mah) const;
  float get_soc_from_voltage(float voltage_v) const;
  void maybe_save_to_nvs(uint32_t now_ms, bool force);

  void logf(const char *format, ...) const;

  static const SocPoint k_default_soc_table_[];

  Config config_{};
  Print *logger_ = nullptr;

  INA226 ina226_;

  Sample sample_{};
  double remaining_capacity_mah_ = NAN;
  float soc_percent_ = NAN;

  uint32_t last_time_ms_ = 0;
  uint32_t last_nvs_save_ms_ = 0;
  double last_saved_remaining_capacity_mah_ = NAN;
};

