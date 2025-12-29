#pragma once // 防止头文件重复包含

#include <Arduino.h> // 包含Arduino核心库
#include <INA226.h> // 包含INA226驱动库

#include <math.h> // 包含数学库

/**
 * @brief INA226 电池监视器类
 */
class Ina226BatteryMonitor
{
public:
  /**
   * @brief 电池SOC电压对照点结构体
   */
  struct SocPoint
  {
    float voltage_v; // 电池电压(V)
    float soc_percent; // 对应的SOC百分比(0-100)
  };

  /**
   * @brief 配置结构体
   */
  struct Config
  {
    uint8_t i2c_address = 0x40; // INA226 I2C设备地址
    TwoWire *wire = &Wire; // I2C总线指针,默认Wire
    bool init_wire = true; // 是否自动初始化Wire
    int sda_pin = -1; // I2C SDA引脚,-1表示使用默认
    int scl_pin = -1; // I2C SCL引脚,-1表示使用默认

    float battery_capacity_mah = 3000.0f; // 电池总容量(mAh)
    float shunt_resistor_ohm = 0.02f; // 采样电阻阻值(Ohm)
    float max_current_amps = 4.0f; // 预期的最大电流(A)
    int current_polarity = 1; // 电流极性修正: 1 或 -1
    float current_deadzone_ma = 1.0f; // 电流死区(mA),小于此值视为0
    uint8_t average = INA226_16_SAMPLES; // INA226平均采样点数

    const SocPoint *soc_table = nullptr; // 自定义SOC查表数组指针
    size_t soc_table_len = 0; // 自定义SOC查表数组长度

    const char *nvs_namespace = "bat"; // NVS命名空间
    const char *nvs_key_state = "state"; // NVS键名,用于存储状态
    uint32_t save_interval_ms = 10UL * 60UL * 1000UL; // 自动保存到NVS的时间间隔(ms)
    double min_save_delta_mah = 1.0; // 触发NVS保存的最小容量变化(mAh)

    uint32_t startup_voltage_samples = 5; // 启动时的电压采样次数,用于初始估算
    uint32_t startup_voltage_sample_delay_ms = 50; // 启动时每次电压采样的间隔(ms)

    float full_charge_voltage_v = 12.5f; // 满充判定电压(V)
    float full_charge_current_ma = 50.0f; // 满充判定电流(mA),小于此值且电压满足视为满充
  };

  /**
   * @brief 采样数据结构体
   */
  struct Sample
  {
    float bus_voltage_v = NAN; // 总线电压(V)
    float shunt_voltage_mv = NAN; // 分流电阻电压(mV)
    float current_ma = NAN; // 电流(mA)
    float power_mw = NAN; // 功率(mW)
    float power2_mw = NAN; // 计算功率 P=U*I (mW)
    double remaining_capacity_mah = NAN; // 剩余容量(mAh)
    float soc_percent = NAN; // 剩余电量百分比(%)
  };

  /**
   * @brief 构造函数
   * @param config 配置对象
   */
  explicit Ina226BatteryMonitor(const Config &config);

  /**
   * @brief 设置日志输出对象
   * @param logger实现了Print接口的对象指针(如&Serial)
   */
  void set_logger(Print *logger);

  /**
   * @brief 初始化电池监视器
   * @return true 初始化成功, false 初始化失败
   */
  bool begin();

  /**
   * @brief 更新电池状态
   * @param now_ms 当前系统时间戳(ms)
   * @param serial 可选的调试串口,用于接收调试指令('c'清除NVS, 'r'重置状态)
   */
  void update(uint32_t now_ms, Stream *serial = nullptr);

  /**
   * @brief 更新电池状态(自动获取时间)
   * @param serial 可选的调试串口
   */
  void update(Stream *serial = nullptr);

  /**
   * @brief 获取最新的采样数据
   * @return Sample结构体的常量引用
   */
  const Sample &sample() const;

  /**
   * @brief 根据电压重置电池状态(SOC和容量)
   * @param voltage_v 当前电池电压(V)
   */
  void reset_state_from_voltage(float voltage_v);

  /**
   * @brief 清除NVS中保存的电池状态
   */
  void clear_nvs_state();

private:
  /**
   * @brief 持久化保存的电池状态结构
   */
  struct __attribute__((packed)) PersistedBatteryState
  {
    uint32_t magic; // 魔数,用于校验数据有效性
    uint16_t version; // 版本号
    uint16_t reserved; // 保留字段
    uint32_t capacity_mah_x1; // 电池总容量
    uint32_t remaining_mah_x100; // 剩余容量 * 100
    uint32_t crc32; // CRC32校验和
  };

  /**
   * @brief 计算CRC32校验和(小端序)
   * @param data 数据指针
   * @param length 数据长度
   * @return 计算出的CRC32值
   */
  static uint32_t calc_crc32_le(const uint8_t *data, size_t length);

  /**
   * @brief 检查NVS是否启用
   * @return true 已启用, false 未启用
   */
  bool is_nvs_enabled() const;

  /**
   * @brief 从NVS加载剩余容量
   * @param out_remaining_capacity_mah 输出参数,加载到的剩余容量
   * @return true 加载成功, false 加载失败
   */
  bool load_remaining_capacity_from_nvs(double &out_remaining_capacity_mah) const;

  /**
   * @brief 保存剩余容量到NVS
   * @param remaining_capacity_mah 当前剩余容量
   * @return true 保存成功, false 保存失败
   */
  bool save_remaining_capacity_to_nvs(double remaining_capacity_mah) const;

  /**
   * @brief 根据电压估算SOC
   * @param voltage_v 电池电压(V)
   * @return 估算的SOC百分比(0-100)
   */
  float get_soc_from_voltage(float voltage_v) const;

  /**
   * @brief 尝试保存到NVS(根据策略判断是否需要保存)
   * @param now_ms 当前时间戳(ms)
   * @param force 是否强制保存
   */
  void maybe_save_to_nvs(uint32_t now_ms, bool force);

  /**
   * @brief 格式化输出日志
   * @param format 格式化字符串
   * @param ... 可变参数
   */
  void logf(const char *format, ...) const;

  static const SocPoint k_default_soc_table_[]; // 默认的SOC查表

  Config config_{}; // 配置副本
  Print *logger_ = nullptr; // 日志对象指针

  INA226 ina226_; // INA226驱动实例

  Sample sample_{}; // 最新采样数据
  double remaining_capacity_mah_ = NAN; // 当前剩余容量(mAh)
  float soc_percent_ = NAN; // 当前SOC(%)

  uint32_t last_time_ms_ = 0; // 上次更新的时间戳
  uint32_t last_nvs_save_ms_ = 0; // 上次NVS保存的时间戳
  double last_saved_remaining_capacity_mah_ = NAN; // 上次保存到NVS的容量值
};

