#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "pico/stdlib.h"
#include "hardware/adc.h"

// ---------- CONFIG ----------

// ADC pins
#define CELL_ADC_GPIO      26  // GPIO26 = ADC0 για cell voltage
#define CELL_ADC_INPUT     0
#define PACK_V_ADC_GPIO    27  // GPIO27 = ADC1 για pack voltage
#define PACK_V_ADC_INPUT   1
#define PACK_I_ADC_GPIO    28  // GPIO28 = ADC2 για pack current
#define PACK_I_ADC_INPUT   2

// RMS sampling
#define RMS_SAMPLES        100
#define RMS_SAMPLE_DELAY_US 50

// Logging
#define LOG_SIZE           100  // Πόσες καταχωρήσεις να κρατάει στη μνήμη
#define LOG_INTERVAL_MS    1000 // Κάθε πόσο να κάνει log entry

// Divider values (ohms)
static const float R_TOP    = 100000.0f;   // 100 kΩ
static const float R_BOTTOM = 47000.0f;    // 47 kΩ

// Current sensor config
static const float I_ZERO_V = 1.65f;
static const float I_SCALE  = 30.3f;

// Cell voltage limits 
static float CELL_OV_LIMIT_V = 4.20f;
static float CELL_UV_LIMIT_V = 3.00f;

// Temperature limits 
static float TEMP_WARN_C   = 50.0f;
static float TEMP_SHUT_C   = 60.0f;

// Thermal throttling
static float THROTTLE_START_TEMP = 45.0f;
static float THROTTLE_MAX_TEMP   = 55.0f;

// Calibration factor and offset
static float cal_factor = 1.0f;
static float voltage_offset = 0.0f;

typedef struct {
    // Cell measurements
    float cell_v;
    float cell_v_rms;
    float cell_temp_c;
    bool  ov_flag;
    bool  uv_flag;
    bool  t_warn;
    bool  t_fault;
    
    // Pack measurements
    float pack_v;
    float pack_v_rms;
    float pack_i;
    float pack_i_rms;
    float pack_power;
    float pack_power_rms;
    
    // Thermal throttling
    float throttle_factor;
    
    // Diagnostics
    float voltage_pin;
    bool  data_valid;
} bms_status_t;

// Log entry με timestamp
typedef struct {
    uint32_t timestamp_ms;
    bms_status_t status;
} log_entry_t;

// Circular buffer για logs
typedef struct {
    log_entry_t entries[LOG_SIZE];
    uint32_t write_index;
    uint32_t count;
} log_buffer_t;

static log_buffer_t log_buffer = {0};

// ---------- LOGGING FUNCTIONS ----------

static void log_add_entry(const bms_status_t *status) {
    log_entry_t *entry = &log_buffer.entries[log_buffer.write_index];
    
    entry->timestamp_ms = to_ms_since_boot(get_absolute_time());
    entry->status = *status;
    
    log_buffer.write_index = (log_buffer.write_index + 1) % LOG_SIZE;
    
    if (log_buffer.count < LOG_SIZE) {
        log_buffer.count++;
    }
}

static void log_export_csv(void) {
    if (log_buffer.count == 0) {
        printf("ERROR: No data to export\n");
        return;
    }
    
    printf("\n========== CSV EXPORT START ==========\n");
    
    // CSV Header
    printf("Timestamp_ms,Timestamp_sec,");
    printf("Cell_V,Cell_V_RMS,Cell_Temp_C,");
    printf("Pack_V,Pack_V_RMS,Pack_I,Pack_I_RMS,");
    printf("Power_W,Power_W_RMS,");
    printf("Throttle_Percent,");
    printf("OV_Flag,UV_Flag,T_Warn,T_Fault,Data_Valid\n");
    
    // Data rows
    uint32_t start = (log_buffer.write_index + LOG_SIZE - log_buffer.count) % LOG_SIZE;
    
    for (uint32_t i = 0; i < log_buffer.count; i++) {
        uint32_t idx = (start + i) % LOG_SIZE;
        log_entry_t *e = &log_buffer.entries[idx];
        
        printf("%lu,%.3f,", 
               e->timestamp_ms,
               e->timestamp_ms / 1000.0f);
        
        printf("%.3f,%.3f,%.1f,",
               e->status.cell_v,
               e->status.cell_v_rms,
               e->status.cell_temp_c);
        
        printf("%.3f,%.3f,%.3f,%.3f,",
               e->status.pack_v,
               e->status.pack_v_rms,
               e->status.pack_i,
               e->status.pack_i_rms);
        
        printf("%.2f,%.2f,",
               e->status.pack_power,
               e->status.pack_power_rms);
        
        printf("%.1f,",
               e->status.throttle_factor * 100.0f);
        
        printf("%d,%d,%d,%d,%d\n",
               e->status.ov_flag,
               e->status.uv_flag,
               e->status.t_warn,
               e->status.t_fault,
               e->status.data_valid);
    }
    
    printf("========== CSV EXPORT END ==========\n\n");
    printf("INSTRUCTIONS:\n");
    printf("1. Select all text between START and END lines\n");
    printf("2. Copy to clipboard (Ctrl+C)\n");
    printf("3. Paste into text editor and save as .csv\n");
    printf("4. Open with Excel, Python pandas, etc.\n\n");
}

static void log_print_all(void) {
    if (log_buffer.count == 0) {
        printf("LOG: Empty\n");
        return;
    }
    
    printf("\n=== LOG DUMP (%lu entries) ===\n", log_buffer.count);
    printf("Time(s) | CellV | CellVrms | PackV | PackI | Power | Temp | Thr%% | OV|UV|TW|TF|VAL\n");
    printf("--------|-------|----------|-------|-------|-------|------|------|---------------\n");
    
    uint32_t start = (log_buffer.write_index + LOG_SIZE - log_buffer.count) % LOG_SIZE;
    
    for (uint32_t i = 0; i < log_buffer.count; i++) {
        uint32_t idx = (start + i) % LOG_SIZE;
        log_entry_t *entry = &log_buffer.entries[idx];
        
        printf("%7.1f | %5.2f | %8.2f | %5.2f | %5.2f | %5.1f | %4.1f | %4.0f | %d | %d | %d | %d | %d\n",
               entry->timestamp_ms / 1000.0f,
               entry->status.cell_v,
               entry->status.cell_v_rms,
               entry->status.pack_v,
               entry->status.pack_i,
               entry->status.pack_power,
               entry->status.cell_temp_c,
               entry->status.throttle_factor * 100.0f,
               entry->status.ov_flag,
               entry->status.uv_flag,
               entry->status.t_warn,
               entry->status.t_fault,
               entry->status.data_valid);
    }
    
    printf("======================\n\n");
}

static void log_print_last(uint32_t n) {
    if (log_buffer.count == 0) {
        printf("LOG: Empty\n");
        return;
    }
    
    if (n > log_buffer.count) n = log_buffer.count;
    
    printf("\n=== LAST %lu ENTRIES ===\n", n);
    printf("Time(s) | CellV | PackV | PackI | Power | Temp | Flags\n");
    printf("--------|-------|-------|-------|-------|------|-------\n");
    
    uint32_t start = (log_buffer.write_index + LOG_SIZE - n) % LOG_SIZE;
    
    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = (start + i) % LOG_SIZE;
        log_entry_t *entry = &log_buffer.entries[idx];
        
        printf("%7.1f | %5.2f | %5.2f | %5.2f | %5.1f | %4.1f | OV=%d UV=%d TW=%d TF=%d\n",
               entry->timestamp_ms / 1000.0f,
               entry->status.cell_v,
               entry->status.pack_v,
               entry->status.pack_i,
               entry->status.pack_power,
               entry->status.cell_temp_c,
               entry->status.ov_flag,
               entry->status.uv_flag,
               entry->status.t_warn,
               entry->status.t_fault);
    }
    
    printf("====================\n\n");
}

static void log_clear(void) {
    log_buffer.write_index = 0;
    log_buffer.count = 0;
    printf("LOG: Cleared\n");
}

static void log_stats(void) {
    if (log_buffer.count == 0) {
        printf("LOG: No data for statistics\n");
        return;
    }
    
    float min_v = 999.0f, max_v = 0.0f, avg_v = 0.0f;
    float min_t = 999.0f, max_t = 0.0f, avg_t = 0.0f;
    float total_energy = 0.0f;
    uint32_t fault_count = 0;
    
    uint32_t start = (log_buffer.write_index + LOG_SIZE - log_buffer.count) % LOG_SIZE;
    
    for (uint32_t i = 0; i < log_buffer.count; i++) {
        uint32_t idx = (start + i) % LOG_SIZE;
        log_entry_t *entry = &log_buffer.entries[idx];
        
        float v = entry->status.cell_v;
        float t = entry->status.cell_temp_c;
        
        if (v < min_v) min_v = v;
        if (v > max_v) max_v = v;
        avg_v += v;
        
        if (t < min_t) min_t = t;
        if (t > max_t) max_t = t;
        avg_t += t;
        
        total_energy += entry->status.pack_power * (LOG_INTERVAL_MS / 3600000.0f);
        
        if (entry->status.ov_flag || entry->status.uv_flag || 
            entry->status.t_warn || entry->status.t_fault) {
            fault_count++;
        }
    }
    
    avg_v /= log_buffer.count;
    avg_t /= log_buffer.count;
    
    printf("\n=== LOG STATISTICS ===\n");
    printf("Entries: %lu\n", log_buffer.count);
    printf("Cell Voltage: min=%.2f V, max=%.2f V, avg=%.2f V\n", min_v, max_v, avg_v);
    printf("Temperature:  min=%.1f C, max=%.1f C, avg=%.1f C\n", min_t, max_t, avg_t);
    printf("Total Energy: %.3f Wh\n", total_energy);
    printf("Fault events: %lu (%.1f%%)\n", fault_count, (fault_count * 100.0f) / log_buffer.count);
    printf("======================\n\n");
}

// ---------- DIGITAL DATA VALIDITY ----------

static bool read_digital_data_valid(void) {
    return (rand() % 100) < 80;
}

// ---------- ADC HELPERS ----------

static float adc_raw_to_vpin(uint16_t raw) {
    const float VREF = 3.3f;
    const float ADC_MAX = 4095.0f;
    return (raw * VREF) / ADC_MAX;
}

static float vpin_to_vcell(float vpin) {
    return (vpin * (R_TOP + R_BOTTOM) / R_BOTTOM * cal_factor) + voltage_offset;
}

static float vpin_to_vcell(float vpin) {
    return vpin * R_div_calibrated + voltage_offset;
}

static float vpin_to_pack_v(float vpin) {
    return vpin * (R_TOP + R_BOTTOM) / R_BOTTOM;
}

static float vpin_to_current(float vpin) {
    return (vpin - I_ZERO_V) * I_SCALE;
}

// ---------- RMS CALCULATIONS (check agaain) ----------

static float read_cell_v_rms(void) {
    float sum_sq = 0.0f;
    adc_select_input(CELL_ADC_INPUT);
    
    for (int i = 0; i < RMS_SAMPLES; i++) {
        uint16_t raw = adc_read();
        float v = vpin_to_vcell(adc_raw_to_vpin(raw));
        sum_sq += v * v;
        sleep_us(RMS_SAMPLE_DELAY_US);
    }
    
    return sqrtf(sum_sq / RMS_SAMPLES);
}

static float read_pack_v_rms(void) {
    float sum_sq = 0.0f;
    adc_select_input(PACK_V_ADC_INPUT);
    
    for (int i = 0; i < RMS_SAMPLES; i++) {
        uint16_t raw = adc_read();
        float v = vpin_to_pack_v(adc_raw_to_vpin(raw));
        sum_sq += v * v;
        sleep_us(RMS_SAMPLE_DELAY_US);
    }
    
    return sqrtf(sum_sq / RMS_SAMPLES);
}

static float read_pack_i_rms(void) {
    float sum_sq = 0.0f;
    adc_select_input(PACK_I_ADC_INPUT);
    
    for (int i = 0; i < RMS_SAMPLES; i++) {
        uint16_t raw = adc_read();
        float i = vpin_to_current(adc_raw_to_vpin(raw));
        sum_sq += i * i;
        sleep_us(RMS_SAMPLE_DELAY_US);
    }
    
    return sqrtf(sum_sq / RMS_SAMPLES);
}

static float calculate_power_rms(void) {
    float sum_sq = 0.0f;
    
    for (int i = 0; i < RMS_SAMPLES; i++) {
        adc_select_input(PACK_V_ADC_INPUT);
        uint16_t raw_v = adc_read();
        float v = vpin_to_pack_v(adc_raw_to_vpin(raw_v));
        
        adc_select_input(PACK_I_ADC_INPUT);
        uint16_t raw_i = adc_read();
        float i = vpin_to_current(adc_raw_to_vpin(raw_i));
        
        float p = v * i;
        sum_sq += p * p;
        
        sleep_us(RMS_SAMPLE_DELAY_US);
    }
    
    return sqrtf(sum_sq / RMS_SAMPLES);
}

static float read_temperature_c(void) {
    return 25.0f + (rand() % 20);
}

// ---------- THERMAL THROTTLING ----------

static float calculate_throttle_factor(float temp_c) {
    if (temp_c < THROTTLE_START_TEMP) {
        return 1.0f;
    }
    if (temp_c >= THROTTLE_MAX_TEMP) {
        return 0.0f;
    }
    
    float range = THROTTLE_MAX_TEMP - THROTTLE_START_TEMP;
    float factor = 1.0f - ((temp_c - THROTTLE_START_TEMP) / range);
    return factor;
}

// ---------- MEASUREMENT ----------

static void measure_bms(bms_status_t *st) {
    st->data_valid = read_digital_data_valid();
    
    if (!st->data_valid) {
        printf("WARNING: Wrong digital value given - data not valid!\n");
        return;
    }
    
    // Cell voltage
    adc_select_input(CELL_ADC_INPUT);
    sleep_us(10);
    uint16_t raw = adc_read();
    float vpin = adc_raw_to_vpin(raw);
    st->cell_v = vpin_to_vcell(vpin);
    st->voltage_pin = vpin;
    st->cell_v_rms = read_cell_v_rms();
    
    // Pack voltage
    adc_select_input(PACK_V_ADC_INPUT);
    sleep_us(10);
    raw = adc_read();
    st->pack_v = vpin_to_pack_v(adc_raw_to_vpin(raw));
    st->pack_v_rms = read_pack_v_rms();
    
    // Pack current
    adc_select_input(PACK_I_ADC_INPUT);
    sleep_us(10);
    raw = adc_read();
    st->pack_i = vpin_to_current(adc_raw_to_vpin(raw));
    st->pack_i_rms = read_pack_i_rms();
    
    // Power
    st->pack_power = st->pack_v * st->pack_i;
    st->pack_power_rms = calculate_power_rms();
    
    // Temperature
    st->cell_temp_c = read_temperature_c();
    
    // Flags
    st->ov_flag = (st->cell_v > CELL_OV_LIMIT_V);
    st->uv_flag = (st->cell_v < CELL_UV_LIMIT_V);
    st->t_warn  = (st->cell_temp_c > TEMP_WARN_C);
    st->t_fault = (st->cell_temp_c > TEMP_SHUT_C);
    
    // Thermal throttling
    st->throttle_factor = calculate_throttle_factor(st->cell_temp_c);
}

// ---------- SERIAL COMMAND HANDLER ----------

#define CMD_BUF_SIZE 64

static void process_command(const char *line, const bms_status_t *st) {
    if (strcmp(line, "READ") == 0) {
        printf("\n=== BMS STATUS ===\n");
        printf("Cell V: %.3f V (RMS: %.3f V)\n", st->cell_v, st->cell_v_rms);
        printf("Cell T: %.1f C\n", st->cell_temp_c);
        printf("Pack V: %.3f V (RMS: %.3f V)\n", st->pack_v, st->pack_v_rms);
        printf("Pack I: %.3f A (RMS: %.3f A)\n", st->pack_i, st->pack_i_rms);
        printf("Power:  %.2f W (RMS: %.2f W)\n", st->pack_power, st->pack_power_rms);
        printf("Throttle: %.1f%%\n", st->throttle_factor * 100.0f);
        printf("Flags: OV=%d UV=%d TW=%d TF=%d VALID=%d\n",
               st->ov_flag, st->uv_flag, st->t_warn, st->t_fault, st->data_valid);
        printf("==================\n");
    }
    else if (strcmp(line, "LOG") == 0 || strcmp(line, "LOGALL") == 0) {
        log_print_all();
    }
    else if (strcmp(line, "LOGCSV") == 0 || strcmp(line, "CSV") == 0) {
        log_export_csv();
    }
    else if (strncmp(line, "LOGLAST ", 8) == 0) {
        uint32_t n = atoi(line + 8);
        if (n > 0) {
            log_print_last(n);
        } else {
            printf("ERR: invalid number\n");
        }
    }
    else if (strcmp(line, "LOGSTATS") == 0) {
        log_stats();
    }
    else if (strcmp(line, "LOGCLEAR") == 0) {
        log_clear();
    }
    else if (strncmp(line, "SETCAL ", 7) == 0) {
        float val = strtof(line + 7, NULL);
        if (val > 0.5f && val < 2.0f) {
            cal_factor = val;
            printf("OK: cal_factor=%.4f\n", cal_factor);
        } else {
            printf("ERR: cal value out of range\n");
        }
    }
    else if (strncmp(line, "CAL ", 4) == 0) {
        float delta = strtof(line + 4, NULL);
        cal_factor += delta;
        if (cal_factor < 0.5f) cal_factor = 0.5f;
        if (cal_factor > 2.0f) cal_factor = 2.0f;
        printf("OK: cal_factor=%.4f\n", cal_factor);
    }
    else if (strncmp(line, "SETOFFSET ", 10) == 0) {
        float val = strtof(line + 10, NULL);
        if (val >= -1.0f && val <= 1.0f) {
            voltage_offset = val;
            printf("OK: voltage_offset=%.3f V\n", voltage_offset);
        } else {
            printf("ERR: offset out of range\n");
        }
    }
    else if (strncmp(line, "OFFSET ", 7) == 0) {
        float delta = strtof(line + 7, NULL);
        voltage_offset += delta;
        if (voltage_offset < -1.0f) voltage_offset = -1.0f;
        if (voltage_offset > 1.0f) voltage_offset = 1.0f;
        printf("OK: voltage_offset=%.3f V\n", voltage_offset);
    }
    else if (strncmp(line, "SETOV ", 6) == 0) {
        float val = strtof(line + 6, NULL);
        if (val > 2.5f && val < 5.0f) {
            CELL_OV_LIMIT_V = val;
            printf("OK: OV_LIMIT=%.2f V\n", CELL_OV_LIMIT_V);
        } else {
            printf("ERR: OV value out of range\n");
        }
    }
    else if (strncmp(line, "SETUV ", 6) == 0) {
        float val = strtof(line + 6, NULL);
        if (val > 2.0f && val < 4.0f) {
            CELL_UV_LIMIT_V = val;
            printf("OK: UV_LIMIT=%.2f V\n", CELL_UV_LIMIT_V);
        } else {
            printf("ERR: UV value out of range\n");
        }
    }
    else if (strcmp(line, "HELP") == 0) {
        printf("\n=== BMS COMMANDS ===\n");
        printf("  READ              - show current status\n");
        printf("  LOG / LOGALL      - dump all log entries\n");
        printf("  LOGCSV / CSV      - export log as CSV format\n");
        printf("  LOGLAST <n>       - show last n entries\n");
        printf("  LOGSTATS          - show statistics\n");
        printf("  LOGCLEAR          - clear log\n");
        printf("  SETCAL <val>      - set calibration factor\n");
        printf("  CAL +/-<val>      - adjust calibration\n");
        printf("  SETOFFSET <val>   - set voltage offset\n");
        printf("  OFFSET +/-<val>   - adjust offset\n");
        printf("  SETOV <voltage>   - set OV limit\n");
        printf("  SETUV <voltage>   - set UV limit\n");
        printf("  HELP              - show this help\n");
        printf("====================\n\n");
    }
    else {
        printf("ERR: unknown command '%s' (type HELP)\n", line);
    }
}

static void poll_serial_commands(const bms_status_t *st) {
    static char buf[CMD_BUF_SIZE];
    static int pos = 0;

    while (true) {
        int c = getchar_timeout_us(0);
        if (c == PICO_ERROR_TIMEOUT) break;

        if (c == '\r' || c == '\n') {
            if (pos > 0) {
                buf[pos] = '\0';
                printf("\n>>> %s\n", buf);
                process_command(buf, st);
                pos = 0;
            }
        } else if (pos < CMD_BUF_SIZE - 1) {
            buf[pos++] = (char)c;
        }
    }
}

// ---------- MAIN ----------

int main() {
    stdio_init_all();
    sleep_ms(2000);

    printf("\n================================\n");
    printf("  BMS with CSV Export via USB\n");
    printf("================================\n");
    printf("Type HELP for commands\n");
    printf("Type LOGCSV to export data\n\n");

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    adc_init();
    adc_gpio_init(CELL_ADC_GPIO);
    adc_gpio_init(PACK_V_ADC_GPIO);
    adc_gpio_init(PACK_I_ADC_GPIO);

    bms_status_t bms = {0};
    bms.data_valid = true;

    srand(time_us_32());

    while (true) {
        measure_bms(&bms);
        
        if (bms.data_valid) {
            log_add_entry(&bms);
            
            // LED logic
            bool any_flag = bms.ov_flag || bms.uv_flag || bms.t_warn || bms.t_fault;
            static bool led_state = false;
            if (any_flag) {
                led_state = !led_state;
                gpio_put(PICO_DEFAULT_LED_PIN, led_state);
            } else {
                gpio_put(PICO_DEFAULT_LED_PIN, 1);
            }

            // Compact print
            printf("CellV:%.2f | PackV:%.2f | I:%.2f | P:%.1fW | T:%.1fC | Thr:%.0f%% | OV=%d UV=%d | Log:%lu/%d\n",
                   bms.cell_v,
                   bms.pack_v,
                   bms.pack_i,
                   bms.pack_power,
                   bms.cell_temp_c,
                   bms.throttle_factor * 100.0f,
                   bms.ov_flag, bms.uv_flag,
                   log_buffer.count, LOG_SIZE);
        }

        poll_serial_commands(&bms);

        sleep_ms(LOG_INTERVAL_MS);
    }

    return 0;
}
