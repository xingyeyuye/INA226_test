#include <Arduino.h>

#include <ina226_battery_monitor.h>

#include <math.h>

static Ina226BatteryMonitor::Config battery_config = [] {
  Ina226BatteryMonitor::Config config{};
  config.i2c_address = 0x40;
  config.sda_pin = 32;
  config.scl_pin = 33;

  config.battery_capacity_mah = 3000.0f;
  config.shunt_resistor_ohm = 0.002f;
  config.max_current_amps = 6.0f;

  config.current_polarity = 1;
  config.current_deadzone_ma = 1.0f;

  config.nvs_namespace = "bat";
  config.nvs_key_state = "state";
  config.save_interval_ms = 10UL * 60UL * 1000UL;
  config.min_save_delta_mah = 1.0;

  config.startup_voltage_samples = 5;
  config.startup_voltage_sample_delay_ms = 50;

  config.full_charge_voltage_v = 12.5f;
  config.full_charge_current_ma = 50.0f;

  config.average = INA226_16_SAMPLES;
  return config;
}();

static Ina226BatteryMonitor battery_monitor(battery_config);

void setup()
{
  Serial.begin(115200);
  battery_monitor.set_logger(&Serial);

  Serial.println();
  Serial.println(__FILE__);
  Serial.print("INA226_LIB_VERSION: ");
  Serial.println(INA226_LIB_VERSION);

  Serial.println("Initializing INA226...");
  if (!battery_monitor.begin())
  {
    while (true)
    {
      Serial.println("Could not connect to INA226. Fix wiring.");
      delay(2000);
    }
  }

  Serial.println("INA226 Ready!");
  Serial.println("\nPOWER2 = busVoltage x current");
  Serial.println(" V\t mA \t mW \t mW \t %");
  Serial.println("BUS\tCURRENT\tPOWER\tPOWER2\tSoC");
  Serial.println("Commands: [R] reset SoC from voltage, [C] clear NVS + reset");
}

void loop()
{
  const uint32_t now_ms = millis();
  battery_monitor.update(now_ms, &Serial);

  const Ina226BatteryMonitor::Sample &sample = battery_monitor.sample();

  Serial.print(sample.bus_voltage_v, 3);
  Serial.print("\t");
  Serial.print(fabsf(sample.current_ma), 3);
  Serial.print("\t");
  Serial.print(sample.power_mw, 2);
  Serial.print("\t");
  Serial.print(sample.power2_mw, 2);
  Serial.print("\t");
  Serial.print(sample.soc_percent, 3);
  Serial.print(" %");
  Serial.print("\t");
  Serial.println();

  delay(1000);
}

