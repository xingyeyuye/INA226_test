#include <INA226.h> // 引入INA226传感器库
#include <Preferences.h> // 引入ESP32 NVS偏好设置库
#include <Wire.h> // 引入I2C通信库

#include <math.h> // 引入数学库
#include <stddef.h> // 引入标准定义库

// ---- Battery state persistence (ESP32 NVS/Preferences) ----
// ---- 电池状态持久化 (ESP32 NVS/首选项) ----
static constexpr const char *NVS_NAMESPACE = "bat"; // NVS 命名空间
static constexpr const char *NVS_KEY_STATE = "state"; // NVS 键名
static constexpr uint32_t BATTERY_STATE_MAGIC = 0x42415431; // "BAT1" 魔数，用于校验数据合法性
static constexpr uint16_t BATTERY_STATE_VERSION = 1; // 数据结构版本号
static constexpr unsigned long SAVE_INTERVAL_MS = 10UL * 60UL * 1000UL; // 保存间隔：10分钟
static constexpr double MIN_SAVE_DELTA_MAH = 1.0; // 最小保存变化量：1mAh

// 定义持久化电池状态结构体，禁止字节对齐填充
struct __attribute__((packed)) PersistedBatteryState
{
  uint32_t magic; // 魔数
  uint16_t version; // 版本号
  uint16_t reserved; // 保留字段
  uint32_t capacity_mAh_x1; // 电池容量 (mAh)
  uint32_t remaining_mAh_x100; // 剩余容量 (maH * 100)
  uint32_t crc32; // CRC32 校验和
};

// CRC32 校验算法实现
static uint32_t crc32_le(const uint8_t *data, size_t length)
{
  uint32_t crc = 0xFFFFFFFFu; // 初始化 CRC
  for (size_t i = 0; i < length; i++) // 遍历每个字节
  {
    crc ^= data[i]; // 异或数据
    for (int bit = 0; bit < 8; bit++) // 遍历每一位
    {
      const uint32_t mask = -(crc & 1u); // 计算掩码
      crc = (crc >> 1) ^ (0xEDB88320u & mask); // CRC 移位和异或多项式
    }
  }
  return ~crc; // 返回反码
}

// 从 NVS 加载剩余容量
static bool loadRemainingCapacityFromNvs(double &outRemainingCapacity_mAh, float batteryCapacity_mAh)
{
  Preferences prefs; // 创建 Preferences 对象
  if (!prefs.begin(NVS_NAMESPACE, true)) // 以只读模式打开 NVS 命名空间
  {
    return false; // 打开失败
  }

  PersistedBatteryState state{}; // 定义状态变量
  const size_t expectedSize = sizeof(state); // 获取结构体大小
  const size_t storedSize = prefs.getBytesLength(NVS_KEY_STATE); // 获取存储的数据长度
  if (storedSize != expectedSize) // 如果长度不匹配
  {
    prefs.end(); // 关闭 NVS
    return false; // 返回失败
  }

  const size_t readSize = prefs.getBytes(NVS_KEY_STATE, &state, expectedSize); // 读取数据
  prefs.end(); // 关闭 NVS
  if (readSize != expectedSize) // 如果读取长度不匹配
  {
    return false; // 返回失败
  }

  if (state.magic != BATTERY_STATE_MAGIC || state.version != BATTERY_STATE_VERSION) // 校验魔数和版本
  {
    return false; // 校验失败
  }

  const uint32_t expectedCapacity = static_cast<uint32_t>(batteryCapacity_mAh + 0.5f); // 计算期望容量
  if (state.capacity_mAh_x1 != expectedCapacity) // 如果容量不匹配 (更换了电池配置)
  {
    return false; // 返回失败
  }

  const uint32_t expectedCrc = crc32_le(reinterpret_cast<const uint8_t *>(&state), offsetof(PersistedBatteryState, crc32)); // 计算 CRC
  if (state.crc32 != expectedCrc) // 校验 CRC
  {
    return false; // 校验失败
  }

  outRemainingCapacity_mAh = static_cast<double>(state.remaining_mAh_x100) / 100.0; // 转换剩余容量格式
  return true; // 加载成功
}

// 保存剩余容量到 NVS
static bool saveRemainingCapacityToNvs(double remainingCapacity_mAh, float batteryCapacity_mAh)
{
  if (remainingCapacity_mAh < 0) // 限制下限
    remainingCapacity_mAh = 0;
  if (remainingCapacity_mAh > batteryCapacity_mAh) // 限制上限
    remainingCapacity_mAh = batteryCapacity_mAh;

  PersistedBatteryState state{}; // 定义状态变量
  state.magic = BATTERY_STATE_MAGIC; // 设置魔数
  state.version = BATTERY_STATE_VERSION; // 设置版本
  state.reserved = 0; // 清零保留字段
  state.capacity_mAh_x1 = static_cast<uint32_t>(batteryCapacity_mAh + 0.5f); // 设置总容量
  state.remaining_mAh_x100 = static_cast<uint32_t>(remainingCapacity_mAh * 100.0 + 0.5); // 设置剩余容量
  state.crc32 = crc32_le(reinterpret_cast<const uint8_t *>(&state), offsetof(PersistedBatteryState, crc32)); // 计算并设置 CRC

  Preferences prefs; // 创建 Preferences 对象
  if (!prefs.begin(NVS_NAMESPACE, false)) // 以读写模式打开 NVS
  {
    return false; // 打开失败
  }

  const size_t writtenSize = prefs.putBytes(NVS_KEY_STATE, &state, sizeof(state)); // 写入数据
  prefs.end(); // 关闭 NVS
  return writtenSize == sizeof(state); // 返回写入结果
}

// I2C 引脚 (ESP32 默认)
#define I2C_SDA 32 // 定义 SDA 引脚
#define I2C_SCL 33 // 定义 SCL 引脚

// INA226 实例
INA226 INA(0x40); // 实例化 INA226 对象，地址 0x40

// 电池参数设置
const float BATTERY_CAPACITY_MAH = 3000.0; // 电池总容量 3000mAh
const float SHUNT_RESISTOR_OHM = 0.02;     // 采样电阻阻值 (0.02欧姆)
const float MAX_CURRENT_AMPS = 4.0;        // 预计最大电流 4A

// 库仑计变量
float current_mA = 0; // 当前电流 (mA)
float busVoltage_V = 0; // 母线电压 (V)
float shuntVoltage_mV = 0; // 分流电压 (mV)
double remainingCapacity_mAh = BATTERY_CAPACITY_MAH; // 剩余容量 (mAh)，初始假设满电
float soc = 100.0;                                   // 电池荷电状态 (%)

// 时间积分变量
unsigned long lastTime = 0; // 上次计算时间
unsigned long lastNvsSaveMs = 0; // 上次 NVS 保存时间
double lastSavedRemainingCapacity_mAh = NAN; // 上次保存的剩余容量

// 3S 三元锂电池电压 vs 容量百分比查表 (参考值)
// 格式: {电压(V), 百分比(%)}
// 注意：这是开路电压(OCV)，即空载时的电压
const float soc_lookup[][2] = {
    {12.60, 100}, // 100%
    {12.30, 90},  // 90%
    {12.00, 80},  // 80%
    {11.70, 70},  // 70%
    {11.40, 60},  // 60%
    {11.10, 50},  // 50%
    {10.80, 40},  // 40%
    {10.50, 30},  // 30%
    {10.20, 20},  // 20%
    {9.60, 10},   // 10%
    {9.00, 0}     // 0%
};

/**
 * @brief: 根据电压查表并线性插值计算 SoC 百分比
 * @param  float  voltage
 * @return  float  电池剩余容量百分比 0.0~100.0%
 * @author: 朱凯文
 * @Date: 2025-12-27 14:41:36
 **/
float getSoCFromVoltage(float voltage)
{
  // 1. 处理边界情况
  if (voltage >= 12.60) // 高于最高电压
    return 100.0; // 返回 100%
  if (voltage <= 9.00) // 低于最低电压
    return 0.0; // 返回 0%

  // 2. 查表并进行线性插值
  for (int i = 0; i < 10; i++) // 遍历查找表
  {
    float highV = soc_lookup[i][0]; // 上限电压
    float lowV = soc_lookup[i + 1][0]; // 下限电压

    // 如果电压在当前区间内
    if (voltage <= highV && voltage > lowV)
    {
      float highP = soc_lookup[i][1]; // 上限百分比
      float lowP = soc_lookup[i + 1][1]; // 下限百分比

      // 线性插值公式
      float percentage = lowP + (voltage - lowV) * (highP - lowP) / (highV - lowV);
      return percentage; // 返回计算出的百分比
    }
  }
  return 0.0; // 默认返回 0
}

// 尝试保存到 NVS，带节流
static void maybeSaveToNvs(unsigned long nowMs, bool force = false)
{
  if (!force && (nowMs - lastNvsSaveMs) < SAVE_INTERVAL_MS) // 检查时间间隔
  {
    return; // 时间未到，不保存
  }

  if (!force && !isnan(lastSavedRemainingCapacity_mAh) &&
      fabs(remainingCapacity_mAh - lastSavedRemainingCapacity_mAh) < MIN_SAVE_DELTA_MAH) // 检查变化量
  {
    lastNvsSaveMs = nowMs; // 更新时间，推迟保存
    return; // 变化太小，不保存
  }

  if (saveRemainingCapacityToNvs(remainingCapacity_mAh, BATTERY_CAPACITY_MAH)) // 执行保存
  {
    lastSavedRemainingCapacity_mAh = remainingCapacity_mAh; // 更新上次保存值
    lastNvsSaveMs = nowMs; // 更新保存时间
    Serial.printf("NVS saved: remaining=%.2f mAh (SoC %.1f%%)\n", remainingCapacity_mAh, soc); // 打印日志
  }
  else
  {
    Serial.println("NVS save failed"); // 保存失败日志
    lastNvsSaveMs = nowMs; // 更新时间
  }
}

// 初始化 Setup 函数
void setup()
{
  Serial.begin(115200); // 初始化串口波特率
  Wire.begin(I2C_SDA, I2C_SCL); // 初始化 I2C 总线

  Serial.println(__FILE__); // 打印文件名
  Serial.print("INA226_LIB_VERSION: "); // 打印库版本前缀
  Serial.println(INA226_LIB_VERSION); // 打印库版本

  Serial.println("Initializing INA226..."); // 打印初始化提示

  if (!INA.begin()) // 初始化 INA226
  {
    while (1) // 如果失败，进入死循环
    {
      Serial.println("Could not connect to INA226. Fix wiring."); // 打印错误提示
      delay(2000); // 延时 2 秒
    }
  }
  else
  {
    Serial.println("INA226 connected successfully."); // 初始化成功提示
  }

  // --- 核心配置 ---
  // 设置最大电流和分流电阻以进行自动校准
  // INA226 的精度取决于这个校准。
  INA.setMaxCurrentShunt(MAX_CURRENT_AMPS, SHUNT_RESISTOR_OHM); // 设置校准参数

  // 设置平均模式，减少抖动 (16次取平均)
  INA.setAverage(INA226_16_SAMPLES); // 设置平均采样数 16

  // --- 新增：开机初值估算 ---
  // 连续读取几次求平均，避免瞬间波动
  float totalVoltage = 0; // 总电压累加器
  for (int i = 0; i < 5; i++) // 循环 5 次
  {
    totalVoltage += INA.getBusVoltage(); // 读取电压并累加
    delay(50); // 延时 50ms
  }
  float startUpVoltage = totalVoltage / 5.0; // 计算平均电压

  // 通过电压估算当前百分比
  soc = getSoCFromVoltage(startUpVoltage); // 查表获取 SoC

  // 根据估算的百分比，反推当前剩余 mAh
  remainingCapacity_mAh = (soc / 100.0) * BATTERY_CAPACITY_MAH; // 计算剩余容量

  Serial.print("Startup Voltage: "); // 打印启动电压
  Serial.print(startUpVoltage); // 数值
  Serial.println(" V"); // 单位
  Serial.print("Estimated Initial SoC: "); // 打印初始 SoC
  Serial.print(soc); // 数值
  Serial.println(" %"); // 单位
  // ---------------------------

  double savedRemainingCapacity_mAh = 0; // 定义临时变量存储加载的容量
  if (loadRemainingCapacityFromNvs(savedRemainingCapacity_mAh, BATTERY_CAPACITY_MAH)) // 从 NVS 加载
  {
    remainingCapacity_mAh = savedRemainingCapacity_mAh; // 使用加载的值覆盖估算值
    if (remainingCapacity_mAh < 0) // 下限保护
      remainingCapacity_mAh = 0;
    if (remainingCapacity_mAh > BATTERY_CAPACITY_MAH) // 上限保护
      remainingCapacity_mAh = BATTERY_CAPACITY_MAH;

    soc = (remainingCapacity_mAh / BATTERY_CAPACITY_MAH) * 100.0; // 重新计算 SoC
    Serial.printf("NVS loaded: remaining=%.2f mAh (SoC %.1f%%)\n", remainingCapacity_mAh, soc); // 打印加载成功
  }
  else
  {
    Serial.println("NVS not found/invalid, using OCV estimate and seeding NVS..."); // 打印 NVS 未找到
    if (saveRemainingCapacityToNvs(remainingCapacity_mAh, BATTERY_CAPACITY_MAH)) // 尝试保存当前估算值
    {
      Serial.printf("NVS seeded: remaining=%.2f mAh (SoC %.1f%%)\n", remainingCapacity_mAh, soc); // 保存成功
    }
    else
    {
      Serial.println("NVS seed failed"); // 保存失败
    }
  }

  Serial.println("INA226 Ready!"); // 准备就绪
  lastTime = millis(); // 记录当前时间
  lastNvsSaveMs = lastTime; // 记录上次保存时间
  lastSavedRemainingCapacity_mAh = remainingCapacity_mAh; // 记录上次保存容量
}

// 主循环 Loop
void loop()
{
  unsigned long currentTime = millis(); // 获取当前运行时间

  // 1. 读取传感器数据
  busVoltage_V = INA.getBusVoltage();         // 读取负载电压 (V)
  shuntVoltage_mV = INA.getShuntVoltage_mV(); // 读取采样电阻两端压降 (mV)
  current_mA = INA.getCurrent_mA();           // 读取电流 (mA)

  // 修正：INA226 读取的放电电流方向。
  // 如果读取为负值，取绝对值用于计算消耗；如果是充电，则逻辑相反。
  // 这里假设我们在监测放电（电流为正或需取反，取决于你的 IN+/IN- 接线方向）
  float dischargeCurrent = (abs(current_mA) < 1.0) ? 0 : abs(current_mA); // 设置死区，避免底噪影响

  // 2. 库仑计积分算法 (积分：电流 * 时间)
  if (currentTime > lastTime) // 确保时间流逝
  {
    unsigned long timeDiff = currentTime - lastTime; // 计算时间差

    // 将毫秒转换为小时: timeDiff / 1000.0 / 3600.0
    double hoursPassed = (double)timeDiff / 3600000.0; // 转换为小时

    // 计算这段时间内消耗的 mAh
    double mAhConsumed = dischargeCurrent * hoursPassed; // 积分计算：电流 * 时间

    // 从剩余容量中减去
    // 注意：如果 current_mA 读数有极小的底噪（如无负载显示 0.5mA），需设置死区阈值过滤
    if (dischargeCurrent > 1.0) // 再次确认电流大于死区（冗余但安全）
    {
      remainingCapacity_mAh -= mAhConsumed; // 扣除消耗容量
    }

    // 限制范围，防止变成负数
    if (remainingCapacity_mAh < 0) // 下限保护
      remainingCapacity_mAh = 0;
    if (remainingCapacity_mAh > BATTERY_CAPACITY_MAH) // 上限保护
      remainingCapacity_mAh = BATTERY_CAPACITY_MAH;

    // 计算百分比 SoC
    soc = (remainingCapacity_mAh / BATTERY_CAPACITY_MAH) * 100.0; // 更新 SoC

    lastTime = currentTime; // 更新上次处理时间
  }

  // 3. 简单的电压复位校准逻辑 (可选)
  // 如果电压达到 12.5V 以上且电流很小，我们可以认为电池充满，重置库仑计
  if (busVoltage_V > 12.5 && dischargeCurrent < 50) // 判断充满条件
  {
    remainingCapacity_mAh = BATTERY_CAPACITY_MAH; // 重置容量为满
    soc = 100.0; // 重置 SoC 为 100%
    Serial.println("Battery Charged. SoC reset to 100%"); // 打印日志
    maybeSaveToNvs(currentTime, true); // 强制保存 NVS
  }

  // 4. 打印数据
  // 打印表头
  maybeSaveToNvs(currentTime); // 尝试定期保存 NVS

  Serial.println("\nPOWER2 = busVoltage x current"); // 打印说明
  Serial.println(" V\t mA \t mW \t mW \t %"); // 打印单位
  Serial.println("BUS\tCURRENT\tPOWER\tPOWER2\tSoC"); // 打印列名
  // 打印读取到的数据
  Serial.print(busVoltage_V, 3); // 打印电压，保留3位小数
  Serial.print("\t"); // 制表符
  Serial.print(current_mA, 3); // 打印电流
  Serial.print("\t"); // 制表符
  Serial.print(INA.getPower_mW(), 2); // 打印传感器计算的功率
  Serial.print("\t"); // 制表符

  // 手动计算功率 (电压 * 电流) 用于对比验证
  Serial.print(busVoltage_V * current_mA, 2); // 打印手动计算功率
  Serial.print("\t"); // 制表符
  Serial.print(soc, 2); // 打印 SoC
  Serial.print("\t"); // 制表符

  delay(1000); // 延时 1 秒，控制循环频率
}
