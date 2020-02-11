/*
  Скетч к проекту "Многофункциональный RGB светильник"
  Страница проекта (схемы, описания): https://alexgyver.ru/GyverLamp/
  Исходники на GitHub: https://github.com/AlexGyver/GyverLamp/
  Нравится, как написан код? Поддержи автора! https://alexgyver.ru/support_alex/
  Автор: AlexGyver, AlexGyver Technologies, 2019
  https://AlexGyver.ru/
*/


// Ссылка для менеджера плат:
// https://arduino.esp8266.com/stable/package_esp8266com_index.json

#include "pgmspace.h"
#include "Constants.h"
#include <FastLED.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include "CaptivePortalManager.h"
#include <WiFiUdp.h>
#include <EEPROM.h>
#include "Types.h"
#include "timerMinim.h"
#ifdef ESP_USE_BUTTON
#include <GyverButton.h>
#endif
#include "fonts.h"
#ifdef USE_NTP
#include <NTPClient.h>
#include <Timezone.h>
#endif
#include <TimeLib.h>
#ifdef OTA
#include "OtaManager.h"
#endif
#if USE_MQTT
#include "MqttManager.h"
#endif
#include "TimerManager.h"
#include "FavoritesManager.h"
#include "EepromManager.h"

// --- ИНИЦИАЛИЗАЦИЯ ОБЪЕКТОВ ----------
CRGB leds[NUM_LEDS];
WiFiManager wifiManager;
WiFiServer wifiServer(ESP_HTTP_PORT);
WiFiUDP Udp;

#ifdef USE_NTP
WiFiUDP ntpUDP;
// объект, запрашивающий время с ntp сервера;
// в нём смещение часового пояса не используется (перенесено в объект localTimeZone);
// здесь всегда должно быть время UTC

NTPClient timeClient(ntpUDP, NTP_ADDRESS, 0, NTP_INTERVAL);
#ifdef SUMMER_WINTER_TIME
TimeChangeRule summerTime = {SUMMER_TIMEZONE_NAME, SUMMER_WEEK_NUM, SUMMER_WEEKDAY, SUMMER_MONTH, SUMMER_HOUR, SUMMER_OFFSET};
TimeChangeRule winterTime = {WINTER_TIMEZONE_NAME, WINTER_WEEK_NUM, WINTER_WEEKDAY, WINTER_MONTH, WINTER_HOUR, WINTER_OFFSET};
Timezone localTimeZone(summerTime, winterTime);
#else
TimeChangeRule localTime = {LOCAL_TIMEZONE_NAME, LOCAL_WEEK_NUM, LOCAL_WEEKDAY, LOCAL_MONTH, LOCAL_HOUR, LOCAL_OFFSET};
Timezone localTimeZone(localTime);
#endif
#endif

timerMinim timeTimer(3000);
bool ntpServerAddressResolved = false;
bool timeSynched = false;
uint32_t lastTimePrinted = 0U;

#ifdef ESP_USE_BUTTON
GButton touch(BTN_PIN, LOW_PULL, NORM_OPEN);
#endif

#ifdef OTA
OtaManager otaManager(&showWarning);
OtaPhase OtaManager::OtaFlag = OtaPhase::None;
#endif

#if USE_MQTT
AsyncMqttClient *mqttClient = NULL;
AsyncMqttClient *MqttManager::mqttClient = NULL;
char *MqttManager::mqttServer = NULL;
char *MqttManager::mqttUser = NULL;
char *MqttManager::mqttPassword = NULL;
char *MqttManager::clientId = NULL;
char *MqttManager::lampInputBuffer = NULL;
char *MqttManager::topicInput = NULL;
char *MqttManager::topicOutput = NULL;
bool MqttManager::needToPublish = false;
char MqttManager::mqttBuffer[] = {};
uint32_t MqttManager::mqttLastConnectingAttempt = 0;
SendCurrentDelegate MqttManager::sendCurrentDelegate = NULL;
#endif

// --- ИНИЦИАЛИЗАЦИЯ ПЕРЕМЕННЫХ -------
uint16_t localPort = ESP_UDP_PORT;
// buffer to hold incoming packet
char packetBuffer[MAX_UDP_BUFFER_SIZE];
char inputBuffer[MAX_UDP_BUFFER_SIZE];
static const uint8_t maxDim = max(WIDTH, HEIGHT);

ModeType modes[MODE_AMOUNT];
AlarmType alarms[7];

// опции для выпадающего списка параметра "время перед 'рассветом'" (будильник);
// синхронизировано с android приложением
static const uint8_t dawnOffsets[] PROGMEM = {5, 10, 15, 20, 25, 30, 40, 50, 60};
uint8_t dawnMode;
bool dawnFlag = false;
uint32_t thisTime;
bool manualOff = false;

int8_t currentMode = 0;
bool loadingFlag = true;
bool ONflag = false;
uint32_t eepromTimeout;
bool settChanged = false;
bool buttonEnabled = true;

unsigned char matrixValue[8][16];

bool TimerManager::TimerRunning = false;
bool TimerManager::TimerHasFired = false;
uint8_t TimerManager::TimerOption = 1U;
uint64_t TimerManager::TimeToFire = 0ULL;

uint8_t FavoritesManager::FavoritesRunning = 0;
uint16_t FavoritesManager::Interval = DEFAULT_FAVORITES_INTERVAL;
uint16_t FavoritesManager::Dispersion = DEFAULT_FAVORITES_DISPERSION;
uint8_t FavoritesManager::UseSavedFavoritesRunning = 0;
uint8_t FavoritesManager::FavoriteModes[MODE_AMOUNT] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
uint32_t FavoritesManager::nextModeAt = 0UL;

bool CaptivePortalManager::captivePortalCalled = false;

void setup()
{
  Serial.begin(115200);
  Serial.println();
  ESP.wdtEnable(WDTO_8S);

// ПИНЫ
// инициализация пина, управляющего MOSFET транзистором в состояние "выключен"
#ifdef MOSFET_PIN
  pinMode(MOSFET_PIN, OUTPUT);
#ifdef MOSFET_LEVEL
  digitalWrite(MOSFET_PIN, !MOSFET_LEVEL);
#endif
#endif

// инициализация пина, управляющего будильником в состояние "выключен"
#ifdef ALARM_PIN
  pinMode(ALARM_PIN, OUTPUT);
#ifdef ALARM_LEVEL
  digitalWrite(ALARM_PIN, !ALARM_LEVEL);
#endif
#endif

// TELNET
#if defined(GENERAL_DEBUG) && GENERAL_DEBUG_TELNET
  telnetServer.begin();
  // пауза 10 секунд в отладочном режиме,
  // чтобы успеть подключиться по протоколу telnet до вывода первых сообщений
  for (uint8_t i = 0; i < 100; i++)
  {
    handleTelnetClient();
    delay(100);
    ESP.wdtFeed();
  }
#endif

// КНОПКА
#if defined(ESP_USE_BUTTON)
  touch.setStepTimeout(BUTTON_STEP_TIMEOUT);
  touch.setClickTimeout(BUTTON_CLICK_TIMEOUT);
#if ESP_RESET_ON_START
  // ожидание инициализации модуля кнопки ttp223 (по спецификации 250мс)
  delay(1000);
  if (digitalRead(BTN_PIN))
  {
    // сброс сохранённых SSID и пароля при старте с зажатой кнопкой, если разрешено
    wifiManager.resetSettings();
    LOG.println(F("Настройки WiFiManager сброшены"));
  }
  // при сбросе параметров WiFi сразу после старта с зажатой кнопкой,
  // также разблокируется кнопка, если была заблокирована раньше
  buttonEnabled = true;
  EepromManager::SaveButtonEnabled(&buttonEnabled);
  ESP.wdtFeed();
#endif
#endif

  // ЛЕНТА/МАТРИЦА
  FastLED.addLeds<WS2812B, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS) /*.setCorrection(TypicalLEDStrip)*/;
  FastLED.setBrightness(BRIGHTNESS);
  if (CURRENT_LIMIT > 0)
  {
    FastLED.setMaxPowerInVoltsAndMilliamps(5, CURRENT_LIMIT);
  }
  FastLED.clear();
  FastLED.show();

  // EEPROM
  // инициализация EEPROM;
  // запись начального состояния настроек,
  // если их там ещё нет; инициализация настроек лампы значениями из EEPROM
  EepromManager::InitEepromSettings(
      modes, alarms, &espMode, &ONflag, &dawnMode, &currentMode, &buttonEnabled,
      &(FavoritesManager::ReadFavoritesFromEeprom),
      &(FavoritesManager::SaveFavoritesToEeprom));
  LOG.printf_P(PSTR("Рабочий режим лампы: ESP_MODE = %d\n"), espMode);

  // WI-FI
  // вывод отладочных сообщений
  wifiManager.setDebugOutput(WIFIMAN_DEBUG);
  // установка минимально приемлемого уровня сигнала WiFi сетей (8% по умолчанию)
  // wifiManager.setMinimumSignalQuality();
  CaptivePortalManager *captivePortalManager = new CaptivePortalManager(&wifiManager);
  // режим WiFi точки доступа
  if (espMode == 0U)
  {
    // wifiManager.setConfigPortalBlocking(false);

    if (sizeof(AP_STATIC_IP))
    {
      LOG.println(F("Используется статический IP адрес WiFi точки доступа"));
      // wifiManager.startConfigPortal использовать нельзя,
      // т.к. он блокирует вычислительный процесс внутри себя,
      // а затем перезагружает ESP, т.е. предназначен только для ввода SSID и пароля
      wifiManager.setAPStaticIPConfig(
          // IP адрес WiFi точки доступа
          IPAddress(AP_STATIC_IP[0], AP_STATIC_IP[1], AP_STATIC_IP[2], AP_STATIC_IP[3]),
          // первый доступный IP адрес сети
          IPAddress(AP_STATIC_IP[0], AP_STATIC_IP[1], AP_STATIC_IP[2], 1),
          // маска подсети
          IPAddress(255, 255, 255, 0));
    }

    WiFi.softAP(AP_NAME, AP_PASS);

    LOG.println(F("Старт в режиме WiFi точки доступа"));
    LOG.print(F("IP адрес: "));
    LOG.println(WiFi.softAPIP());

    wifiServer.begin();
  }
  // режим WiFi клиента (подключаемся к роутеру,
  // если есть сохранённые SSID и пароль,
  // иначе создаём WiFi точку доступа и запрашиваем их)
  else
  {
    LOG.println(F("Старт в режиме WiFi клиента (подключение к роутеру)"));

    if (WiFi.SSID().length())
    {
      LOG.printf_P(PSTR("Подключение к WiFi сети: %s\n"), WiFi.SSID().c_str());

      // ВНИМАНИЕ: настраивать статический ip WiFi клиента
      // можно только при уже сохранённых имени и пароле WiFi сети
      // (иначе проявляется несовместимость библиотек WiFiManager и WiFi)
      if (sizeof(STA_STATIC_IP))
      {
        LOG.print(F("Сконфигурирован статический IP адрес: "));
        LOG.printf_P(PSTR("%u.%u.%u.%u\n"), STA_STATIC_IP[0], STA_STATIC_IP[1], STA_STATIC_IP[2], STA_STATIC_IP[3]);
        wifiManager.setSTAStaticIPConfig(
            // статический IP адрес ESP в режиме WiFi клиента
            IPAddress(STA_STATIC_IP[0], STA_STATIC_IP[1], STA_STATIC_IP[2], STA_STATIC_IP[3]),
            // первый доступный IP адрес сети
            //(справедливо для 99,99% случаев;
            //для сетей меньше чем на 255 адресов нужно вынести в константы)
            IPAddress(STA_STATIC_IP[0], STA_STATIC_IP[1], STA_STATIC_IP[2], 1),
            // маска подсети
            // (справедливо для 99,99% случаев;
            // для сетей меньше чем на 255 адресов нужно вынести в константы)
            IPAddress(255, 255, 255, 0));
      }
    }
    else
    {
      LOG.println(F("WiFi сеть не определена, запуск WiFi точки доступа для настройки параметров подключения к WiFi сети..."));
      CaptivePortalManager::captivePortalCalled = true;
      // перезагрузка после ввода и сохранения имени и пароля WiFi сети
      wifiManager.setBreakAfterConfig(true);
      // мигание жёлтым цветом 0,5 секунды (1 раз) - нужно ввести параметры WiFi сети для подключения
      showWarning(CRGB::Yellow, 1000U, 500U);
    }

    // установка времени ожидания подключения к WiFi сети, затем старт WiFi точки доступа
    wifiManager.setConnectTimeout(ESP_CONN_TIMEOUT);
    // установка времени работы WiFi точки доступа, затем перезагрузка; отключить watchdog?
    wifiManager.setConfigPortalTimeout(ESP_CONF_TIMEOUT);
    // пытаемся подключиться к сохранённой ранее WiFi сети;
    // в случае ошибки, будет развёрнута WiFi точка доступа с
    // указанными AP_NAME и паролем на время ESP_CONN_TIMEOUT секунд;
    // http://AP_STATIC_IP:ESP_HTTP_PORT (обычно http://192.168.0.1:80)
    // - страница для ввода SSID и пароля от WiFi сети роутера
    wifiManager.autoConnect(AP_NAME, AP_PASS);

    delete captivePortalManager;
    captivePortalManager = NULL;

    // подключение к WiFi не установлено
    if (WiFi.status() != WL_CONNECTED)
    {
      // была показана страница настройки WiFi ...
      if (CaptivePortalManager::captivePortalCalled)
      {
        // пользователь ввёл некорректное имя WiFi сети и/или пароль
        //или запрошенная WiFi сеть недоступна
        if (millis() < (ESP_CONN_TIMEOUT + ESP_CONF_TIMEOUT) * 1000U)
        {
          LOG.println(F("Не удалось подключиться к WiFi сети\nУбедитесь в корректности имени WiFi сети и пароля\nРестарт для запроса нового имени WiFi сети и пароля...\n"));
          wifiManager.resetSettings();
        }
        // пользователь не вводил имя WiFi сети и пароль
        else
        {
          LOG.println(F("Время ожидания ввода SSID и пароля от WiFi сети или подключения к WiFi сети превышено\nЛампа будет перезагружена в режиме WiFi точки доступа!\n"));
          espMode = (espMode == 0U) ? 1U : 0U;
          EepromManager::SaveEspMode(&espMode);

          LOG.printf_P(PSTR("Рабочий режим лампы изменён и сохранён в энергонезависимую память\nНовый рабочий режим: ESP_MODE = %d, %s\nРестарт...\n"),
                       espMode, espMode == 0U ? F("WiFi точка доступа") : F("WiFi клиент (подключение к роутеру)"));
        }
      }
      // страница настройки WiFi не была показана,
      // не удалось подключиться к ранее сохранённой WiFi сети (перенос в новую WiFi сеть)
      else
      {
        LOG.println(F("Не удалось подключиться к WiFi сети\nВозможно, заданная WiFi сеть больше не доступна\nРестарт для запроса нового имени WiFi сети и пароля...\n"));
        wifiManager.resetSettings();
      }

      // мигание красным цветом 0,5 секунды (1 раз)
      // ожидание ввода SSID'а и пароля WiFi сети прекращено,
      // перезагрузка
      showWarning(CRGB::Red, 1000U, 500U);
      ESP.restart();
    }

    // первое подключение к WiFi сети после настройки параметров WiFi
    // на странице настройки - нужна перезагрузка для применения статического IP
    if (CaptivePortalManager::captivePortalCalled &&
        sizeof(STA_STATIC_IP) &&
        WiFi.localIP() != IPAddress(STA_STATIC_IP[0], STA_STATIC_IP[1], STA_STATIC_IP[2], STA_STATIC_IP[3]))
    {
      LOG.println(F("Рестарт для применения заданного статического IP адреса..."));
      delay(100);
      ESP.restart();
    }

    LOG.print(F("IP адрес: "));
    LOG.println(WiFi.localIP());
  }
  ESP.wdtFeed();

  LOG.printf_P(PSTR("Порт UDP сервера: %u\n"), localPort);
  Udp.begin(localPort);

// NTP
#ifdef USE_NTP
  timeClient.begin();
  ESP.wdtFeed();
#endif

// MQTT
#if (USE_MQTT)
  if (espMode == 1U)
  {
    mqttClient = new AsyncMqttClient();
    // создание экземпляров объектов для работы с MQTT,
    // их инициализация и подключение к MQTT брокеру
    MqttManager::setupMqtt(mqttClient, inputBuffer, &sendCurrent);
  }
  ESP.wdtFeed();
#endif

  // ОСТАЛЬНОЕ
  memset(matrixValue, 0, sizeof(matrixValue));
  randomSeed(micros());
  changePower();
  loadingFlag = true;
}

void loop()
{
  parseUDP();
  effectsTick();

  EepromManager::HandleEepromTick(&settChanged, &eepromTimeout, &ONflag,
                                  &currentMode, modes, &(FavoritesManager::SaveFavoritesToEeprom));

#ifdef USE_NTP
  timeTick();
#endif

#ifdef ESP_USE_BUTTON
  if (buttonEnabled)
  {
    buttonTick();
  }
#endif

#ifdef OTA
  // ожидание и обработка команды на обновление прошивки по воздуху
  otaManager.HandleOtaUpdate();
#endif

  // обработка событий таймера отключения лампы
  TimerManager::HandleTimer(&ONflag, &settChanged,
                            &eepromTimeout, &changePower);

  // обработка режима избранных эффектов
  if (FavoritesManager::HandleFavorites(
          &ONflag,
          &currentMode,
          &loadingFlag
#ifdef USE_NTP
          ,
          &dawnFlag
#endif
          ))
  {
    FastLED.setBrightness(modes[currentMode].Brightness);
    FastLED.clear();
    delay(1);
  }

#if USE_MQTT
  if (espMode == 1U && mqttClient && WiFi.isConnected() && !mqttClient->connected())
  {
    // библиотека не умеет восстанавливать соединение
    // в случае потери подключения к MQTT брокеру, нужно управлять этим явно
    MqttManager::mqttConnect();
    MqttManager::needToPublish = true;
  }

  if (MqttManager::needToPublish)
  {
    // проверка входящего MQTT сообщения;
    // если оно не пустое - выполнение команды из него и формирование MQTT ответа
    if (strlen(inputBuffer) > 0)
    {
      processInputBuffer(inputBuffer, MqttManager::mqttBuffer, true);
    }

    MqttManager::publishState();
  }
#endif

#if defined(GENERAL_DEBUG) && GENERAL_DEBUG_TELNET
  handleTelnetClient();
#endif

  // пнуть собаку
  ESP.wdtFeed();
}
