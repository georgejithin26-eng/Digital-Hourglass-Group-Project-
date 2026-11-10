#include "Arduino.h"
#include "LedControl.h"
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// 前置声明
int getGravity();
void resetTime();

#define MATRIX_A 1
#define MATRIX_B 0

#define ACC_THRESHOLD 0.5

// LED 点阵
#define PIN_DATAIN 5
#define PIN_CLK 4
#define PIN_LOAD 6

// 蜂鸣器
#define PIN_BUZZER 14

// D7：暂停/继续；D8：重启
#define PIN_BUTTON 7
#define PIN_RESTART 8

// OLED
#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_RESET_PIN -1
#define OLED_I2C_ADDRESS 0x3C

#define ROTATION_OFFSET 90
#define DELAY_FRAME 100
#define BUTTON_DEBOUNCE 50
#define DEBUG_OUTPUT 1

// 固定一分钟，60 颗沙粒
const unsigned long FIXED_DURATION_MS = 60000UL;
const int TOTAL_PARTICLES = 60;

uint8_t selectedMinutes = 1;
uint8_t selectedSeconds = 0;
bool timeConfirmed = true;

unsigned long selectedDurationMs = FIXED_DURATION_MS;
unsigned long timerDeadline = 0;
unsigned long remainingTime = 0;

// 重要：记录成功跨矩阵转移的次数
int completedDrops = 0;

int gravity;
int resetCounter = 0;
bool alarmWentOff = false;
bool paused = false;

LedControl lc = LedControl(PIN_DATAIN, PIN_CLK, PIN_LOAD, 2);

Adafruit_MPU6050 mpu;
Adafruit_SSD1306 oled(
  OLED_WIDTH,
  OLED_HEIGHT,
  &Wire,
  OLED_RESET_PIN
);

bool oledReady = false;

// OLED 刷新缓存
long lastDisplayedSeconds = -1;
bool lastDisplayedPaused = false;
bool hasDisplayedRuntimeScreen = false;

// D7 消抖
bool buttonLastReading = HIGH;
bool buttonStableState = HIGH;
unsigned long buttonLastChangeTime = 0;

// D8 消抖
bool restartLastReading = HIGH;
bool restartStableState = HIGH;
unsigned long restartLastChangeTime = 0;


// =====================================================
// 计时参数
// =====================================================

unsigned long getSelectedDurationMs() {
  return FIXED_DURATION_MS;
}

// 每颗沙粒对应 1 秒
unsigned long getDropIntervalMs() {
  return FIXED_DURATION_MS / TOTAL_PARTICLES;
}

// 剩余时间完全跟随成功转移的沙粒数量
unsigned long getCountdownRemainingMs() {
  int remainingParticles = TOTAL_PARTICLES - completedDrops;

  if (remainingParticles < 0) {
    remainingParticles = 0;
  }

  return (unsigned long)remainingParticles * getDropIntervalMs();
}


// =====================================================
// OLED 显示
// =====================================================

void printTwoDigits(uint8_t value) {
  if (value < 10) {
    oled.print('0');
  }

  oled.print(value);
}

void printMMSS(unsigned long totalSeconds) {
  unsigned long minutes = totalSeconds / 60UL;
  uint8_t seconds = totalSeconds % 60UL;

  if (minutes < 10) {
    oled.print('0');
  }

  oled.print(minutes);
  oled.print(':');
  printTwoDigits(seconds);
}

void renderRunningScreen(bool forceRefresh) {
  if (!oledReady || !timeConfirmed) {
    return;
  }

  // 不再使用 millis() 直接递减显示时间
  // 只有成功转移沙粒，剩余时间才减少
  unsigned long remainingSeconds =
    (getCountdownRemainingMs() + 999UL) / 1000UL;

  if (
    !forceRefresh &&
    hasDisplayedRuntimeScreen &&
    lastDisplayedSeconds == (long)remainingSeconds &&
    lastDisplayedPaused == paused
  ) {
    return;
  }

  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);

  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print("HOURGLASS COUNTDOWN");

  oled.setTextSize(3);
  oled.setCursor(14, 17);
  printMMSS(remainingSeconds);

  oled.setTextSize(1);
  oled.setCursor(0, 45);
  oled.print(paused ? "PAUSED" : "RUNNING");

  oled.setCursor(0, 56);
  oled.print("D7:PAUSE D8:RESET");

  oled.display();

  lastDisplayedSeconds = (long)remainingSeconds;
  lastDisplayedPaused = paused;
  hasDisplayedRuntimeScreen = true;
}

void waitForNextFrame() {
  unsigned long frameMs = DELAY_FRAME;
  unsigned long interval = getDropIntervalMs();

  if (interval < frameMs) {
    frameMs = interval;
  }

  if (frameMs < 5UL) {
    frameMs = 5UL;
  }

  delay(frameMs);
}


// =====================================================
// D7 按钮：暂停 / 继续
// =====================================================

bool buttonClicked() {
  bool reading = digitalRead(PIN_BUTTON);

  if (reading != buttonLastReading) {
    buttonLastChangeTime = millis();
    buttonLastReading = reading;
  }

  if (
    millis() - buttonLastChangeTime >
    BUTTON_DEBOUNCE
  ) {
    if (reading != buttonStableState) {
      buttonStableState = reading;

      // INPUT_PULLUP：LOW 表示按下
      if (buttonStableState == LOW) {
        return true;
      }
    }
  }

  return false;
}


// =====================================================
// D8 按钮：重启
// =====================================================

bool restartButtonClicked() {
  bool reading = digitalRead(PIN_RESTART);

  if (reading != restartLastReading) {
    restartLastChangeTime = millis();
    restartLastReading = reading;
  }

  if (
    millis() - restartLastChangeTime >
    BUTTON_DEBOUNCE
  ) {
    if (reading != restartStableState) {
      restartStableState = reading;

      if (restartStableState == LOW) {
        return true;
      }
    }
  }

  return false;
}


// =====================================================
// 按钮处理
// =====================================================

void handleButton() {

  // D8 优先：重置整个沙漏
  if (restartButtonClicked()) {

#if DEBUG_OUTPUT
    Serial.println("TIMER RESTART");
#endif

    gravity = getGravity();

    lc.setRotation(
      (ROTATION_OFFSET + gravity) % 360
    );

    resetTime();
    renderRunningScreen(true);

    return;
  }

  if (!timeConfirmed) {
    return;
  }

  if (!buttonClicked()) {
    return;
  }

  // 正在运行 -> 暂停
  if (!paused) {

    unsigned long now = millis();

    if ((long)(timerDeadline - now) > 0) {
      remainingTime = timerDeadline - now;
    } else {
      remainingTime = 0;
    }

    paused = true;

    renderRunningScreen(true);

#if DEBUG_OUTPUT
    Serial.println("TIMER PAUSED");
    Serial.print("Successful drops: ");
    Serial.println(completedDrops);
#endif
  }

  // 暂停 -> 继续
  else {

    unsigned long now = millis();

    timerDeadline = now + remainingTime;

    paused = false;

    renderRunningScreen(true);

#if DEBUG_OUTPUT
    Serial.println("TIMER RESUMED");
    Serial.print("Successful drops: ");
    Serial.println(completedDrops);
#endif
  }
}


// =====================================================
// 沙粒坐标及移动
// =====================================================

coord getDown(int x, int y) {
  coord xy;
  xy.x = x - 1;
  xy.y = y + 1;
  return xy;
}

coord getLeft(int x, int y) {
  coord xy;
  xy.x = x - 1;
  xy.y = y;
  return xy;
}

coord getRight(int x, int y) {
  coord xy;
  xy.x = x;
  xy.y = y + 1;
  return xy;
}

bool canGoLeft(int addr, int x, int y) {
  if (x == 0) {
    return false;
  }

  return !lc.getXY(addr, getLeft(x, y));
}

bool canGoRight(int addr, int x, int y) {
  if (y == 7) {
    return false;
  }

  return !lc.getXY(addr, getRight(x, y));
}

bool canGoDown(int addr, int x, int y) {
  if (y == 7 || x == 0) {
    return false;
  }

  if (!canGoLeft(addr, x, y)) {
    return false;
  }

  if (!canGoRight(addr, x, y)) {
    return false;
  }

  return !lc.getXY(addr, getDown(x, y));
}

void goDown(int addr, int x, int y) {
  lc.setXY(addr, x, y, false);
  lc.setXY(addr, getDown(x, y), true);
}

void goLeft(int addr, int x, int y) {
  lc.setXY(addr, x, y, false);
  lc.setXY(addr, getLeft(x, y), true);
}

void goRight(int addr, int x, int y) {
  lc.setXY(addr, x, y, false);
  lc.setXY(addr, getRight(x, y), true);
}

int countParticles(int addr) {
  int c = 0;

  for (byte y = 0; y < 8; y++) {
    for (byte x = 0; x < 8; x++) {
      if (lc.getXY(addr, x, y)) {
        c++;
      }
    }
  }

  return c;
}

bool moveParticle(int addr, int x, int y) {
  if (!lc.getXY(addr, x, y)) {
    return false;
  }

  bool can_GoLeft = canGoLeft(addr, x, y);
  bool can_GoRight = canGoRight(addr, x, y);

  if (!can_GoLeft && !can_GoRight) {
    return false;
  }

  bool can_GoDown = canGoDown(addr, x, y);

  if (can_GoDown) {
    goDown(addr, x, y);
  }
  else if (can_GoLeft && !can_GoRight) {
    goLeft(addr, x, y);
  }
  else if (can_GoRight && !can_GoLeft) {
    goRight(addr, x, y);
  }
  else if (random(2) == 1) {
    goLeft(addr, x, y);
  }
  else {
    goRight(addr, x, y);
  }

  return true;
}


// =====================================================
// 初始装填沙粒
// =====================================================

void fill(int addr, int maxcount) {
  int n = 8;
  byte x, y;
  int count = 0;

  for (byte slice = 0; slice < 2 * n - 1; ++slice) {

    byte z = slice < n ? 0 : slice - n + 1;

    for (byte j = z; j <= slice - z; ++j) {

      y = 7 - j;
      x = slice - j;

      lc.setXY(
        addr,
        x,
        y,
        (++count <= maxcount)
      );
    }
  }
}


// =====================================================
// MPU6050 方向判断
// =====================================================

int getGravity() {
  sensors_event_t a, g, temp;

  mpu.getEvent(&a, &g, &temp);

  float ax = a.acceleration.x;
  float ay = a.acceleration.y;

  float threshold = ACC_THRESHOLD * 9.81;

  if (ay < -threshold) {
    return 0;
  }

  if (ax > threshold) {
    return 90;
  }

  if (ay > threshold) {
    return 180;
  }

  if (ax < -threshold) {
    return 270;
  }

  // 未达到阈值时保留原方向
  return gravity;
}

int getTopMatrix() {
  return (gravity == 90) ? MATRIX_A : MATRIX_B;
}

int getBottomMatrix() {
  return (gravity != 90) ? MATRIX_A : MATRIX_B;
}


// =====================================================
// 重置整个沙漏和倒计时
// =====================================================

void resetTime() {

  // 清空两个点阵
  for (byte i = 0; i < 2; i++) {
    lc.clearDisplay(i);
  }

  // 60 颗沙粒全部放到上方
  fill(getTopMatrix(), TOTAL_PARTICLES);

  // 清零已完成的沙粒转移次数
  completedDrops = 0;

  // 设置下一次允许转移的时间
  unsigned long now = millis();

  selectedDurationMs = getSelectedDurationMs();
  timerDeadline = now + getDropIntervalMs();

  // 取消暂停
  paused = false;
  remainingTime = 0;

  // 清除报警状态
  alarmWentOff = false;

  // 强制刷新 OLED
  lastDisplayedSeconds = -1;
  lastDisplayedPaused = false;
  hasDisplayedRuntimeScreen = false;
}


// =====================================================
// 更新两个点阵内部的沙粒
// =====================================================

bool updateMatrix() {
  int n = 8;
  bool somethingMoved = false;

  byte x, y;
  bool direction;

  for (byte slice = 0; slice < 2 * n - 1; ++slice) {

    direction = (random(2) == 1);

    byte z = slice < n ? 0 : slice - n + 1;

    for (byte j = z; j <= slice - z; ++j) {

      y = direction ? (7 - j) : (7 - (slice - j));
      x = direction ? (slice - j) : j;

      if (moveParticle(MATRIX_B, x, y)) {
        somethingMoved = true;
      }

      if (moveParticle(MATRIX_A, x, y)) {
        somethingMoved = true;
      }
    }
  }

  return somethingMoved;
}


// =====================================================
// 跨矩阵转移沙粒
// 关键修改：成功转移后才更新倒计时
// =====================================================

boolean dropParticle() {

  if (paused || !timeConfirmed) {
    return false;
  }

  // 已完成全部转移，不再继续计数
  if (completedDrops >= TOTAL_PARTICLES) {
    return false;
  }

  unsigned long now = millis();

  // 尚未到达下一次尝试时间
  if ((long)(now - timerDeadline) < 0) {
    return false;
  }

  // 保留原来的方向逻辑
  if (gravity != 0 && gravity != 180) {
    return false;
  }

  bool aPixel = lc.getRawXY(MATRIX_A, 0, 0);
  bool bPixel = lc.getRawXY(MATRIX_B, 7, 7);

  // 两个位置状态不一致，才允许发生一次转移
  if (aPixel != bPixel) {

    lc.invertRawXY(MATRIX_A, 0, 0);
    lc.invertRawXY(MATRIX_B, 7, 7);

    // 只有发生了真实的跨矩阵状态变化，
    // 才增加完成次数并推进 OLED 倒计时。
    completedDrops++;

    // 下一次转移从当前时刻开始计算间隔
    timerDeadline = millis() + getDropIntervalMs();

    // 蜂鸣器短提示音
    tone(PIN_BUZZER, 440, 10);

#if DEBUG_OUTPUT
    Serial.print("Successful transfer: ");
    Serial.print(completedDrops);
    Serial.print("/");
    Serial.println(TOTAL_PARTICLES);
#endif

    return true;
  }

  // 当前没有可转移的沙粒：
  // 不增加次数，不减少 OLED 时间。
  // 下一轮继续检查，直到条件满足。
  return false;
}


// =====================================================
// 结束报警
// =====================================================

void alarm() {
  for (int i = 0; i < 5; i++) {
    tone(PIN_BUZZER, 440, 200);
    delay(1000);
  }
}


// =====================================================
// 初始化
// =====================================================

void setup() {

  Serial.begin(9600);

  randomSeed(analogRead(A0));

  // 按钮使用内部上拉
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_RESTART, INPUT_PULLUP);

  // I2C
  Wire.begin();

  // OLED 初始化
  oledReady = oled.begin(
    SSD1306_SWITCHCAPVCC,
    OLED_I2C_ADDRESS
  );

  if (oledReady) {
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(1);
    oled.setCursor(0, 0);
    oled.println("LED HOURGLASS");
    oled.println("Fixed 1-minute timer");
    oled.display();
  }
  else {
    Serial.println(
      "OLED not found at 0x3C. Check OLED type/address."
    );
  }

  // MPU6050 初始化
  if (!mpu.begin()) {
    Serial.println(
      "MPU6050 not found! Check wiring."
    );

    while (1) {
      delay(10);
    }
  }

  mpu.setAccelerometerRange(MPU6050_RANGE_2_G);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);

  gravity = getGravity();

  // LED 点阵初始化
  for (byte i = 0; i < 2; i++) {
    lc.shutdown(i, false);
    lc.setIntensity(i, 1);
    lc.clearDisplay(i);
  }

  // 自动启动固定一分钟倒计时
  selectedMinutes = 1;
  selectedSeconds = 0;
  selectedDurationMs = FIXED_DURATION_MS;
  timeConfirmed = true;

  resetTime();
  renderRunningScreen(true);
}


// =====================================================
// 主循环
// =====================================================

void loop() {

  // 1. 检查 D7 和 D8
  handleButton();

  // 2. 控制程序循环间隔
  waitForNextFrame();

  // 3. 更新方向和点阵旋转
  gravity = getGravity();

  lc.setRotation(
    (ROTATION_OFFSET + gravity) % 360
  );

  // 4. 允许沙粒在各自点阵内部运动
  bool moved = updateMatrix();

  // 5. 尝试跨矩阵转移
  bool dropped = false;

  if (!paused) {
    dropped = dropParticle();
  }

  // 6. OLED 根据已成功转移的沙粒数量刷新
  // 没有成功转移时，时间保持不变
  renderRunningScreen(false);

  // 7. 结束报警
  if (
    !paused &&
    !moved &&
    !dropped &&
    !alarmWentOff &&
    countParticles(getTopMatrix()) == 0
  ) {
    alarmWentOff = true;

    renderRunningScreen(true);
    alarm();
  }

  // 8. 成功转移后解除报警锁定
  if (dropped) {
    alarmWentOff = false;
  }
}