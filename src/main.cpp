#include <INA226.h>
#include <Preferences.h>
#include <Wire.h>

#include <math.h>
#include <stddef.h>

// ---- Battery state persistence (ESP32 NVS/Preferences) ----
static constexpr const char *NVS_NAMESPACE = "bat";
static constexpr const char *NVS_KEY_STATE = "state";
static constexpr uint32_t BATTERY_STATE_MAGIC = 0x42415431; // "BAT1"
static constexpr uint16_t BATTERY_STATE_VERSION = 1;
static constexpr unsigned long SAVE_INTERVAL_MS = 10UL * 60UL * 1000UL;
static constexpr double MIN_SAVE_DELTA_MAH = 1.0;

struct __attribute__((packed)) PersistedBatteryState
{
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;
  uint32_t capacity_mAh_x1;
  uint32_t remaining_mAh_x100;
  uint32_t crc32;
};

static uint32_t crc32_le(const uint8_t *data, size_t length)
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

static bool loadRemainingCapacityFromNvs(double &outRemainingCapacity_mAh, float batteryCapacity_mAh)
{
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, true))
  {
    return false;
  }

  PersistedBatteryState state{};
  const size_t expectedSize = sizeof(state);
  const size_t storedSize = prefs.getBytesLength(NVS_KEY_STATE);
  if (storedSize != expectedSize)
  {
    prefs.end();
    return false;
  }

  const size_t readSize = prefs.getBytes(NVS_KEY_STATE, &state, expectedSize);
  prefs.end();
  if (readSize != expectedSize)
  {
    return false;
  }

  if (state.magic != BATTERY_STATE_MAGIC || state.version != BATTERY_STATE_VERSION)
  {
    return false;
  }

  const uint32_t expectedCapacity = static_cast<uint32_t>(batteryCapacity_mAh + 0.5f);
  if (state.capacity_mAh_x1 != expectedCapacity)
  {
    return false;
  }

  const uint32_t expectedCrc = crc32_le(reinterpret_cast<const uint8_t *>(&state), offsetof(PersistedBatteryState, crc32));
  if (state.crc32 != expectedCrc)
  {
    return false;
  }

  outRemainingCapacity_mAh = static_cast<double>(state.remaining_mAh_x100) / 100.0;
  return true;
}

static bool saveRemainingCapacityToNvs(double remainingCapacity_mAh, float batteryCapacity_mAh)
{
  if (remainingCapacity_mAh < 0)
    remainingCapacity_mAh = 0;
  if (remainingCapacity_mAh > batteryCapacity_mAh)
    remainingCapacity_mAh = batteryCapacity_mAh;

  PersistedBatteryState state{};
  state.magic = BATTERY_STATE_MAGIC;
  state.version = BATTERY_STATE_VERSION;
  state.reserved = 0;
  state.capacity_mAh_x1 = static_cast<uint32_t>(batteryCapacity_mAh + 0.5f);
  state.remaining_mAh_x100 = static_cast<uint32_t>(remainingCapacity_mAh * 100.0 + 0.5);
  state.crc32 = crc32_le(reinterpret_cast<const uint8_t *>(&state), offsetof(PersistedBatteryState, crc32));

  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false))
  {
    return false;
  }

  const size_t writtenSize = prefs.putBytes(NVS_KEY_STATE, &state, sizeof(state));
  prefs.end();
  return writtenSize == sizeof(state);
}

// I2C 引脚 (ESP32 默认)
#define I2C_SDA 32
#define I2C_SCL 33

// INA226 实例
INA226 INA(0x40); // 默认 I2C 地址通常是 0x40

// 电池参数设置
const float BATTERY_CAPACITY_MAH = 3000.0; // 电池总容量
const float SHUNT_RESISTOR_OHM = 0.02;     // 采样电阻阻值 (根据你的模块修改！R020=0.02, R100=0.1)
const float MAX_CURRENT_AMPS = 4.0;        // 预计最大电流 (用于校准精度)

// 库仑计变量
float current_mA = 0;
float busVoltage_V = 0;
float shuntVoltage_mV = 0;
double remainingCapacity_mAh = BATTERY_CAPACITY_MAH; // 初始假设满电
float soc = 100.0;                                   // State of Charge (%)

// 时间积分变量
unsigned long lastTime = 0;
unsigned long lastNvsSaveMs = 0;
double lastSavedRemainingCapacity_mAh = NAN;

// 3S 三元锂电池电压 vs 容量百分比查表 (参考值)
// 格式: {电压(V), 百分比(%)}
// 注意：这是开路电压(OCV)，即空载时的电压
const float soc_lookup[][2] = {
    {12.60, 100},
    {12.30, 90},
    {12.00, 80},
    {11.70, 70},
    {11.40, 60},
    {11.10, 50},
    {10.80, 40},
    {10.50, 30},
    {10.20, 20},
    {9.60, 10}, // 低电量段电压下降快
    {9.00, 0}   // 截止电压
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
  if (voltage >= 12.60)
    return 100.0;
  if (voltage <= 9.00)
    return 0.0;

  // 2. 查表并进行线性插值
  for (int i = 0; i < 10; i++)
  {
    float highV = soc_lookup[i][0];
    float lowV = soc_lookup[i + 1][0];

    // 如果电压在当前区间内
    if (voltage <= highV && voltage > lowV)
    {
      float highP = soc_lookup[i][1];
      float lowP = soc_lookup[i + 1][1];

      // 线性插值公式
      float percentage = lowP + (voltage - lowV) * (highP - lowP) / (highV - lowV);
      return percentage;
    }
  }
  return 0.0;
}

static void maybeSaveToNvs(unsigned long nowMs, bool force = false)
{
  if (!force && (nowMs - lastNvsSaveMs) < SAVE_INTERVAL_MS)
  {
    return;
  }

  if (!force && !isnan(lastSavedRemainingCapacity_mAh) &&
      fabs(remainingCapacity_mAh - lastSavedRemainingCapacity_mAh) < MIN_SAVE_DELTA_MAH)
  {
    lastNvsSaveMs = nowMs;
    return;
  }

  if (saveRemainingCapacityToNvs(remainingCapacity_mAh, BATTERY_CAPACITY_MAH))
  {
    lastSavedRemainingCapacity_mAh = remainingCapacity_mAh;
    lastNvsSaveMs = nowMs;
    Serial.printf("NVS saved: remaining=%.2f mAh (SoC %.1f%%)\n", remainingCapacity_mAh, soc);
  }
  else
  {
    Serial.println("NVS save failed");
    lastNvsSaveMs = nowMs;
  }
}

void setup()
{
  Serial.begin(115200);
  Wire.begin(I2C_SDA, I2C_SCL);

  Serial.println(__FILE__);
  Serial.print("INA226_LIB_VERSION: ");
  Serial.println(INA226_LIB_VERSION);

  Serial.println("Initializing INA226...");

  if (!INA.begin())
  {
    while (1)
    {
      Serial.println("Could not connect to INA226. Fix wiring.");
      delay(2000);
    }
  }
  else
  {
    Serial.println("INA226 connected successfully.");
  }

  // --- 核心配置 ---
  // 设置最大电流和分流电阻以进行自动校准
  // INA226 的精度取决于这个校准。
  INA.setMaxCurrentShunt(MAX_CURRENT_AMPS, SHUNT_RESISTOR_OHM);

  // 设置平均模式，减少抖动 (16次取平均)
  INA.setAverage(INA226_16_SAMPLES);

  // --- 新增：开机初值估算 ---
  // 连续读取几次求平均，避免瞬间波动
  float totalVoltage = 0;
  for (int i = 0; i < 5; i++)
  {
    totalVoltage += INA.getBusVoltage();
    delay(50);
  }
  float startUpVoltage = totalVoltage / 5.0;

  // 通过电压估算当前百分比
  soc = getSoCFromVoltage(startUpVoltage);

  // 根据估算的百分比，反推当前剩余 mAh
  remainingCapacity_mAh = (soc / 100.0) * BATTERY_CAPACITY_MAH;

  Serial.print("Startup Voltage: ");
  Serial.print(startUpVoltage);
  Serial.println(" V");
  Serial.print("Estimated Initial SoC: ");
  Serial.print(soc);
  Serial.println(" %");
  // ---------------------------

  double savedRemainingCapacity_mAh = 0;
  if (loadRemainingCapacityFromNvs(savedRemainingCapacity_mAh, BATTERY_CAPACITY_MAH))
  {
    remainingCapacity_mAh = savedRemainingCapacity_mAh;
    if (remainingCapacity_mAh < 0)
      remainingCapacity_mAh = 0;
    if (remainingCapacity_mAh > BATTERY_CAPACITY_MAH)
      remainingCapacity_mAh = BATTERY_CAPACITY_MAH;

    soc = (remainingCapacity_mAh / BATTERY_CAPACITY_MAH) * 100.0;
    Serial.printf("NVS loaded: remaining=%.2f mAh (SoC %.1f%%)\n", remainingCapacity_mAh, soc);
  }
  else
  {
    Serial.println("NVS not found/invalid, using OCV estimate and seeding NVS...");
    if (saveRemainingCapacityToNvs(remainingCapacity_mAh, BATTERY_CAPACITY_MAH))
    {
      Serial.printf("NVS seeded: remaining=%.2f mAh (SoC %.1f%%)\n", remainingCapacity_mAh, soc);
    }
    else
    {
      Serial.println("NVS seed failed");
    }
  }

  Serial.println("INA226 Ready!");
  lastTime = millis();
  lastNvsSaveMs = lastTime;
  lastSavedRemainingCapacity_mAh = remainingCapacity_mAh;
}

void loop()
{
  unsigned long currentTime = millis();

  // 1. 读取传感器数据
  busVoltage_V = INA.getBusVoltage();         // 负载电压 (V)
  shuntVoltage_mV = INA.getShuntVoltage_mV(); // 采样电阻两端压降 (mV)
  current_mA = INA.getCurrent_mA();           // 电流 (mA)

  // 修正：INA226 读取的放电电流方向。
  // 如果读取为负值，取绝对值用于计算消耗；如果是充电，则逻辑相反。
  // 这里假设我们在监测放电（电流为正或需取反，取决于你的 IN+/IN- 接线方向）
  float dischargeCurrent = (abs(current_mA) < 1.0) ? 0 : abs(current_mA); // 设置死区，避免底噪影响

  // 2. 库仑计积分算法 (积分：电流 * 时间)
  if (currentTime > lastTime)
  {
    unsigned long timeDiff = currentTime - lastTime;

    // 将毫秒转换为小时: timeDiff / 1000.0 / 3600.0
    double hoursPassed = (double)timeDiff / 3600000.0;

    // 计算这段时间内消耗的 mAh
    double mAhConsumed = dischargeCurrent * hoursPassed;

    // 从剩余容量中减去
    // 注意：如果 current_mA 读数有极小的底噪（如无负载显示 0.5mA），需设置死区阈值过滤
    if (dischargeCurrent > 1.0)
    {
      remainingCapacity_mAh -= mAhConsumed;
    }

    // 限制范围，防止变成负数
    if (remainingCapacity_mAh < 0)
      remainingCapacity_mAh = 0;
    if (remainingCapacity_mAh > BATTERY_CAPACITY_MAH)
      remainingCapacity_mAh = BATTERY_CAPACITY_MAH;

    // 计算百分比 SoC
    soc = (remainingCapacity_mAh / BATTERY_CAPACITY_MAH) * 100.0;

    lastTime = currentTime;
  }

  // 3. 简单的电压复位校准逻辑 (可选)
  // 如果电压达到 12.5V 以上且电流很小，我们可以认为电池充满，重置库仑计
  if (busVoltage_V > 12.5 && dischargeCurrent < 50)
  {
    remainingCapacity_mAh = BATTERY_CAPACITY_MAH;
    soc = 100.0;
    Serial.println("Battery Charged. SoC reset to 100%");
    maybeSaveToNvs(currentTime, true);
  }

  // 4. 打印数据
  // 打印表头
  maybeSaveToNvs(currentTime);

  Serial.println("\nPOWER2 = busVoltage x current");
  Serial.println(" V\t mA \t mW \t mW \t %");
  Serial.println("BUS\tCURRENT\tPOWER\tPOWER2\tSoC");
  // 打印读取到的数据
  Serial.print(busVoltage_V, 3); // 电压，保留3位小数
  Serial.print("\t");
  Serial.print(current_mA, 3); // 电流
  Serial.print("\t");
  Serial.print(INA.getPower_mW(), 2); // 传感器计算的功率
  Serial.print("\t");

  // 手动计算功率 (电压 * 电流) 用于对比验证
  Serial.print(busVoltage_V * current_mA, 2);
  Serial.print("\t");
  Serial.print(soc, 2);
  Serial.print("\t");

  delay(1000); // 1秒更新一次
}
