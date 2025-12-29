#include "ina226_battery_monitor.h" // 包含INA226电池监视器的头文件

#include <Preferences.h> // 包含Preferences库，用于NVS存储

#include <math.h> // 包含数学库
#include <stdarg.h> // 包含可变参数处理库
#include <stddef.h> // 包含标准定义库
#include <stdio.h> // 包含标准输入输出库

static constexpr uint32_t k_battery_state_magic = 0x42415431; // 定义电池状态魔数，用于校验NVS数据 ('BAT1')
static constexpr uint16_t k_battery_state_version = 1; // 定义电池状态版本号
 
// 默认的SOC（荷电状态）查表，电压对应百分比
const Ina226BatteryMonitor::SocPoint Ina226BatteryMonitor::k_default_soc_table_[] = {
    {12.60f, 100.0f}, // 12.60V 对应 100%
    {12.30f, 90.0f},  // 12.30V 对应 90%
    {12.00f, 80.0f},  // 12.00V 对应 80%
    {11.70f, 70.0f},  // 11.70V 对应 70%
    {11.40f, 60.0f},  // 11.40V 对应 60%
    {11.10f, 50.0f},  // 11.10V 对应 50%
    {10.80f, 40.0f},  // 10.80V 对应 40%
    {10.50f, 30.0f},  // 10.50V 对应 30%
    {10.20f, 20.0f},  // 10.20V 对应 20%
    {9.60f, 10.0f},   // 9.60V 对应 10%
    {9.00f, 0.0f},    // 9.00V 对应 0%
};

// 构造函数，初始化配置和成员变量
Ina226BatteryMonitor::Ina226BatteryMonitor(const Config &config)
    : config_(config), // 初始化配置结构体
      ina226_(config.i2c_address, config.wire != nullptr ? config.wire : &Wire), // 初始化INA226对象，设置I2C地址和Wire对象
      remaining_capacity_mah_(config.battery_capacity_mah), // 初始化剩余容量为电池总容量
      soc_percent_(100.0f) // 初始化SOC为100%
{
  if (config_.wire == nullptr) // 如果配置中的Wire指针为空
  {
    config_.wire = &Wire; // 默认使用Wire
  }
}

void Ina226BatteryMonitor::set_logger(Print *logger)
{
  logger_ = logger; // 保存日志对象指针
}

bool Ina226BatteryMonitor::begin()
{
  if (config_.init_wire) // 如果配置要求初始化Wire
  {
    if (config_.sda_pin >= 0 && config_.scl_pin >= 0) // 如果指定了SDA和SCL引脚
    {
      config_.wire->begin(config_.sda_pin, config_.scl_pin); // 使用指定引脚初始化Wire
    }
    else
    {
      config_.wire->begin(); // 使用默认引脚初始化Wire
    }
  }

  if (!ina226_.begin()) // 初始化INA226传感器
  {
    return false; // 如果失败返回false
  }

  ina226_.setMaxCurrentShunt(config_.max_current_amps, config_.shunt_resistor_ohm); // 设置最大电流和分流电阻值
  ina226_.setAverage(config_.average); // 设置平均采样次数

  const uint32_t samples = config_.startup_voltage_samples > 0 ? config_.startup_voltage_samples : 1; // 确定启动电压采样次数
  float total_voltage = 0.0f; // 总电压累加变量
  for (uint32_t i = 0; i < samples; i++) // 循环采样
  {
    total_voltage += ina226_.getBusVoltage(); // 读取总线电压并累加
    if (config_.startup_voltage_sample_delay_ms > 0) // 如果配置了采样延迟
    {
      delay(config_.startup_voltage_sample_delay_ms); // 延时等待
    }
  }

  const float startup_voltage_v = total_voltage / static_cast<float>(samples); // 计算平均启动电压
  soc_percent_ = get_soc_from_voltage(startup_voltage_v); // 根据电压估算初始SOC
  remaining_capacity_mah_ = (static_cast<double>(soc_percent_) / 100.0) * config_.battery_capacity_mah; // 根据SOC计算剩余容量

  double saved_remaining_capacity_mah = 0.0; // 用于存储从NVS读取的剩余容量
  if (load_remaining_capacity_from_nvs(saved_remaining_capacity_mah)) // 尝试从NVS加载剩余容量
  {
    remaining_capacity_mah_ = saved_remaining_capacity_mah; // 如果成功，更新剩余容量
    if (remaining_capacity_mah_ < 0.0) // 边界检查：小于0
      remaining_capacity_mah_ = 0.0; // 修正为0
    if (remaining_capacity_mah_ > config_.battery_capacity_mah) // 边界检查：大于总容量
      remaining_capacity_mah_ = config_.battery_capacity_mah; // 修正为总容量

    soc_percent_ = static_cast<float>((remaining_capacity_mah_ / config_.battery_capacity_mah) * 100.0); // 重新计算SOC百分比
    logf("NVS loaded: remaining=%.2f mAh (SoC %.3f%%)\n", remaining_capacity_mah_, soc_percent_); // 打印日志：NVS加载成功
  }
  else
  {
    if (is_nvs_enabled()) // 如果NVS已启用但加载失败
    {
      logf("NVS not found/invalid, using OCV estimate and seeding NVS...\n"); // 打印日志：NVS未找到或无效，使用OCV估算并初始化NVS
      if (save_remaining_capacity_to_nvs(remaining_capacity_mah_)) // 尝试将当前估算的容量写入NVS
      {
        logf("NVS seeded: remaining=%.2f mAh (SoC %.3f%%)\n", remaining_capacity_mah_, soc_percent_); // 打印日志：NVS初始化成功
      }
      else
      {
        logf("NVS seed failed\n"); // 打印日志：NVS初始化失败
      }
    }
  }

  sample_.bus_voltage_v = startup_voltage_v; // 更新样本数据：总线电压
  sample_.remaining_capacity_mah = remaining_capacity_mah_; // 更新样本数据：剩余容量
  sample_.soc_percent = soc_percent_; // 更新样本数据：SOC

  last_time_ms_ = millis(); // 记录当前时间
  last_nvs_save_ms_ = last_time_ms_; // 初始化上次NVS保存时间
  last_saved_remaining_capacity_mah_ = remaining_capacity_mah_; // 初始化上次保存的容量
  return true; // 初始化成功
}

void Ina226BatteryMonitor::update(Stream *serial)
{
  update(millis(), serial); // 调用带时间戳的更新函数
}

void Ina226BatteryMonitor::update(uint32_t now_ms, Stream *serial)
{
  sample_.bus_voltage_v = ina226_.getBusVoltage(); // 读取总线电压
  sample_.shunt_voltage_mv = ina226_.getShuntVoltage_mV(); // 读取分流电压
  sample_.current_ma = static_cast<float>(config_.current_polarity) * ina226_.getCurrent_mA(); // 读取电流并应用极性
  sample_.power_mw = ina226_.getPower_mW(); // 读取功率

  const float abs_current_ma = fabsf(sample_.current_ma); // 计算电流绝对值
  sample_.power2_mw = sample_.bus_voltage_v * abs_current_ma; // 计算功率（电压*电流绝对值）
  const float effective_current_ma = (abs_current_ma < config_.current_deadzone_ma) ? 0.0f : sample_.current_ma; // 应用电流死区，小于死区视为0

  if (serial != nullptr && serial->available() > 0) // 如果串口可用且有数据
  {
    const char cmd = static_cast<char>(serial->read()); // 读取命令字符
    if (cmd == 'c' || cmd == 'C') // 如果是清除命令 'c'
    {
      clear_nvs_state(); // 清除NVS状态
      reset_state_from_voltage(sample_.bus_voltage_v);
      maybe_save_to_nvs(now_ms, true); // 强制保存到NVS
    }
    else if (cmd == 'r' || cmd == 'R') // 如果是重置命令 'r'
    {
      reset_state_from_voltage(sample_.bus_voltage_v); 
      maybe_save_to_nvs(now_ms, true); // 强制保存到NVS
    }
  }

  const uint32_t elapsed_ms = now_ms - last_time_ms_; // 计算距离上次更新的时间差
  if (elapsed_ms > 0) // 如果有时间流逝
  {
    const double hours_passed = static_cast<double>(elapsed_ms) / 3600000.0; // 将毫秒转换为小时
    const double mah_delta = static_cast<double>(effective_current_ma) * hours_passed; // 计算消耗/充电的mAh（电流积分）
    remaining_capacity_mah_ -= mah_delta; // 更新剩余容量（减去变化量，注意电流符号）

    if (remaining_capacity_mah_ < 0.0) // 边界检查：小于0
      remaining_capacity_mah_ = 0.0; // 修正为0
    if (remaining_capacity_mah_ > config_.battery_capacity_mah) // 边界检查：大于总容量
      remaining_capacity_mah_ = config_.battery_capacity_mah; // 修正为总容量

    soc_percent_ = static_cast<float>((remaining_capacity_mah_ / config_.battery_capacity_mah) * 100.0); // 重新计算SOC
    last_time_ms_ = now_ms; // 更新上次时间
  }

  if (sample_.bus_voltage_v > config_.full_charge_voltage_v && abs_current_ma < config_.full_charge_current_ma) // 充满电判断：电压高于满充电压且电流小于截止电流
  {
    remaining_capacity_mah_ = config_.battery_capacity_mah; // 设置为满容量
    soc_percent_ = 100.0f; // SoC设为100%
    logf("Battery Charged. SoC reset to 100%%\n"); // 打印日志：电池已充满
    maybe_save_to_nvs(now_ms, true); // 强制保存状态
  }

  maybe_save_to_nvs(now_ms, false); // 尝试保存到NVS（非强制）

  sample_.remaining_capacity_mah = remaining_capacity_mah_; // 更新样本数据：剩余容量
  sample_.soc_percent = soc_percent_; // 更新样本数据：SoC
}

const Ina226BatteryMonitor::Sample &Ina226BatteryMonitor::sample() const
{
  return sample_; // 返回样本成员变量
}

void Ina226BatteryMonitor::reset_state_from_voltage(float voltage_v)
{
  soc_percent_ = get_soc_from_voltage(voltage_v); // 根据电压查表获取SOC
  remaining_capacity_mah_ = (static_cast<double>(soc_percent_) / 100.0) * config_.battery_capacity_mah; // 根据SOC计算容量
}

void Ina226BatteryMonitor::clear_nvs_state()
{
  if (!is_nvs_enabled()) // 如果NVS未启用
  {
    return; // 直接返回
  }

  Preferences prefs; // 创建Preferences对象
  if (!prefs.begin(config_.nvs_namespace, false)) // 打开NVS命名空间（读写模式）
  {
    logf("NVS: Failed to open namespace for clear\n"); // 打印日志：打开失败
    return; // 返回
  }

  prefs.clear(); // 清除该命名空间下的所有键值
  prefs.end(); // 关闭NVS
  logf("NVS: Cleared battery state\n"); // 打印日志：清除完成
}

uint32_t Ina226BatteryMonitor::calc_crc32_le(const uint8_t *data, size_t length) 
{
  uint32_t crc = 0xFFFFFFFFu; // 初始化CRC值为全1
  for (size_t i = 0; i < length; i++) // 遍历每一个字节的数据
  {
    crc ^= data[i]; // 将数据字节与CRC低位异或
    for (int bit = 0; bit < 8; bit++) // 对每一位进行处理
    {
      const uint32_t mask = -(crc & 1u); // 如果最低位为1，则生成掩码
      crc = (crc >> 1) ^ (0xEDB88320u & mask); // 右移并根据掩码进行多项式异或
    }
  }
  return ~crc; // 返回CRC取反后的结果
}

bool Ina226BatteryMonitor::is_nvs_enabled() const
{
  return config_.nvs_namespace != nullptr && config_.nvs_namespace[0] != '\0' && // 检查命名空间是否有效
         config_.nvs_key_state != nullptr && config_.nvs_key_state[0] != '\0'; // 检查键名是否有效
}

bool Ina226BatteryMonitor::load_remaining_capacity_from_nvs(double &out_remaining_capacity_mah) const
{
  if (!is_nvs_enabled()) // 如果NVS未启用
  {
    return false; // 返回失败
  }

  Preferences prefs; // 创建Preferences对象
  if (!prefs.begin(config_.nvs_namespace, true)) // 以只读模式打开NVS命名空间
  {
    logf("NVS: Failed to open namespace\n"); // 打印日志：打开命名空间失败
    return false; // 返回失败
  }

  PersistedBatteryState state{}; // 定义电池状态结构体
  const size_t expected_size = sizeof(state); // 获取预期的大小
  const size_t stored_size = prefs.getBytesLength(config_.nvs_key_state); // 获取存储的数据大小
  if (stored_size != expected_size) // 如果存储大小不匹配
  {
    logf("NVS: Size mismatch (expected=%u, stored=%u)\n", // 打印日志：大小不匹配
         static_cast<unsigned int>(expected_size), // 预期大小
         static_cast<unsigned int>(stored_size)); // 实际存储大小
    prefs.end(); // 关闭Preferences
    return false; // 返回失败
  }

  const size_t read_size = prefs.getBytes(config_.nvs_key_state, &state, expected_size); // 读取数据到结构体
  prefs.end(); // 关闭Preferences
  if (read_size != expected_size) // 如果读取的大小不匹配
  {
    logf("NVS: Read size mismatch (expected=%u, read=%u)\n", // 打印日志：读取大小不匹配
         static_cast<unsigned int>(expected_size), // 预期大小
         static_cast<unsigned int>(read_size)); // 实际读取大小
    return false; // 返回失败
  }

  if (state.magic != k_battery_state_magic || state.version != k_battery_state_version) // 校验Magic数和版本号
  {
    logf("NVS: Invalid Magic/Version (magic=0x%08X, ver=%u)\n", // 打印日志：无效的Magic或版本
         static_cast<unsigned int>(state.magic), // 读取的Magic
         static_cast<unsigned int>(state.version)); // 读取的版本
    return false; // 返回失败
  }

  const uint32_t expected_capacity = static_cast<uint32_t>(config_.battery_capacity_mah + 0.5f); // 计算预期的电池容量（四舍五入）
  if (state.capacity_mah_x1 != expected_capacity) // 如果存储的容量与配置不符
  {
    logf("NVS: Capacity mismatch (expected=%u, stored=%u)\n", // 打印日志：容量不匹配
         static_cast<unsigned int>(expected_capacity), // 预期容量
         static_cast<unsigned int>(state.capacity_mah_x1)); // 存储容量
    return false; // 返回失败
  }

  const uint32_t expected_crc = // 计算校验和
      calc_crc32_le(reinterpret_cast<const uint8_t *>(&state), offsetof(PersistedBatteryState, crc32)); // 计算除了CRC字段之外的数据的CRC32
  if (state.crc32 != expected_crc) // 如果校验和不匹配
  {
    logf("NVS: CRC mismatch (expected=0x%08X, stored=0x%08X)\n", // 打印日志：CRC不匹配
         static_cast<unsigned int>(expected_crc), // 预期CRC
         static_cast<unsigned int>(state.crc32)); // 存储CRC
    return false; // 返回失败
  }

  out_remaining_capacity_mah = static_cast<double>(state.remaining_mah_x100) / 100.0; // 将存储的容量（放大100倍）转换为实际值
  return true; // 返回成功
}

bool Ina226BatteryMonitor::save_remaining_capacity_to_nvs(double remaining_capacity_mah) const
{
  if (!is_nvs_enabled()) // 如果NVS未启用
  {
    return false; // 返回失败
  }

  if (remaining_capacity_mah < 0.0) // 如果剩余容量小于0
    remaining_capacity_mah = 0.0; // 限制为0
  if (remaining_capacity_mah > config_.battery_capacity_mah) // 如果剩余容量大于总容量
    remaining_capacity_mah = config_.battery_capacity_mah; // 限制为总容量

  PersistedBatteryState state{}; // 初始化持久化状态结构体
  state.magic = k_battery_state_magic; // 设置Magic数
  state.version = k_battery_state_version; // 设置版本号
  state.reserved = 0; // 保留字段置0
  state.capacity_mah_x1 = static_cast<uint32_t>(config_.battery_capacity_mah + 0.5f); // 设置电池容量
  state.remaining_mah_x100 = static_cast<uint32_t>(remaining_capacity_mah * 100.0 + 0.5); // 设置剩余容量（放大100倍保存）
  state.crc32 = calc_crc32_le(reinterpret_cast<const uint8_t *>(&state), offsetof(PersistedBatteryState, crc32)); // 计算CRC校验和

  Preferences prefs; // 创建Preferences对象
  if (!prefs.begin(config_.nvs_namespace, false)) // 以读写模式打开NVS命名空间
  {
    logf("NVS: Failed to open namespace\n"); // 打印日志：打开失败
    return false; // 返回失败
  }

  const size_t written_size = prefs.putBytes(config_.nvs_key_state, &state, sizeof(state)); // 写入数据
  prefs.end(); // 关闭Preferences
  return written_size == sizeof(state); // 返回写入是否成功
}

float Ina226BatteryMonitor::get_soc_from_voltage(float voltage_v) const 
{
  const SocPoint *table = config_.soc_table; // 获取SOC查表指针
  size_t table_len = config_.soc_table_len; // 获取查表长度
  if (table == nullptr || table_len < 2) // 如果表格为空或长度不足
  {
    table = k_default_soc_table_; // 使用默认表格
    table_len = sizeof(k_default_soc_table_) / sizeof(k_default_soc_table_[0]); // 计算默认表格长度
  }

  if (voltage_v >= table[0].voltage_v) // 如果电压高于或等于最高电压点
    return table[0].soc_percent; // 返回对应的SOC（通常是100%）
  if (voltage_v <= table[table_len - 1].voltage_v) // 如果电压低于或等于最低电压点
    return table[table_len - 1].soc_percent; // 返回对应的SOC（通常是0%）

  for (size_t i = 0; i + 1 < table_len; i++) // 遍历表格区间
  {
    const float high_v = table[i].voltage_v; // 当前区间高电压
    const float low_v = table[i + 1].voltage_v; // 当前区间低电压
    if (voltage_v <= high_v && voltage_v > low_v) // 如果电压在当前范围内
    {
      const float high_p = table[i].soc_percent; // 对应的SOC高值
      const float low_p = table[i + 1].soc_percent; // 对应的SOC低值
      const float percentage = low_p + (voltage_v - low_v) * (high_p - low_p) / (high_v - low_v); // 线性插值计算SOC
      return percentage; // 返回计算出的百分比
    }
  }

  return table[table_len - 1].soc_percent; // 默认返回最低SOC
}

void Ina226BatteryMonitor::maybe_save_to_nvs(uint32_t now_ms, bool force) 
{
  if (!is_nvs_enabled()) // 如果NVS未启用
  {
    return; // 直接返回
  }

  if (!force && (now_ms - last_nvs_save_ms_) < config_.save_interval_ms) // 如果非强制保存且未到保存间隔
  {
    return; // 直接返回
  }

  if (!force && !isnan(last_saved_remaining_capacity_mah_) && // 检查上次保存值是否有效
      fabs(remaining_capacity_mah_ - last_saved_remaining_capacity_mah_) < config_.min_save_delta_mah) // 检查变化量
  {
    last_nvs_save_ms_ = now_ms; // 更新上次尝试保存的时间
    return; // 直接返回
  }

  if (save_remaining_capacity_to_nvs(remaining_capacity_mah_)) // 尝试执行保存
  {
    last_saved_remaining_capacity_mah_ = remaining_capacity_mah_; // 更新上次保存的容量值
    last_nvs_save_ms_ = now_ms; // 更新保存时间
    logf("NVS saved: remaining=%.2f mAh (SoC %.1f%%)\n", remaining_capacity_mah_, soc_percent_); // 打印日志：保存成功
  }
  else
  {
    logf("NVS save failed\n"); // 打印日志：保存失败
    last_nvs_save_ms_ = now_ms; // 即使失败也更新时间
  }
}

void Ina226BatteryMonitor::logf(const char *format, ...) const 
{
  if (logger_ == nullptr) // 如果日志对象未设置
  {
    return; // 直接返回
  }

  char buffer[128]; // 定义缓冲区
  va_list args; // 定义可变参数列表
  va_start(args, format); // 初始化可变参数
  vsnprintf(buffer, sizeof(buffer), format, args); // 格式化字符串到缓冲区
  va_end(args); // 结束可变参数处理
  logger_->print(buffer); // 输出日志
}
