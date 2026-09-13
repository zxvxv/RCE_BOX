#include "esphome.h"
#include "FS.h"
#include "SD.h"
#include "SPI.h"

class SDLogger : public Component {
 public:
  void setup() override {
    ESP_LOGI("SD_CARD", "Inicjalizacja karty SD...");
    
    // Twarde przypisanie pinów SPI dla płytki LilyGO (SCK: 14, MISO: 2, MOSI: 15, CS: 13)
    SPI.begin(14, 2, 15, 13);
    
    if (!SD.begin(13, SPI)) {
      ESP_LOGE("SD_CARD", "Brak karty SD lub blad montowania!");
      this->mark_failed();
      return;
    }
    
    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
    ESP_LOGI("SD_CARD", "Karta SD zamontowana poprawnie. Pojemnosc: %llu MB", cardSize);
  }

  void log_csv(std::string filename, std::string data) {
    std::string full_path = "/" + filename;
    File file = SD.open(full_path.c_str(), FILE_APPEND);
    if(!file){
      ESP_LOGE("SD_CARD", "Blad otwarcia pliku: %s", full_path.c_str());
      return;
    }
    file.println(data.c_str());
    file.close();
    ESP_LOGD("SD_CARD", "Zapisano w %s: %s", full_path.c_str(), data.c_str());
  }
};