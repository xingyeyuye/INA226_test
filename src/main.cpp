#include "INA226.h"


INA226 INA(0x40);


void setup()
{
  Serial.begin(115200);
  Serial.println();
  Serial.println(__FILE__);
  Serial.print("INA226_LIB_VERSION: ");
  Serial.println(INA226_LIB_VERSION);
  Serial.println();

  Wire.begin(14, 12);
  if (!INA.begin() )
  {
    Serial.println("could not connect. Fix and Reboot");// 连接失败提示
  }
  else
  {
    Serial.println("INA226 connected successfully.");
  }
  delay(1000);
  Serial.println();
  //  Serial.print("AVG:\t");
  //  Serial.println((int)INA.getAverage());
  INA.setAverage(2);
  //  Serial.print("MAN:\t");
  //  Serial.println(INA.getManufacturerID(), HEX);
  //  Serial.print("DIE:\t");
  //  Serial.println(INA.getDieID(), HEX);
  delay(100);

  // 关键配置：设置最大电流和分流电阻值
  // 参数1: 最大预期电流 (Max Current) = 5 安培
  // 参数2: 分流电阻值(采样电阻) (Shunt Resistor) = 0.02 欧姆 (20mΩ)
  INA.setMaxCurrentShunt(5, 0.02);

}


void loop()
{
  // 打印表头
  Serial.println("\nPOWER2 = busVoltage x current");
  Serial.println(" V\t mA \t mW \t mW \t mW");
  Serial.println("BUS\tCURRENT\tPOWER\tPOWER2\tDELTA");

  for (int i = 0; i < 20; i++) // 循环读取 20 次数据
  {
    float bv = INA.getBusVoltage(); // 读取总线电压 (V)
    // float sv = INA.getShuntVoltage_mV(); // (被注释掉) 读取分流电阻(采样电阻)两端压降（mV）
    float cu = INA.getCurrent_mA(); // 读取电流 (mA)
    float po = INA.getPower_mW();   // 读取功率 (mW)

    // 打印读取到的数据
    Serial.print(bv, 3); // 电压，保留3位小数
    Serial.print("\t");
    Serial.print(cu, 3); // 电流
    Serial.print("\t");
    Serial.print(po, 2); // 传感器计算的功率
    Serial.print("\t");
    
    // 手动计算功率 (电压 * 电流) 用于对比验证
    Serial.print(bv * cu, 2); 
    Serial.print("\t");
    
    // 计算传感器读数与手动计算值的差值 (DELTA)
    Serial.print((bv * cu) - po, 2); 
    Serial.println();
    
    delay(50); // 每次读取间隔 50ms
  }
}