#pragma once
#include <EEPROM.h>
#include "EepromManager.h"
#include "Constants.h"

// значение по умолчанию для интервала переключения избранных эффектов в секундах
#define DEFAULT_FAVORITES_INTERVAL           (300U)         
 // значение по умолчанию для разброса интервала переключения избранных эффектов в секундах
#define DEFAULT_FAVORITES_DISPERSION         (0U)          


class FavoritesManager
{
  public:
      // флаг "работает режим автоматической смены избранных эффектов"
    static uint8_t FavoritesRunning;                      
    // статический интервал (время между сменами эффектов)
    static uint16_t Interval;                               
    // дополнительный динамический (случайный) интервал (время между сменами эффектов)
    static uint16_t Dispersion;                             
    // флаг, определяющий, нужно ли использовать сохранённое значение 
    // FavoritesRunning при перезапуске; еслин нет, "избранное" будет выключено при старте
    static uint8_t UseSavedFavoritesRunning;                
    // массив, каждый элемент которого соответствует флагу "эффект №... добавлен в избранные"
    static uint8_t FavoriteModes[MODE_AMOUNT];              

    // помещает в statusText состояние режима работы избранных эффектов
    static void SetStatus(char* statusText)                
    {
      char buff[6];
      strcpy_P(statusText, PSTR("FAV "));

      itoa(FavoritesRunning, buff, 10);
      strcat(statusText, buff);
      strcat_P(statusText, PSTR(" "));
      buff[0] = '\0';

      itoa(Interval, buff, 10);
      strcat(statusText, buff);
      strcat_P(statusText, PSTR(" "));
      buff[0] = '\0';

      itoa(Dispersion, buff, 10);
      strcat(statusText, buff);
      strcat_P(statusText, PSTR(" "));
      buff[0] = '\0';

      itoa(UseSavedFavoritesRunning, buff, 10);
      strcat(statusText, buff);
      strcat_P(statusText, PSTR(" "));
      buff[0] = '\0';

      for (uint8_t i = 0; i < MODE_AMOUNT; i++)
      {
        itoa(FavoriteModes[i], buff, 10);
        strcat(statusText, buff);
        if (i < MODE_AMOUNT - 1) strcat_P(statusText, PSTR(" "));
        buff[0] = '\0';
      }

      statusText += '\0';
    }

    // принимает statusText, парсит его и инициализирует свойства класса значениями из statusText'а
    static void ConfigureFavorites(const char* statusText) 
    {
      FavoritesRunning = getFavoritesRunning(statusText);
      if (FavoritesRunning == 0)
      {
        nextModeAt = 0;
      }
      Interval = getInterval(statusText);
      Dispersion = getDispersion(statusText);
      UseSavedFavoritesRunning = getUseSavedFavoritesRunning(statusText);
      for (uint8_t i = 0; i < MODE_AMOUNT; i++)
      {
        FavoriteModes[i] = getModeOnOff(statusText, i);
      }
    }

      // функция, обрабатывающая циклическое переключение избранных эффектов; 
      // возвращает true, если эффект был переключен
    static bool HandleFavorites(                            
      bool* ONflag,
      int8_t* currentMode,
      bool* loadingFlag
      #ifdef USE_NTP
      , bool* dawnFlag
      #endif
    )
    {
      if (FavoritesRunning == 0 ||
          // лампа не переключается на следующий эффект при выключенной матрице
          !*ONflag                                          
          #ifdef USE_NTP
           // лампа не переключается на следующий эффект при включенном будильнике
          || *dawnFlag                                     
          #endif
      )
      {
        return false;
      }

      // лампа не переключается на следующий эффект сразу после включения режима избранных эффектов
      if (nextModeAt == 0)                                  
      {
        nextModeAt = getNextTime();
        return false;
      }

      if (millis() >= nextModeAt)
      {
        *currentMode = getNextFavoriteMode(currentMode);
        *loadingFlag = true;
        nextModeAt = getNextTime();

        #ifdef GENERAL_DEBUG
        LOG.printf_P(PSTR("Переключение на следующий избранный режим: %d\n\n"), (*currentMode));
        #endif

        return true;
      }

      return false;
    }

    static void ReadFavoritesFromEeprom()
    {
      Interval = EepromManager::ReadUint16(EEPROM_FAVORITES_START_ADDRESS + 1);
      Dispersion = EepromManager::ReadUint16(EEPROM_FAVORITES_START_ADDRESS + 3);
      UseSavedFavoritesRunning = EEPROM.read(EEPROM_FAVORITES_START_ADDRESS + 5);
      FavoritesRunning = UseSavedFavoritesRunning > 0 ? EEPROM.read(EEPROM_FAVORITES_START_ADDRESS) : FavoritesRunning;

      for (uint8_t i = 0; i < MODE_AMOUNT; i++)
      {
        FavoriteModes[i] = EEPROM.read(EEPROM_FAVORITES_START_ADDRESS + i + 6);
        FavoriteModes[i] = FavoriteModes[i] > 0 ? 1 : 0;
      }
    }

    static void SaveFavoritesToEeprom()
    {
      EEPROM.put(EEPROM_FAVORITES_START_ADDRESS, FavoritesRunning);
      EepromManager::WriteUint16(EEPROM_FAVORITES_START_ADDRESS + 1, Interval);
      EepromManager::WriteUint16(EEPROM_FAVORITES_START_ADDRESS + 3, Dispersion);
      EEPROM.put(EEPROM_FAVORITES_START_ADDRESS + 5, UseSavedFavoritesRunning);

      for (uint8_t i = 0; i < MODE_AMOUNT; i++)
      {
        EEPROM.put(EEPROM_FAVORITES_START_ADDRESS + i + 6, FavoriteModes[i] > 0 ? 1 : 0);
      }

      EEPROM.commit();
    }

    static void TurnFavoritesOff()
    {
      FavoritesRunning = 0;
      nextModeAt = 0;
    }

  private:
     // ближайшее время переключения на следующий избранный эффект (millis())
    static uint32_t nextModeAt;                            

    // валидирует statusText (проверяет, правильное ли количество компонентов он содержит)
    static bool isStatusTextCorrect(const char* statusText) 
    {
      char buff[MAX_UDP_BUFFER_SIZE];
      strcpy(buff, statusText);

      uint8_t lexCount = 0;
      char* p = strtok(buff, " ");
      // пока есть лексемы...
      while (p != NULL)                                   
      {
        lexCount++;
        p = strtok(NULL, " ");
      }

      return lexCount == getStatusTextNormalComponentsCount();
    }

     // возвращает правильное ли коичество компонентов для statusText 
     // в зависимости от определённого формата команды и количества эффектов
    static uint8_t getStatusTextNormalComponentsCount()    
    {
      // "FAV 0/1 <цифра> <цифра> 0/1 <массив цифр 0/1 для каждого режима>"
      // (вкл/выкл, интервал в секундах, разброс в секундах, использовать ли хранимый вкл/выкл,
      // вкл/выкл каждого эффекта в избранные)
      return
        1 +          // "FAV"
        1 +          // вкл/выкл
        1 +          // интервал
        1 +          // разброс
        1 +          // использовать ли хранимый вкл/выкл
        MODE_AMOUNT; // 0/1 для каждого эффекта
    }

    // возвращает признак вкл/выкл режима избранных эффектов из statusText
    static uint8_t getFavoritesRunning(const char* statusText)      
    {
      char lexem[2];
      memset(lexem, 0, 2);
      strcpy(lexem, getLexNo(statusText, 1));
      return lexem != NULL
        ? !strcmp(lexem, "1")
        : 0;
    }

     // возвращает интервал (постоянную составляющую) переключения избранных эффектов из statusText
    static uint16_t getInterval(const char* statusText)    
    {
      char lexem[6];
      memset(lexem, 0, 6);
      strcpy(lexem, getLexNo(statusText, 2));
      return lexem != NULL
        ? atoi((const char*)lexem)
        : DEFAULT_FAVORITES_INTERVAL;
    }

    // возвращает разброс (случайную составляющую) интервала переключения избранных эффектов из statusText
    static uint16_t getDispersion(const char* statusText)   
    {
      char lexem[6];
      memset(lexem, 0, 6);
      strcpy(lexem, getLexNo(statusText, 3));
      return lexem != NULL
        ? atoi((const char*)lexem)
        : DEFAULT_FAVORITES_DISPERSION;
    }

    // возвращает признак вкл/выкл режима избранных эффектов из statusText
    static uint8_t getUseSavedFavoritesRunning(const char* statusText)
    {
      char lexem[2];
      memset(lexem, 0, 2);
      strcpy(lexem, getLexNo(statusText, 4));
      return lexem != NULL
        ? !strcmp(lexem, "1")
        : 0;
    }

    // возвращает признак включения указанного эффекта в избранные эффекты
    static bool getModeOnOff(const char* statusText, uint8_t modeId)  
    {
      char lexem[2];
      memset(lexem, 0, 2);
      strcpy(lexem, getLexNo(statusText, modeId + 5));
      return lexem != NULL
        ? !strcmp(lexem, "1")
        : false;
    }

     // служебная функция, разбивает команду statusText на лексемы
     // ("слова", разделённые пробелами) и возвращает указанную по счёту лексему
    static char* getLexNo(const char* statusText, uint8_t pos)       
    {
      if (!isStatusTextCorrect(statusText))
      {
        return NULL;
      }

      const uint8_t buffSize = MAX_UDP_BUFFER_SIZE;
      char buff[buffSize];
      memset(buff, 0, buffSize);
      strcpy(buff, statusText);

      uint8_t lexPos = 0;
      char* p = strtok(buff, " ");
      // пока есть лексемы...
      while (p != NULL)                                   
      {
        if (lexPos == pos)
        {
          return p;
        }

        p = strtok(NULL, " ");
        lexPos++;
      }

      return NULL;
    }

     // возвращает следующий (случайный) включенный в избранные эффект
    static int8_t getNextFavoriteMode(int8_t* currentMode) 
    {
      int8_t result = *currentMode;

      // случайное количество попыток определения следующего эффекта;
      // без этого будет выбран следующий (избранный) по порядку после текущего
      for (int8_t tryNo = 0; tryNo <= random(0, MODE_AMOUNT); tryNo++)
      {
        for (uint8_t i = (result + 1); i <= (result + MODE_AMOUNT); i++)
        {
          if (FavoriteModes[i < MODE_AMOUNT ? i : i - MODE_AMOUNT] > 0)
          {
            result = i < MODE_AMOUNT ? i : i - MODE_AMOUNT;
            break;
          }
        }        
      }

      return result;
    }

     // определяет время следующего переключения на следующий избранный эффект
    static uint32_t getNextTime()                          
    {
      return millis() + Interval * 1000 + random(0, Dispersion + 1) * 1000;
    }
};
