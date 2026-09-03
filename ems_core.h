// =============================================================
// ALPHA HEMS: SILNIK DECYZYJNY C++
// Repozytorium: zxvxv/RCE_BOX/main/ems_core.h
// =============================================================

#pragma once

#include "esphome.h"
#include <map>

using namespace esphome;

class AlphaEMSCore : public PollingComponent {
private:
    modbus::Modbus *modbus_dev;
    
    // Tarcza Ochrony EEPROM (Pamięć podręczna rejestrów)
    std::map<uint16_t, uint16_t> register_cache;

    // Parametry Bazowe Systemu
    float battery_capacity_kwh = 47.0;
    float safe_min_soc = 20.0;
    float sell_threshold_price = 0.70; // PLN
    
    // Zmienne dynamiczne (w pełnej wersji aktualizowane przez zapytania HTTPS)
    float current_rce_price = 0.0;
    float pv_forecast_today = 0.0;
    float pv_forecast_tomorrow = 0.0;
    
    // Matryca szufladek (Noc, Poranek, Wieczór) dla dni od 0 (Niedziela) do 6 (Sobota)
    float szufladki[7][3] = {
        {3.0, 8.0, 11.0}, // Nd
        {2.5, 7.0, 12.0}, // Pn
        {2.5, 7.0, 12.0}, // Wt
        {2.5, 7.0, 12.0}, // Śr
        {2.5, 7.0, 12.0}, // Cz
        {3.0, 6.0, 15.0}, // Pt
        {3.5, 8.0, 13.0}  // Sb
    };

    int current_soc = 50;

public:
    AlphaEMSCore(modbus::Modbus *modbus_dev) : PollingComponent(15000), modbus_dev(modbus_dev) {}

    void setup() override {
        ESP_LOGI("ALPHA_EMS", "Inicjalizacja pancerki C++ ALPHA HEMS...");
    }

    void update() override {
        syncModbusTelemetry();
        executePredictiveArbitrage();
    }

    // ==========================================
    // TWARDA OCHRONA PAMIĘCI FLASH / EEPROM
    // ==========================================
    void safeModbusWrite(uint16_t reg_addr, uint16_t new_value) {
        // 1. Ochrona w locie (RAM cache)
        if (register_cache.find(reg_addr) != register_cache.end()) {
            if (register_cache[reg_addr] == new_value) {
                return; // Wartość zbieżna, blokujemy zapis.
            }
        }

        // 2. Weryfikacja sprzętowa (zabezpieczenie przed desynchronizacją)
        uint16_t current_hw_val = readHardwareRegister(reg_addr);
        register_cache[reg_addr] = current_hw_val;

        if (current_hw_val != new_value) {
            ESP_LOGW("ALPHA_EMS", "EEPROM GUARD: Modyfikacja 0x%04X (Aktualnie: %d -> Nowa: %d)", reg_addr, current_hw_val, new_value);
            
            modbus::ModbusCommandItem cmd;
            cmd.command = 0x06; // Write Single Register
            cmd.register_address = reg_addr;
            cmd.register_count = 1;
            cmd.payload.push_back(new_value >> 8);
            cmd.payload.push_back(new_value & 0xFF);
            modbus_dev->send(modbus::ModbusDevice::broadcast_address, cmd);
            
            register_cache[reg_addr] = new_value;
        }
    }

    uint16_t readHardwareRegister(uint16_t reg_addr) {
        // Zastępcza funkcja blokującego odczytu Modbus
        // Docelowo tu wykorzystujemy API ESPHome do nadania komendy 0x03 i oczekiwania na odpowiedź
        return register_cache[reg_addr]; // Symulacja
    }

    void syncModbusTelemetry() {
        // Tu aktualizujemy current_soc oraz ew. moce (odpytanie rejestru 0x0222 / 546)
    }

    // ==========================================
    // MÓZG: RENTGEN DECYZJI & PREDICTIVE TWIN
    // ==========================================
    void executePredictiveArbitrage() {
        auto time = id(sntp_time).now();
        if (!time.is_valid()) return;

        int day_of_week = time.day_of_week - 1; // 0 = Niedziela
        int hour = time.hour;
        int day_tomorrow = (day_of_week + 1) % 7;

        float cons_n_jutro = szufladki[day_tomorrow][0];
        float cons_p_jutro = szufladki[day_tomorrow][1];
        float cons_w_jutro = szufladki[day_tomorrow][2];

        float total_jutro = cons_n_jutro + cons_p_jutro + cons_w_jutro;
        float wymog_140 = total_jutro * 1.4;
        bool is_sunny_tomorrow = pv_forecast_tomorrow >= wymog_140;

        int target_soc = 100;

        // --- MATRYCA WYLICZEŃ PŁOTKA ---
        if (hour >= 22 || hour < 6) { 
            // Noc - Smart Morning logic
            float mnoznik = is_sunny_tomorrow ? 0.2 : 1.2;
            float req_kwh = (cons_p_jutro * mnoznik) + ((safe_min_soc / 100.0) * battery_capacity_kwh);
            target_soc = (int)((req_kwh / battery_capacity_kwh) * 100) + 1;
        } else if (hour >= 6 && hour < 13) { 
            // Poranek
            target_soc = (int)safe_min_soc;
        } else { 
            // Wieczór
            float req_kwh = cons_w_jutro * 1.2;
            target_soc = (int)((req_kwh / battery_capacity_kwh) * 100) + (int)safe_min_soc;
        }

        // Twardy docisk (Anti-spike clamp)
        int min_clamp = (int)(safe_min_soc * 1.1);
        target_soc = std::max(target_soc, min_clamp);
        target_soc = std::min(target_soc, 100);

        // --- DECYZJA MAKLERA ---
        if (current_rce_price >= sell_threshold_price && current_soc > target_soc) {
            ESP_LOGI("ALPHA_EMS", "✅ DECYZJA: SPRZEDAŻ. Wolny SOC: %d%%. Płotek: %d%%", (current_soc - target_soc), target_soc);
            safeModbusWrite(0x008F, 12000); // Max Sell Power
            safeModbusWrite(0x0112, 1);     // Włącz Solar Sell (Deye)
        } else {
            ESP_LOGI("ALPHA_EMS", "⏳ DECYZJA: OCZEKIWANIE (Tryb Bilansowy). Płotek: %d%%", target_soc);
            safeModbusWrite(0x0112, 0);     // Wyłącz Solar Sell
            safeModbusWrite(0x006C, 240);   // Max Charge Current (240A Kaskada)
        }
    }
};