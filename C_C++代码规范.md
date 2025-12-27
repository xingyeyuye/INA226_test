# 文件编码

文件采用UTF-8编码

原因：UTF-8编码更加通用，部分国外软件无法正确读取GB2312、GBK编码

# 语法风格规范

## 文件

文件使用蛇形命名法（全小写，使用下划线作为连词符）

```C++
joystick_task.cpp
led_task.cpp
```

## 类名、结构体名

使用大驼峰（所有单词首字母大写，无连词符）

```C++
class JoystickTask{}；
class LedTask{}；
class ServoTool{}；
```

## 变量名、类实例名

使用蛇形

```C++
int16_t max_angle = 180;
int16_t min_angle = 0;
int16_t target_angle = 90;
LedTask led_task;
```

## 类成员名

使用蛇形，以 `_` 结尾

```C++
class Led
{
private:
    int pin_;
};
```

## 宏定义

所有字母大写，使用下划线(_)作为连词符

```C++
#define LED_PIN                 10
#define LED_TASK_RUNNING_CYCLE  20
```

## 常量

使用蛇形

```C++
const int16_t max_angle = 180;
const int16_t min_angle = -180;
```

常量表达式与宏规则一致

```C++
constexpr int8_t LOWEST_TEMPERATURE = -100;
constexpr uint8_t STEPPER_MICROSTEPS = 8;
constexpr uint8_t LED_PIN = 10;
constexpr uint8_t LED_TASK_RUNNING_CYCLE = 20;
```

## 枚举

枚举名使用大驼峰，枚举值与宏定义规则一致

```C++
enum class ArmState : uint8_t 
{
    ZERO_FORCE
    INITIALIZE,
    RUN,
};
```

## 函数、类方法

使用蛇形

```C++
void init_peripheral();
void update_data();
```

## 命名空间

使用蛇形

```C++
namespace can_bus {};
namespace system_task {};
```

## **文件内静态变量**

以 `s_` 开头

```C++
static uint32_t s_last_tick;
```

## 全局变量

以 `g_` 开头

```C++
uint32_t g_system_state;
```

## 输出 / 输入输出参数

使用 `out_` / `inout_` 前缀

```C++
bool read_config(Config* out_cfg);
```

## 注释

行内使用`//`，不使用`/**/`

正例：

```C++
// BFTP 正在处理连接，此时需要触发 OTA 准备逻辑
LOG_INFO("BFTP connecting, preparing OTA resources...");
// 获取文件信息（由 BFTP 解析 CONNECT_REQ 后填充）
const bftp_file_info_t *file_info = bftp_get_file_info(ctx->bftp);
```

反例：

```C++
/* 更新状态 */
change_state(instance, BFTP_STATE_CONNECTING);
```

函数前使用Doxygen风格注释，说明函数功能、参数、返回值、注意事项等

```C++
/**
 * @brief 发送文件数据
 * @param instance BFTP 实例指针
 * @param data 待发送的数据指针
 * @param len 数据长度
 * @return true 发送成功, false 发送失败
 * @note 通过用户注册的 on_ble_send 回调函数发送
 * @note 如果回调函数未设置,返回 false
 */
static bool send_file_data(bftp_instance_t *instance, const uint8_t *data, uint16_t len)
```

## 例外

当代码中引入了不属于本团队开发的外部库时，外部库的内容无需遵循这些规则，遵循其原有的风格即可。也不要进行格式化，否则在后续涉及库更新时会带来额外的困难。如果外部库是C语言实现的，保持其扩展名为.c，不要更改为.cpp。

## 总结

| 项目           | 命名规则                  | 示例                                |
| :------------- | :------------------------ | :---------------------------------- |
| 文件           | 蛇形命名（全小写+下划线） | `joystick_task.cpp`                 |
| 类名/结构体名  | 大驼峰（单词首字母大写）  | `class JoystickTask {}`             |
| 变量/实例      | 蛇形命名                  | `int16_t target_angle = 90;`        |
| 类成员名       | 蛇形命名 + `_` 结尾       | `int pin_;`                         |
| 宏定义         | 全大写 + 下划线           | `#define LED_TASK_RUNNING_CYCLE 20` |
| 常量           | 蛇形命名                  | `const int16_t max_angle = 180;`    |
| 常量表达式     | 宏规则（全大写+下划线）   | `constexpr uint8_t LED_PIN = 10;`   |
| 枚举名         | 大驼峰                    | `enum class ArmState { Run };`      |
| 枚举值         | 宏规则（全大写+下划线）   | `ZERO_FORCE`                        |
| 函数/类方法    | 蛇形命名                  | `void update_data();`               |
| 命名空间       | 蛇形命名                  | `namespace system_task {};`         |
| 文件内静态变量 | `s_` 前缀                 | `static uint32_t s_last_tick;`      |
| 全局变量       | `g_` 前缀                 | `uint32_t g_system_state;`          |
| 输出参数       | `out_` 前缀               | `Config* out_cfg`                   |
| 输入输出参数   | `inout_` 前缀             | `Config* inout_cfg`                 |

# 语义命名规范

## 布尔元素必须使用明确的前缀

**规则说明：** 布尔变量、函数返回值或函数名本身应能清晰地表达一个可判断真伪的命题。使用 `is_`, `has_`, `should_`, `can_`, `enable_` 等前缀来明确其布尔属性及其具体含义。

| 类型     | 反面例子 (含糊不清)             | 正面例子 (意图明确)                         |
| :------- | :------------------------------ | :------------------------------------------ |
| **变量** | `bool flag;``bool status;`      | `bool is_enabled;``bool has_network_link;`  |
| **函数** | `bool check();``bool device();` | `bool is_ready();``bool can_communicate();` |
| **参数** | `void set_mode(bool mode);`     | `void set_debug_mode(bool enable);`         |

## 函数名必须是动词或动词短语

**规则说明：** 函数代表一个操作或行为，其名称应明确指示它要执行的动作。Getter/Setter函数是此规则的常见特例。

| 类型     | 反面例子 (名词或含义模糊)         | 正面例子 (明确的动作)                                        |
| :------- | :-------------------------------- | :----------------------------------------------------------- |
| 动作函数 | `void data();``void processor();` | `void process_data();``void calculate_sum();`                |
| Setter   | `void brightness(int);`           | `void set_brightness(int);`                                  |
| Getter   | `int brightness();`               | `int get_brightness() const;``// 或者对于bool: bool is_bright() const;` |
| 状态改变 | `void toggle();`                  | `void enable();``void disable();``void toggle_state();`      |

## 类/结构体名必须是名词或名词短语

**规则说明：** 类代表一个实体或概念，其名称应该清楚地表明它“是什么”，而不是“做什么”。

| 反面例子 (动词或含义模糊)                       | 正面例子 (清晰的实体)                                      |
| :---------------------------------------------- | :--------------------------------------------------------- |
| `class Calculate;``class Manager;``class Data;` | `class Calculator;``class TaskManager;``class DataPacket;` |

## 避免使用泛型和模糊的词汇

**规则说明：** 在变量的作用域范围内，名称应尽可能具体，避免使用 `data`, `value`, `info`, `temp`, `obj` 等几乎不包含任何信息的单词。如果不得不使用，必须加上上下文相关的修饰。

| 上下文 | 反面例子 (过于泛化)                               | 正面例子 (具体且有上下文)                                    |
| :----- | :------------------------------------------------ | :----------------------------------------------------------- |
| 函数内 | `int value = Compute();``void* data = GetData();` | `int packet_count = Compute();``SensorData* sensor_readings = GetData();` |
| 类成员 | `std::vector<int> list;``int num;`                | `std::vector<Client> connected_clients_;``int retry_attempt_count_;` |
| 参数   | `void Process(Data data);`                        | `void Process(ImageData image_frame);`                       |

## 使用一致的“对立词”命名

**规则说明：** 对于表示相反含义的变量或函数，使用一组通用的、一致的反义词前缀，这有助于提高可读性和减少错误。

| 反面例子 (不一致)                                            | 正面例子 (一致且清晰)                                        |
| :----------------------------------------------------------- | :----------------------------------------------------------- |
| `bool disable_log = false;``// 含义是“不禁用日志”即启用，非常绕` | `bool enable_log = true;``// 直接表示启用`                   |
| `int min_buffer;``int buffer_max;``// 前缀后缀混用，不一致`  | `size_t min_buffer_size_;``size_t max_buffer_size_;``// 前缀一致` |
| `void start();``void close();``// 反义词不匹配`              | `void start();``void stop();``// 或``void open();``void close();``// 配对使用` |

**常见的反义词对：**

- `begin` / `end`
- `first` / `last`
- `get` / `set`
- `add` / `remove`
- `create` / `destroy`
- `increment` / `decrement`
- `lock` / `unlock`
- `show` / `hide`

# 头文件（`.h / .hpp`）规范

1. **头文件必须使用 include guard 或** **`#pragma once`****（二选一）**

```C++
#pragma once
```

或

```C++
#ifndef LED_TASK_H
#define LED_TASK_H
// ...
#endif
```

1. **头文件中禁止出现以下内容：**

- `using namespace xxx;`
- 非 `inline` 的函数定义
- 非 `constexpr` 的变量定义
- 任何会引入动态分配的对象

1. **头文件只做“声明”，不做“实现”**

```C++
// led_task.h
class LedTask {
public:
    void init();
    void run();
};
// led_task.cpp
void LedTask::init() { ... }
```

# C++特性使用规范

> 原则：
>
> - **不引入动态分配、不依赖复杂****泛型****、不增加隐藏运行时机制**
> - **尽量做到：编译期确定、行为可预测、容易被 C 工程师理解**

## 推荐使用的 C++ 特性

### 命名空间（`namespace`）

**作用**

- 解决全局符号污染（比 C 的前缀命名更清晰）

**嵌入式****价值**

- 零运行时开销
- 非常适合驱动层 / 模块层划分

```C++
namespace drv {
    void init();
}
```

### 强类型枚举（`enum class`）

**作用**

- 枚举不再隐式转换为整数
- 有作用域，避免名字冲突

```C++
enum class PowerMode : uint8_t {
    Off,
    Standby,
    Active
};
```

**相比 C 的好处**

- 防止把错误的枚举传给函数
- 编译期即可发现错误

### `constexpr`（编译期常量/函数）

**作用**

- 用类型安全的方式替代 `#define`

```C++
constexpr uint32_t UART_BAUD = 115200;
```

**嵌入式****价值**

- 编译期计算
- 无运行时成本
- 可被调试器识别

### `static_assert`

**作用**

- 编译期做约束检查

```C++
static_assert(sizeof(Packet) == 8, "Protocol size mismatch");
```

**嵌入式****价值**

- 防止协议、Flash 布局、结构体变化导致隐性 Bug

### 类（Class）+ 构造函数 / 析构函数（RAII，限资源类）

> ⚠️ 不等于“面向对象全家桶”

**推荐用法**

- 外设、资源、状态的**生命周期管理**

```C++
class Led {
public:
    explicit Led(int pin) : pin_(pin) { gpio_init(pin_); }
    ~Led() { gpio_deinit(pin_); }
private:
    int pin_;
};
```

**相比 C 的好处**

- 初始化和释放不再依赖“调用顺序”
- 减少遗漏 `deinit()` 的风险

### `std::array`（固定长度容器）

**作用**

- 类型安全的“数组替代品”

```C++
std::array<uint8_t, 16> rx_buf;
```

**相比 C 数组**

- 知道自己的大小
- 可传引用，避免长度参数错误

### `auto`（有限使用）

**建议范围**

- 右值类型显而易见时使用
- 避免用于接口参数、返回值

```C++
auto len = rx_buf.size();
```

## 可使用但需明确约束的特性

### `virtual` / 继承

**问题**

- 虚表在 Flash
- 运行时多一次间接调用
- 中断 / Cache 关闭场景危险

**建议**

- 大量 / 深层 / 高频调用的虚函数：需要谨慎
- 接口抽象时可以使用

### `std::string`

**问题**

- 动态内存
- 不可控扩容

**建议**

- 仅允许被动使用（对接第三方库）
- 产品代码优先用：
  - `char[]`
  - `std::array<char, N>`

## 禁止使用的特性

### 模板

**原因**

- 学习曲线陡峭
- 代码体积膨胀
- 编译错误难理解

### `new / delete`

**原因**

- 堆碎片
- 不可预测延迟

### 智能指针

**原因**

- 依赖动态内存
- RAII 在无堆设计中意义有限

### Lambda 表达式

**原因**

- 无捕获 lambda 功能有限
- 捕获 lambda 依赖模板与闭包
- 学习成本与收益不成正比

### 异常（Exception）

**原因**

- 代码体积大
- 栈消耗不可控
- ESP-IDF 默认关闭

### RTTI / `dynamic_cast`

**原因**

- 固件体积明显增加
- 运行时类型判断违背“静态设计”原则

### `iostream`

**原因**

- 一个头文件就能让固件膨胀 100KB+

# 参考资料

[7. 命名约定 — Google 开源项目风格指南](https://zh-google-styleguide.readthedocs.io/en/latest/google-cpp-styleguide/naming.html#)

华为代码规范.pdf

[GitHub - RoboMaster/Development-Board-C-Examples](https://github.com/RoboMaster/Development-Board-C-Examples)

[Modern C++ in Embedded: How Far Can You Push It? - RunTime Recruitment](https://runtimerec.com/modern-c-in-embedded-how-far-can-you-push-it/)

[C++ On Embedded Systems](https://blog.mbedded.ninja/programming/languages/c-plus-plus/cpp-on-embedded-systems/)

[C++ 支持 - ESP32 - — ESP-IDF 编程指南 v5.5.1 文档](https://docs.espressif.com/projects/esp-idf/zh_CN/v5.5.1/esp32/api-guides/cplusplus.html)