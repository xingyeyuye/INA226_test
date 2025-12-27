#include "ina226_battery_monitor.h"

#include <Preferences.h>

#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

static constexpr uint32_t k_battery_state_magic = 0x42415431;
static constexpr uint16_t k_battery_state_version = 1;

const Ina226BatteryMonitor::SocPoint Ina226BatteryMonitor::k_default_soc_table_[] = {
    {12.60f, 100.0f},
    {12.30f, 90.0f},
    {12.00f, 80.0f},
    {11.70f, 70.0f},
    {11.40f, 60.0f},
    {11.10f, 50.0f},
    {10.80f, 40.0f},
    {10.50f, 30.0f},
    {10.20f, 20.0f},
    {9.60f, 10.0f},
    {9.00f, 0.0f},
};

Ina226BatteryMonitor::Ina226BatteryMonitor(const Config &config)
    : config_(config),
      ina226_(config.i2c_address, config.wire != nullptr ? config.wire : &Wire),
      remaining_capacity_mah_(config.battery_capacity_mah),
      soc_percent_(100.0f)
{
  if (config_.wire == nullptr)
  {
    config_.wire = &Wire;
  }
}

void Ina226BatteryMonitor::set_logger(Print *logger)
{
  logger_ = logger;
}

bool Ina226BatteryMonitor::begin()
{
  if (config_.init_wire)
  {
    if (config_.sda_pin >= 0 && config_.scl_pin >= 0)
    {
      config_.wire->begin(config_.sda_pin, config_.scl_pin);
    }
    else
    {
      config_.wire->begin();
    }
  }

  if (!ina226_.begin())
  {
    return false;
  }

  ina226_.setMaxCurrentShunt(config_.max_current_amps, config_.shunt_resistor_ohm);
  ina226_.setAverage(config_.average);

  const uint32_t samples = config_.startup_voltage_samples > 0 ? config_.startup_voltage_samples : 1;
  float total_voltage = 0.0f;
  for (uint32_t i = 0; i < samples; i++)
  {
    total_voltage += ina226_.getBusVoltage();
    if (config_.startup_voltage_sample_delay_ms > 0)
    {
      delay(config_.startup_voltage_sample_delay_ms);
    }
  }

  const float startup_voltage_v = total_voltage / static_cast<float>(samples);
  soc_percent_ = get_soc_from_voltage(startup_voltage_v);
  remaining_capacity_mah_ = (static_cast<double>(soc_percent_) / 100.0) * config_.battery_capacity_mah;

  double saved_remaining_capacity_mah = 0.0;
  if (load_remaining_capacity_from_nvs(saved_remaining_capacity_mah))
  {
    remaining_capacity_mah_ = saved_remaining_capacity_mah;
    if (remaining_capacity_mah_ < 0.0)
      remaining_capacity_mah_ = 0.0;
    if (remaining_capacity_mah_ > config_.battery_capacity_mah)
      remaining_capacity_mah_ = config_.battery_capacity_mah;

    soc_percent_ = static_cast<float>((remaining_capacity_mah_ / config_.battery_capacity_mah) * 100.0);
    logf("NVS loaded: remaining=%.2f mAh (SoC %.3f%%)\n", remaining_capacity_mah_, soc_percent_);
  }
  else
  {
    if (is_nvs_enabled())
    {
      logf("NVS not found/invalid, using OCV estimate and seeding NVS...\n");
      if (save_remaining_capacity_to_nvs(remaining_capacity_mah_))
      {
        logf("NVS seeded: remaining=%.2f mAh (SoC %.3f%%)\n", remaining_capacity_mah_, soc_percent_);
      }
      else
      {
        logf("NVS seed failed\n");
      }
    }
  }

  sample_.bus_voltage_v = startup_voltage_v;
  sample_.remaining_capacity_mah = remaining_capacity_mah_;
  sample_.soc_percent = soc_percent_;

  last_time_ms_ = millis();
  last_nvs_save_ms_ = last_time_ms_;
  last_saved_remaining_capacity_mah_ = remaining_capacity_mah_;
  return true;
}

void Ina226BatteryMonitor::update(Stream *serial)
{
  update(millis(), serial);
}

void Ina226BatteryMonitor::update(uint32_t now_ms, Stream *serial)
{
  sample_.bus_voltage_v = ina226_.getBusVoltage();
  sample_.shunt_voltage_mv = ina226_.getShuntVoltage_mV();
  sample_.current_ma = static_cast<float>(config_.current_polarity) * ina226_.getCurrent_mA();
  sample_.power_mw = ina226_.getPower_mW();

  const float abs_current_ma = fabsf(sample_.current_ma);
  sample_.power2_mw = sample_.bus_voltage_v * abs_current_ma;
  const float effective_current_ma = (abs_current_ma < config_.current_deadzone_ma) ? 0.0f : sample_.current_ma;

  if (serial != nullptr && serial->available() > 0)
  {
    const char cmd = static_cast<char>(serial->read());
    if (cmd == 'c' || cmd == 'C')
    {
      clear_nvs_state();
      reset_state_from_voltage(sample_.bus_voltage_v);
      maybe_save_to_nvs(now_ms, true);
    }
    else if (cmd == 'r' || cmd == 'R')
    {
      reset_state_from_voltage(sample_.bus_voltage_v);
      maybe_save_to_nvs(now_ms, true);
    }
  }

  const uint32_t elapsed_ms = now_ms - last_time_ms_;
  if (elapsed_ms > 0)
  {
    const double hours_passed = static_cast<double>(elapsed_ms) / 3600000.0;
    const double mah_delta = static_cast<double>(effective_current_ma) * hours_passed;
    remaining_capacity_mah_ -= mah_delta;

    if (remaining_capacity_mah_ < 0.0)
      remaining_capacity_mah_ = 0.0;
    if (remaining_capacity_mah_ > config_.battery_capacity_mah)
      remaining_capacity_mah_ = config_.battery_capacity_mah;

    soc_percent_ = static_cast<float>((remaining_capacity_mah_ / config_.battery_capacity_mah) * 100.0);
    last_time_ms_ = now_ms;
  }

  if (sample_.bus_voltage_v > config_.full_charge_voltage_v && abs_current_ma < config_.full_charge_current_ma)
  {
    remaining_capacity_mah_ = config_.battery_capacity_mah;
    soc_percent_ = 100.0f;
    logf("Battery Charged. SoC reset to 100%%\n");
    maybe_save_to_nvs(now_ms, true);
  }

  maybe_save_to_nvs(now_ms, false);

  sample_.remaining_capacity_mah = remaining_capacity_mah_;
  sample_.soc_percent = soc_percent_;
}

const Ina226BatteryMonitor::Sample &Ina226BatteryMonitor::sample() const
{
  return sample_;
}

void Ina226BatteryMonitor::reset_state_from_voltage(float voltage_v)
{
  soc_percent_ = get_soc_from_voltage(voltage_v);
  remaining_capacity_mah_ = (static_cast<double>(soc_percent_) / 100.0) * config_.battery_capacity_mah;
}

void Ina226BatteryMonitor::clear_nvs_state()
{
  if (!is_nvs_enabled())
  {
    return;
  }

  Preferences prefs;
  if (!prefs.begin(config_.nvs_namespace, false))
  {
    logf("NVS: Failed to open namespace for clear\n");
    return;
  }

  prefs.clear();
  prefs.end();
  logf("NVS: Cleared battery state\n");
}

uint32_t Ina226BatteryMonitor::calc_crc32_le(const uint8_t *data, size_t length)
{
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < length; i++)
  {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++)
    {
      const uint32_t mask = -(crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return ~crc;
}

bool Ina226BatteryMonitor::is_nvs_enabled() const
{
  return config_.nvs_namespace != nullptr && config_.nvs_namespace[0] != '\0' &&
         config_.nvs_key_state != nullptr && config_.nvs_key_state[0] != '\0';
}

bool Ina226BatteryMonitor::load_remaining_capacity_from_nvs(double &out_remaining_capacity_mah) const
{
  if (!is_nvs_enabled())
  {
    return false;
  }

  Preferences prefs;
  if (!prefs.begin(config_.nvs_namespace, true))
  {
    logf("NVS: Failed to open namespace\n");
    return false;
  }

  PersistedBatteryState state{};
  const size_t expected_size = sizeof(state);
  const size_t stored_size = prefs.getBytesLength(config_.nvs_key_state);
  if (stored_size != expected_size)
  {
    logf("NVS: Size mismatch (expected=%u, stored=%u)\n",
         static_cast<unsigned int>(expected_size),
         static_cast<unsigned int>(stored_size));
    prefs.end();
    return false;
  }

  const size_t read_size = prefs.getBytes(config_.nvs_key_state, &state, expected_size);
  prefs.end();
  if (read_size != expected_size)
  {
    logf("NVS: Read size mismatch (expected=%u, read=%u)\n",
         static_cast<unsigned int>(expected_size),
         static_cast<unsigned int>(read_size));
    return false;
  }

  if (state.magic != k_battery_state_magic || state.version != k_battery_state_version)
  {
    logf("NVS: Invalid Magic/Version (magic=0x%08X, ver=%u)\n",
         static_cast<unsigned int>(state.magic),
         static_cast<unsigned int>(state.version));
    return false;
  }

  const uint32_t expected_capacity = static_cast<uint32_t>(config_.battery_capacity_mah + 0.5f);
  if (state.capacity_mah_x1 != expected_capacity)
  {
    logf("NVS: Capacity mismatch (expected=%u, stored=%u)\n",
         static_cast<unsigned int>(expected_capacity),
         static_cast<unsigned int>(state.capacity_mah_x1));
    return false;
  }

  const uint32_t expected_crc =
      calc_crc32_le(reinterpret_cast<const uint8_t *>(&state), offsetof(PersistedBatteryState, crc32));
  if (state.crc32 != expected_crc)
  {
    logf("NVS: CRC mismatch (expected=0x%08X, stored=0x%08X)\n",
         static_cast<unsigned int>(expected_crc),
         static_cast<unsigned int>(state.crc32));
    return false;
  }

  out_remaining_capacity_mah = static_cast<double>(state.remaining_mah_x100) / 100.0;
  return true;
}

bool Ina226BatteryMonitor::save_remaining_capacity_to_nvs(double remaining_capacity_mah) const
{
  if (!is_nvs_enabled())
  {
    return false;
  }

  if (remaining_capacity_mah < 0.0)
    remaining_capacity_mah = 0.0;
  if (remaining_capacity_mah > config_.battery_capacity_mah)
    remaining_capacity_mah = config_.battery_capacity_mah;

  PersistedBatteryState state{};
  state.magic = k_battery_state_magic;
  state.version = k_battery_state_version;
  state.reserved = 0;
  state.capacity_mah_x1 = static_cast<uint32_t>(config_.battery_capacity_mah + 0.5f);
  state.remaining_mah_x100 = static_cast<uint32_t>(remaining_capacity_mah * 100.0 + 0.5);
  state.crc32 = calc_crc32_le(reinterpret_cast<const uint8_t *>(&state), offsetof(PersistedBatteryState, crc32));

  Preferences prefs;
  if (!prefs.begin(config_.nvs_namespace, false))
  {
    logf("NVS: Failed to open namespace\n");
    return false;
  }

  const size_t written_size = prefs.putBytes(config_.nvs_key_state, &state, sizeof(state));
  prefs.end();
  return written_size == sizeof(state);
}

float Ina226BatteryMonitor::get_soc_from_voltage(float voltage_v) const
{
  const SocPoint *table = config_.soc_table;
  size_t table_len = config_.soc_table_len;
  if (table == nullptr || table_len < 2)
  {
    table = k_default_soc_table_;
    table_len = sizeof(k_default_soc_table_) / sizeof(k_default_soc_table_[0]);
  }

  if (voltage_v >= table[0].voltage_v)
    return table[0].soc_percent;
  if (voltage_v <= table[table_len - 1].voltage_v)
    return table[table_len - 1].soc_percent;

  for (size_t i = 0; i + 1 < table_len; i++)
  {
    const float high_v = table[i].voltage_v;
    const float low_v = table[i + 1].voltage_v;
    if (voltage_v <= high_v && voltage_v > low_v)
    {
      const float high_p = table[i].soc_percent;
      const float low_p = table[i + 1].soc_percent;
      const float percentage = low_p + (voltage_v - low_v) * (high_p - low_p) / (high_v - low_v);
      return percentage;
    }
  }

  return table[table_len - 1].soc_percent;
}

void Ina226BatteryMonitor::maybe_save_to_nvs(uint32_t now_ms, bool force)
{
  if (!is_nvs_enabled())
  {
    return;
  }

  if (!force && (now_ms - last_nvs_save_ms_) < config_.save_interval_ms)
  {
    return;
  }

  if (!force && !isnan(last_saved_remaining_capacity_mah_) &&
      fabs(remaining_capacity_mah_ - last_saved_remaining_capacity_mah_) < config_.min_save_delta_mah)
  {
    last_nvs_save_ms_ = now_ms;
    return;
  }

  if (save_remaining_capacity_to_nvs(remaining_capacity_mah_))
  {
    last_saved_remaining_capacity_mah_ = remaining_capacity_mah_;
    last_nvs_save_ms_ = now_ms;
    logf("NVS saved: remaining=%.2f mAh (SoC %.1f%%)\n", remaining_capacity_mah_, soc_percent_);
  }
  else
  {
    logf("NVS save failed\n");
    last_nvs_save_ms_ = now_ms;
  }
}

void Ina226BatteryMonitor::logf(const char *format, ...) const
{
  if (logger_ == nullptr)
  {
    return;
  }

  char buffer[128];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  logger_->print(buffer);
}
