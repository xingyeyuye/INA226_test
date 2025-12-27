#include <INA226.h>

// I2C 引脚 (ESP32 默认)
#define I2C_SDA 32
#define I2C_SCL 33

// INA226 实例
INA226 INA(0x40); // 默认 I2C 地址通常是 0x40

// 电池参数设置
const float BATTERY_CAPACITY_MAH = 3000.0; // 电池总容量
const float SHUNT_RESISTOR_OHM = 0.02;     // 采样电阻阻值 (根据你的模块修改！R020=0.02, R100=0.1)
const float MAX_CURRENT_AMPS = 4.0;       // 预计最大电流 (用于校准精度)

// 库仑计变量
float current_mA = 0;
float busVoltage_V = 0;
float shuntVoltage_mV = 0;
double remainingCapacity_mAh = BATTERY_CAPACITY_MAH; // 初始假设满电
float soc = 100.0; // State of Charge (%)

// 时间积分变量
unsigned long lastTime = 0;

void setup() {
  Serial.begin(115200);
  Wire.begin(I2C_SDA, I2C_SCL);

  Serial.println(__FILE__);
  Serial.print("INA226_LIB_VERSION: ");
  Serial.println(INA226_LIB_VERSION);

  Serial.println("Initializing INA226...");
  
  if (!INA.begin()) {
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
  
  Serial.println("INA226 Ready!");
  lastTime = millis();
}
 
void loop() {
  unsigned long currentTime = millis();
  
  // 1. 读取传感器数据
  busVoltage_V = INA.getBusVoltage();       // 负载电压 (V)
  shuntVoltage_mV = INA.getShuntVoltage_mV(); // 采样电阻两端压降 (mV)
  current_mA = INA.getCurrent_mA();         // 电流 (mA)
  
  // 修正：INA226 读取的放电电流方向。
  // 如果读取为负值，取绝对值用于计算消耗；如果是充电，则逻辑相反。
  // 这里假设我们在监测放电（电流为正或需取反，取决于你的 IN+/IN- 接线方向）
  float dischargeCurrent = abs(current_mA); 

  // 2. 库仑计积分算法 (积分：电流 * 时间)
  if (currentTime > lastTime) { 
    unsigned long timeDiff = currentTime - lastTime;
      
    // 将毫秒转换为小时: timeDiff / 1000.0 / 3600.0
    double hoursPassed = (double)timeDiff / 3600000.0;
    
    // 计算这段时间内消耗的 mAh
    double mAhConsumed = dischargeCurrent * hoursPassed;
     
    // 从剩余容量中减去
    // 注意：如果 current_mA 读数有极小的底噪（如无负载显示 0.5mA），需设置死区阈值过滤
    if(dischargeCurrent > 1.0) { 
        remainingCapacity_mAh -= mAhConsumed;
    }
    
    // 限制范围，防止变成负数
    if (remainingCapacity_mAh < 0) remainingCapacity_mAh = 0;
    if (remainingCapacity_mAh > BATTERY_CAPACITY_MAH) remainingCapacity_mAh = BATTERY_CAPACITY_MAH;
    
    // 计算百分比 SoC
    soc = (remainingCapacity_mAh / BATTERY_CAPACITY_MAH) * 100.0;
    
    lastTime = currentTime;
  }

  // 3. 简单的电压复位校准逻辑 (可选)
  // 如果电压达到 12.5V 以上且电流很小，我们可以认为电池充满，重置库仑计
  if (busVoltage_V > 12.5 && dischargeCurrent < 50) {
      remainingCapacity_mAh = BATTERY_CAPACITY_MAH;
      soc = 100.0;
      Serial.println("Battery Charged. SoC reset to 100%");
  }

  // 4. 打印数据
  // 打印表头
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
