#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <vector>
#include <cstring>
#include <cstdlib>

#include "ESP_I2S.h"

#include "I2C_Driver.h"
#include "TCA9554PWR.h"
#include "Display_SPD2010.h"
#include "LVGL_Driver.h"
#include "SD_Card.h"
#include "Audio_PCM5101.h"
#include "MIC_MSM.h"
#include "secrets.h"

constexpr const char *IMAGE_URL = "https://raw.githubusercontent.com/Bodmer/TFT_eSPI/master/examples/320%20x%20240/BMPs/Autumn_240x320.bmp";
constexpr const char *MP3_URL = "https://zvukogram.com/index.php?r=site/download&id=44225&type=mp3";
constexpr const char *MP3_DEST_PATH = "/tutututu.mp3";
constexpr const char *MP3_FILE_NAME = "tutututu.mp3";
constexpr const char *VOICE_RECORD_PATH = "/voice_input.wav";
constexpr const char *ASSISTANT_RESPONSE_PATH = "/assistant_response.mp3";
constexpr uint32_t VOICE_SAMPLE_RATE = 16000;
constexpr uint8_t VOICE_BITS_PER_SAMPLE = 16;
constexpr uint8_t VOICE_CHANNELS = 2;
constexpr uint32_t MAX_RECORDING_DURATION_MS = 15000;
constexpr size_t VOICE_BUFFER_SIZE = 512;
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;           // 20 секунд таймаут на подключение

static lv_obj_t *status_label = nullptr;
static lv_obj_t *image_obj = nullptr;
static lv_img_dsc_t remote_image_dsc = {};
static std::vector<uint8_t> image_data;
static std::vector<lv_color_t> image_pixels;
static uint32_t decoded_width = 0;
static uint32_t decoded_height = 0;
static bool sd_ready = false;
static bool audio_playback_started = false;
static String lastStatusMessage;

enum class AssistantState {
  Idle,
  Recording,
  Processing,
  Speaking
};

static AssistantState assistant_state = AssistantState::Idle;
static bool assistant_task_running = false;
static bool upload_requested = false;
static uint32_t recording_start_ms = 0;

static lv_obj_t *assistant_button = nullptr;
static lv_obj_t *assistant_button_label = nullptr;

static I2SClass voice_i2s;
static File recording_file;
static size_t recording_bytes_written = 0;

static String last_user_transcript;
static String last_assistant_reply;

void pumpLvgl(uint32_t delayMs = 5) {
  Lvgl_Loop();
  delay(delayMs);
}

void setStatus(const char *text) {
  if (!status_label) {
    return;
  }

  if (lastStatusMessage == text) {
    return;
  }

  lv_label_set_text(status_label, text);
  lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 10);
  lastStatusMessage = text;
  Serial.println(text);
}

void setStatus(const String &text) {
  setStatus(text.c_str());
}

bool wifiCredentialsConfigured() {
  return std::strcmp(WIFI_SSID, "YOUR_WIFI_SSID") != 0 &&
         std::strcmp(WIFI_PASSWORD, "YOUR_WIFI_PASSWORD") != 0;
}

bool connectToWifi() {
  if (!wifiCredentialsConfigured()) {
    setStatus("Укажите значения WIFI_SSID и WIFI_PASSWORD в коде");
    Serial.println("Wi-Fi credentials are not configured");
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t start = millis();
  uint8_t dotCount = 0;

  while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_CONNECT_TIMEOUT_MS) {
    String status = "Подключение к Wi-Fi";
    for (uint8_t i = 0; i <= dotCount; ++i) {
      status += '.';
    }
    setStatus(status);
    dotCount = (dotCount + 1) % 4;
    pumpLvgl(200);
  }

  if (WiFi.status() != WL_CONNECTED) {
    setStatus("Не удалось подключиться к Wi-Fi");
    Serial.println("Wi-Fi connection failed");
    return false;
  }

  String ip = WiFi.localIP().toString();
  setStatus("Wi-Fi подключен\nIP: " + ip);
  pumpLvgl(100);
  return true;
}

bool processHttpResponse(HTTPClient &http) {
  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    setStatus(String("HTTP ошибка: ") + httpCode);
    Serial.printf("HTTP GET failed, code: %d\n", httpCode);
    return false;
  }

  int remaining = http.getSize();
  image_data.clear();
  if (remaining > 0) {
    image_data.reserve(remaining);
  }

  WiFiClient *stream = http.getStreamPtr();
  uint8_t buffer[1024];
  uint32_t lastUpdate = 0;

  while (http.connected() && (remaining > 0 || remaining == -1)) {
    size_t available = stream->available();
    if (!available) {
      pumpLvgl(5);
      continue;
    }

    size_t toRead = available > sizeof(buffer) ? sizeof(buffer) : available;
    int bytesRead = stream->readBytes(buffer, toRead);
    if (bytesRead <= 0) {
      break;
    }

    image_data.insert(image_data.end(), buffer, buffer + bytesRead);

    if (remaining > 0) {
      remaining -= bytesRead;
    }

    uint32_t now = millis();
    if (now - lastUpdate > 250) {
      String bytes = String(image_data.size());
      setStatus(String("Загружается изображение...\n") + bytes + " байт");
      lastUpdate = now;
    }

    pumpLvgl(1);
  }

  if (image_data.empty()) {
    setStatus("Получены пустые данные изображения");
    Serial.println("Downloaded image is empty");
    return false;
  }

  return true;
}

bool downloadImage(const char *url) {
  HTTPClient http;
  String urlString(url);

  if (urlString.startsWith("https://")) {
    WiFiClientSecure client;
    client.setInsecure();
    if (!http.begin(client, urlString)) {
      setStatus("Не удалось открыть HTTPS соединение");
      Serial.println("Failed to begin HTTPS connection");
      return false;
    }
    bool result = processHttpResponse(http);
    http.end();
    return result;
  } else {
    WiFiClient client;
    if (!http.begin(client, urlString)) {
      setStatus("Не удалось открыть HTTP соединение");
      Serial.println("Failed to begin HTTP connection");
      return false;
    }
    bool result = processHttpResponse(http);
    http.end();
    return result;
  }
}

bool convertBmpToLvglImage(const std::vector<uint8_t> &bmpData) {
  decoded_width = 0;
  decoded_height = 0;
  image_pixels.clear();

  if (bmpData.size() < 54) {
    setStatus("BMP: файл слишком мал");
    Serial.println("BMP decode error: file too small");
    return false;
  }

  auto read16 = [](const uint8_t *ptr) -> uint16_t {
    return static_cast<uint16_t>(ptr[0]) | (static_cast<uint16_t>(ptr[1]) << 8);
  };
  auto read32 = [](const uint8_t *ptr) -> uint32_t {
    return static_cast<uint32_t>(ptr[0]) |
           (static_cast<uint32_t>(ptr[1]) << 8) |
           (static_cast<uint32_t>(ptr[2]) << 16) |
           (static_cast<uint32_t>(ptr[3]) << 24);
  };

  const uint8_t *data = bmpData.data();
  if (data[0] != 'B' || data[1] != 'M') {
    setStatus("BMP: неверная сигнатура");
    Serial.println("BMP decode error: invalid signature");
    return false;
  }

  uint32_t pixelOffset = read32(data + 10);
  uint32_t dibHeaderSize = read32(data + 14);
  if (dibHeaderSize < 40 || bmpData.size() <= pixelOffset) {
    setStatus("BMP: поврежденный заголовок");
    Serial.println("BMP decode error: header too small or offset invalid");
    return false;
  }

  int32_t width = static_cast<int32_t>(read32(data + 18));
  int32_t height = static_cast<int32_t>(read32(data + 22));
  uint16_t planes = read16(data + 26);
  uint16_t bitsPerPixel = read16(data + 28);
  uint32_t compression = read32(data + 30);

  if (planes != 1 || (bitsPerPixel != 24 && bitsPerPixel != 32) || compression != 0) {
    setStatus("BMP: поддерживаются только 24/32 bpp без сжатия");
    Serial.printf("BMP decode error: planes=%u bpp=%u compression=%u\n", planes, bitsPerPixel, compression);
    return false;
  }

  if (width <= 0 || height == 0) {
    setStatus("BMP: некорректные размеры");
    Serial.printf("BMP decode error: width=%ld height=%ld\n", static_cast<long>(width), static_cast<long>(height));
    return false;
  }

  uint32_t absHeight = height > 0 ? static_cast<uint32_t>(height) : static_cast<uint32_t>(-height);
  uint32_t rowSize = ((static_cast<uint32_t>(bitsPerPixel) * static_cast<uint32_t>(width) + 31) / 32) * 4;
  uint32_t neededSize = pixelOffset + rowSize * absHeight;
  if (bmpData.size() < neededSize) {
    setStatus("BMP: данные усечены");
    Serial.printf("BMP decode error: file size %u < needed %u\n", static_cast<unsigned>(bmpData.size()), neededSize);
    return false;
  }

  image_pixels.resize(static_cast<size_t>(width) * absHeight);

  const uint8_t *pixelBase = data + pixelOffset;
  uint8_t bytesPerPixel = static_cast<uint8_t>(bitsPerPixel / 8);

  for (uint32_t y = 0; y < absHeight; ++y) {
    uint32_t srcRow = height > 0 ? (absHeight - 1 - y) : y;
    const uint8_t *rowPtr = pixelBase + srcRow * rowSize;
    lv_color_t *dstRow = image_pixels.data() + (static_cast<size_t>(y) * static_cast<size_t>(width));

    for (int32_t x = 0; x < width; ++x) {
      const uint8_t *pixelPtr = rowPtr + static_cast<size_t>(x) * bytesPerPixel;
      uint8_t b = pixelPtr[0];
      uint8_t g = pixelPtr[1];
      uint8_t r = pixelPtr[2];
      lv_color_t color = lv_color_make(r, g, b);
      dstRow[x] = color;
    }
  }

  remote_image_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
  remote_image_dsc.header.always_zero = 0;
  remote_image_dsc.header.reserved = 0;
  remote_image_dsc.header.w = static_cast<uint32_t>(width);
  remote_image_dsc.header.h = absHeight;
  remote_image_dsc.data_size = image_pixels.size() * sizeof(lv_color_t);
  remote_image_dsc.data = reinterpret_cast<const uint8_t *>(image_pixels.data());

  decoded_width = static_cast<uint32_t>(width);
  decoded_height = absHeight;
  return true;
}

String formatSize(uint64_t bytes) {
  const uint64_t KB = 1024;
  const uint64_t MB = KB * 1024;
  if (bytes >= MB) {
    double value = static_cast<double>(bytes) / static_cast<double>(MB);
    return String(value, 2) + " МБ";
  }
  if (bytes >= KB) {
    double value = static_cast<double>(bytes) / static_cast<double>(KB);
    return String(value, 1) + " КБ";
  }
  return String(bytes) + " Б";
}

bool initializeSdCard() {
  SD_Init();
  uint8_t cardType = SD_MMC.cardType();
  if (cardType == CARD_NONE) {
    setStatus("SD карта не обнаружена");
    Serial.println("SD card not detected");
    return false;
  }

  const char *typeText = "UNKNOWN";
  if (cardType == CARD_MMC) typeText = "MMC";
  else if (cardType == CARD_SD) typeText = "SDSC";
  else if (cardType == CARD_SDHC) typeText = "SDHC";

  uint64_t totalBytes = SD_MMC.totalBytes();
  String message = String("SD карта готова\nТип: ") + typeText;
  if (totalBytes > 0) {
    message += "\nОбъем: " + formatSize(totalBytes);
  }
  setStatus(message);
  return true;
}

bool downloadFileToSD(const char *url, const char *destPath, const char *displayName) {
  HTTPClient http;
  String urlString(url);
  bool isHttps = urlString.startsWith("https://");
  bool beginOk = false;

  WiFiClientSecure secureClient;
  WiFiClient plainClient;
  WiFiClient *clientPtr = nullptr;

  if (isHttps) {
    secureClient.setInsecure();
    clientPtr = &secureClient;
  } else {
    clientPtr = &plainClient;
  }

  beginOk = http.begin(*clientPtr, urlString);

  if (!beginOk) {
    setStatus(String("Не удалось подключиться:\n") + displayName);
    Serial.println("HTTP begin failed for audio download");
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    setStatus(String("Ошибка загрузки (HTTP ") + httpCode + ")\n" + displayName);
    Serial.printf("HTTP GET for audio failed, code: %d\n", httpCode);
    http.end();
    return false;
  }

  if (SD_MMC.exists(destPath)) {
    SD_MMC.remove(destPath);
  }

  File file = SD_MMC.open(destPath, FILE_WRITE);
  if (!file) {
    setStatus(String("Не удалось открыть файл\n") + destPath);
    Serial.println("Failed to open destination file on SD");
    http.end();
    return false;
  }

  int totalBytes = http.getSize();
  int remaining = totalBytes;
  uint64_t downloaded = 0;
  uint8_t buffer[2048];
  WiFiClient *stream = http.getStreamPtr();
  uint32_t lastUpdate = 0;

  while (http.connected() && (remaining > 0 || remaining == -1)) {
    size_t available = stream->available();
    if (!available) {
      pumpLvgl(5);
      continue;
    }

    size_t toRead = available > sizeof(buffer) ? sizeof(buffer) : available;
    int bytesRead = stream->readBytes(buffer, toRead);
    if (bytesRead <= 0) {
      break;
    }

    size_t written = file.write(buffer, bytesRead);
    if (written != static_cast<size_t>(bytesRead)) {
      setStatus("Ошибка записи на SD карту");
      Serial.println("Failed to write all bytes to SD");
      file.close();
      http.end();
      return false;
    }

    downloaded += bytesRead;
    if (remaining > 0) {
      remaining -= bytesRead;
    }

    uint32_t now = millis();
    if (now - lastUpdate > 400) {
      String msg = String("Скачивание аудио...\n") + formatSize(downloaded);
      if (totalBytes > 0) {
        int percent = static_cast<int>((downloaded * 100) / totalBytes);
        msg += String(" ( ") + percent + "% )";
      }
      setStatus(msg);
      lastUpdate = now;
    }

    pumpLvgl(1);
  }

  file.close();
  http.end();

  if (downloaded == 0) {
    setStatus("Получен пустой файл аудио");
    Serial.println("Downloaded audio size is zero");
    return false;
  }

  setStatus(String("Аудио сохранено\n") + destPath + "\n" + formatSize(downloaded));
  return true;
}

bool playAudioFromSd(const char *filePath, const char *displayName) {
  audio.stopSong();
  bool ret = audio.connecttoFS(SD_MMC, filePath);
  if (!ret) {
    setStatus(String("Не удалось открыть аудио:\n") + displayName);
    Serial.println("Failed to start audio playback");
    return false;
  }

  setStatus(String("Воспроизведение: ") + displayName);
  return true;
}

void updateAssistantButton();

void writeUint32LE(File &file, uint32_t value) {
  uint8_t data[4];
  data[0] = value & 0xFF;
  data[1] = (value >> 8) & 0xFF;
  data[2] = (value >> 16) & 0xFF;
  data[3] = (value >> 24) & 0xFF;
  file.write(data, 4);
}

void writeUint16LE(File &file, uint16_t value) {
  uint8_t data[2];
  data[0] = value & 0xFF;
  data[1] = (value >> 8) & 0xFF;
  file.write(data, 2);
}

void writeWavHeader(File &file, uint32_t sampleRate, uint16_t bitsPerSample, uint16_t channels, uint32_t dataSize) {
  file.seek(0);
  file.write(reinterpret_cast<const uint8_t *>("RIFF"), 4);
  writeUint32LE(file, 36 + dataSize);
  file.write(reinterpret_cast<const uint8_t *>("WAVE"), 4);
  file.write(reinterpret_cast<const uint8_t *>("fmt "), 4);
  writeUint32LE(file, 16);  // PCM header size
  writeUint16LE(file, 1);   // PCM format
  writeUint16LE(file, channels);
  writeUint32LE(file, sampleRate);
  uint32_t byteRate = sampleRate * channels * bitsPerSample / 8;
  writeUint32LE(file, byteRate);
  uint16_t blockAlign = channels * bitsPerSample / 8;
  writeUint16LE(file, blockAlign);
  writeUint16LE(file, bitsPerSample);
  file.write(reinterpret_cast<const uint8_t *>("data"), 4);
  writeUint32LE(file, dataSize);
}

void updateWavHeader(File &file, uint32_t dataSize) {
  if (!file) {
    return;
  }
  file.seek(0);
  writeWavHeader(file, VOICE_SAMPLE_RATE, VOICE_BITS_PER_SAMPLE, VOICE_CHANNELS, dataSize);
}

bool startVoiceRecording() {
  if (!sd_ready) {
    setStatus("SD карта недоступна для записи");
    return false;
  }

  if (assistant_state == AssistantState::Recording) {
    return false;
  }

  if (SD_MMC.exists(VOICE_RECORD_PATH)) {
    SD_MMC.remove(VOICE_RECORD_PATH);
  }
  recording_file = SD_MMC.open(VOICE_RECORD_PATH, FILE_WRITE);
  if (!recording_file) {
    setStatus("Не удалось создать файл записи");
    Serial.println("Failed to create recording file");
    return false;
  }

  writeWavHeader(recording_file, VOICE_SAMPLE_RATE, VOICE_BITS_PER_SAMPLE, VOICE_CHANNELS, 0);
  recording_bytes_written = 0;

  voice_i2s.setPins(I2S_PIN_BCK, I2S_PIN_WS, I2S_PIN_DOUT, I2S_PIN_DIN);
  voice_i2s.setTimeout(0);
  if (!voice_i2s.begin(I2S_MODE_STD, VOICE_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    setStatus("Не удалось запустить I2S для микрофона");
    recording_file.close();
    return false;
  }

  recording_start_ms = millis();
  assistant_state = AssistantState::Recording;
  upload_requested = false;
  setStatus("Запись голоса... нажмите ещё раз, чтобы остановить");
  updateAssistantButton();
  return true;
}

void stopVoiceRecordingInternal(bool dueToTimeout) {
  if (assistant_state != AssistantState::Recording) {
    return;
  }

  voice_i2s.end();
  if (recording_file) {
    updateWavHeader(recording_file, static_cast<uint32_t>(recording_bytes_written));
    recording_file.close();
  }

  assistant_state = AssistantState::Processing;
  upload_requested = true;
  if (dueToTimeout) {
    setStatus("Запись остановлена (таймаут)");
  } else {
    setStatus("Запись остановлена");
  }
  updateAssistantButton();
}

bool stopVoiceRecording() {
  if (assistant_state != AssistantState::Recording) {
    return false;
  }
  stopVoiceRecordingInternal(false);
  return true;
}

void handleVoiceRecording() {
  if (assistant_state != AssistantState::Recording || !recording_file) {
    return;
  }

  uint8_t buffer[VOICE_BUFFER_SIZE];
  size_t bytesRead = voice_i2s.readBytes(reinterpret_cast<char *>(buffer), sizeof(buffer));
  if (bytesRead > 0) {
    size_t written = recording_file.write(buffer, bytesRead);
    recording_bytes_written += written;
  }

  if (millis() - recording_start_ms > MAX_RECORDING_DURATION_MS) {
    stopVoiceRecordingInternal(true);
  }
}

void updateAssistantButton() {
  if (!assistant_button || !assistant_button_label) {
    return;
  }

  const char *text = "Говорить";
  switch (assistant_state) {
    case AssistantState::Recording:
      text = "Остановить";
      lv_obj_clear_state(assistant_button, LV_STATE_DISABLED);
      break;
    case AssistantState::Processing:
      text = "Обработка...";
      lv_obj_add_state(assistant_button, LV_STATE_DISABLED);
      break;
    case AssistantState::Speaking:
      text = "Остановить ответ";
      lv_obj_clear_state(assistant_button, LV_STATE_DISABLED);
      break;
    case AssistantState::Idle:
    default:
      text = "Говорить";
      lv_obj_clear_state(assistant_button, LV_STATE_DISABLED);
      break;
  }

  lv_label_set_text(assistant_button_label, text);
  lv_obj_center(assistant_button_label);
}

void assistant_button_event_cb(lv_event_t *e) {
  if (assistant_state == AssistantState::Processing) {
    return;
  }

  if (assistant_state == AssistantState::Recording) {
    stopVoiceRecording();
    updateAssistantButton();
    return;
  }

  if (assistant_state == AssistantState::Speaking) {
    audio.stopSong();
    assistant_state = AssistantState::Idle;
    setStatus("Воспроизведение остановлено");
    updateAssistantButton();
    return;
  }

  audio.stopSong();
  last_user_transcript.clear();
  last_assistant_reply.clear();

  if (!startVoiceRecording()) {
    updateAssistantButton();
    return;
  }

  updateAssistantButton();
}

bool openAiConfigured() {
  return std::strcmp(PROXY_HOST, "YOUR_PROXY_HOST") != 0 &&
         std::strcmp(SERVICE_TOKEN, "REPLACE_WITH_SERVICE_TOKEN") != 0;
}

String escapeJson(const String &input) {
  String output;
  output.reserve(input.length() + 8);
  for (size_t i = 0; i < input.length(); ++i) {
    char c = input[i];
    switch (c) {
      case '\\': output += "\\\\"; break;
      case '"': output += "\\\""; break;
      case '\n': output += "\\n"; break;
      case '\r': output += "\\r"; break;
      case '\t': output += "\\t"; break;
      default:
        if (static_cast<uint8_t>(c) < 0x20) {
          output += ' ';
        } else {
          output += c;
        }
        break;
    }
  }
  return output;
}

String decodeJsonString(const String &json, int startIndex) {
  String value;
  bool escape = false;
  for (int i = startIndex; i < json.length(); ++i) {
    char c = json[i];
    if (escape) {
      switch (c) {
        case '\\': value += '\\'; break;
        case '"': value += '"'; break;
        case 'n': value += '\n'; break;
        case 'r': value += '\r'; break;
        case 't': value += '\t'; break;
        default: value += c; break;
      }
      escape = false;
      continue;
    }
    if (c == '\\') {
      escape = true;
      continue;
    }
    if (c == '"') {
      break;
    }
    value += c;
  }
  return value;
}

// Устойчивый поиск строкового значения по ключу: допускает пробелы после ':'
String findJsonStringByKey(const String &json, const String &key, int startPos) {
  String quotedKey = String('"') + key + String('"');
  int k = json.indexOf(quotedKey, startPos);
  if (k < 0) return "";

  int colon = json.indexOf(':', k + quotedKey.length());
  if (colon < 0) return "";

  // пропускаем пробелы/табуляции/переводы строк
  int i = colon + 1;
  while (i < (int)json.length()) {
    char c = json[i];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { i++; continue; }
    break;
  }

  // ожидаем строку в кавычках
  if (i >= (int)json.length() || json[i] != '"') return "";
  return decodeJsonString(json, i + 1);
}

String jsonExtractString(const String &json, const String &key) {
  return findJsonStringByKey(json, key, 0);
}

String extractAssistantContent(const String &json) {
  // 1) Находим блок(и), где role == "assistant"
  int pos = 0;
  while (true) {
    int roleKey = json.indexOf("\"role\"", pos);
    if (roleKey < 0) break;

    String roleVal = findJsonStringByKey(json, "role", roleKey);
    if (roleVal == "assistant") {
      // Сначала пробуем обычный content как строку
      String s = findJsonStringByKey(json, "content", roleKey);
      if (!s.isEmpty()) return s;

      // Некоторые ответы отдают content как массив объектов {type:"text", text:"..."}
      s = findJsonStringByKey(json, "text", roleKey);
      if (!s.isEmpty()) return s;
    }

    pos = roleKey + 6; // сдвигаем поиск дальше
  }

  // 2) Фоллбек: choices[].message.*
  int choicesPos = json.indexOf("\"choices\"");
  if (choicesPos >= 0) {
    int msgPos = json.indexOf("\"message\"", choicesPos);
    if (msgPos >= 0) {
      String s = findJsonStringByKey(json, "content", msgPos);
      if (!s.isEmpty()) return s;

      s = findJsonStringByKey(json, "text", msgPos);
      if (!s.isEmpty()) return s;
    }
  }

  // 3) Глобальные фоллбеки
  {
    String s = findJsonStringByKey(json, "content", 0);
    if (!s.isEmpty()) return s;
  }
  {
    String s = findJsonStringByKey(json, "text", 0);
    if (!s.isEmpty()) return s;
  }

  return "";
}
 

// ---------- УСТОЙЧИВЫЙ ПАРСЕР HTTP-ОТВЕТА + таймаут ожидания ----------
bool readHttpResponse(WiFiClient &client, int &statusCode, String &contentType, String &body, File *destFile) {
  statusCode = -1;
  contentType = "";
  body = "";

  // Ждём появления данных (или закрытия) с таймаутом
  unsigned long t0 = millis();
  while (!client.available()) {
    if (!client.connected() && !client.available()) {
      return false; // сервер закрыл соединение до ответа
    }
    if (millis() - t0 > 15000) { // 15s
      return false; // таймаут ожидания ответа
    }
    delay(5);
  }

  // Читаем первую непустую строку как статус-строку (иногда бывают пустые строки/CRLF)
  String statusLine;
  for (;;) {
    statusLine = client.readStringUntil('\n');
    if (statusLine.length() == 0) {
      if (!client.connected() && !client.available()) return false;
      continue;
    }
    statusLine.trim(); // убираем \r и пробелы
    if (statusLine.length() == 0) continue;
    // ожидаем "HTTP/1.x ..."
    if (!statusLine.startsWith("HTTP/1.")) {
      // иногда могут прилететь странные строки — попробуем читать дальше
      if (!client.connected() && !client.available()) return false;
      continue;
    }
    break;
  }

  int firstSpace = statusLine.indexOf(' ');
  if (firstSpace < 0) return false;
  int secondSpace = statusLine.indexOf(' ', firstSpace + 1);
  if (secondSpace < 0) secondSpace = statusLine.length();
  statusCode = statusLine.substring(firstSpace + 1, secondSpace).toInt();

  int contentLength = -1;
  bool chunked = false;

  // Заголовки
  while (client.connected()) {
    String headerLine = client.readStringUntil('\n');
    if (headerLine.length() == 0) break;
    headerLine.trim();
    if (headerLine.length() == 0) break; // пустая строка => конец заголовков

    int colon = headerLine.indexOf(':');
    if (colon <= 0) continue;

    String headerName = headerLine.substring(0, colon);
    headerName.toLowerCase();
    String headerValue = headerLine.substring(colon + 1);
    headerValue.trim();

    if (headerName == "content-length") {
      contentLength = headerValue.toInt();
    } else if (headerName == "transfer-encoding" && headerValue.equalsIgnoreCase("chunked")) {
      chunked = true;
    } else if (headerName == "content-type") {
      contentType = headerValue;
    }
  }

  uint8_t buffer[1024];
  auto appendData = [&](const uint8_t *data, size_t len) {
    if (destFile) destFile->write(data, len);
    else body.concat(String((const char*)data, len));
  };

  // Тело
  if (chunked) {
    for (;;) {
      String line = client.readStringUntil('\n');
      line.trim();
      if (line.length() == 0) continue;
      uint32_t chunkSize = strtoul(line.c_str(), nullptr, 16);
      if (chunkSize == 0) {
        // финальные заголовки после нулевого чанка (обычно пусто)
        client.readStringUntil('\n');
        break;
      }
      uint32_t remaining = chunkSize;
      while (remaining > 0) {
        size_t toRead = remaining > sizeof(buffer) ? sizeof(buffer) : remaining;
        int n = client.read(buffer, toRead);
        if (n <= 0) return false;
        appendData(buffer, (size_t)n);
        remaining -= (uint32_t)n;
      }
      // CRLF после чанка
      client.read(); client.read();
    }
  } else if (contentLength >= 0) {
    int remaining = contentLength;
    while (remaining > 0) {
      size_t toRead = remaining > (int)sizeof(buffer) ? sizeof(buffer) : remaining;
      int n = client.read(buffer, toRead);
      if (n <= 0) return false;
      appendData(buffer, (size_t)n);
      remaining -= n;
    }
  } else {
    // Без Content-Length — читаем до закрытия
    while (client.connected() || client.available()) {
      int n = client.read(buffer, sizeof(buffer));
      if (n > 0) appendData(buffer, (size_t)n);
      else delay(1);
    }
  }

  return true;
}

bool transcribeRecording(String &outText) {
  outText.clear();
  if (!openAiConfigured()) {
    setStatus("Укажите OPENAI_API_KEY в secrets.h");
    return false;
  }

  File file = SD_MMC.open(VOICE_RECORD_PATH, FILE_READ);
  if (!file) {
    setStatus("Файл записи не найден");
    return false;
  }
  size_t fileSize = file.size();
  if (fileSize <= 44) {
    setStatus("Запись пустая");
    file.close();
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(15000);
  client.setTimeout(15000);
  if (!client.connect(PROXY_HOST, 443)) {
    setStatus("Нет подключения к прокси (transcribe)");
    file.close();
    return false;
  }

  String boundary = "----ESP32VoiceBoundary";
  String partModel = "--" + boundary + "\r\n" +
                     "Content-Disposition: form-data; name=\"model\"\r\n\r\n" +
                     "gpt-4o-mini-transcribe\r\n";
  String partFormat = "--" + boundary + "\r\n" +
                      "Content-Disposition: form-data; name=\"response_format\"\r\n\r\n" +
                      "json\r\n";
  String partFileHeader = "--" + boundary + "\r\n" +
                          "Content-Disposition: form-data; name=\"file\"; filename=\"voice.wav\"\r\n" +
                          "Content-Type: audio/wav\r\n\r\n";
  String closing = "\r\n--" + boundary + "--\r\n";

  size_t contentLength = partModel.length() + partFormat.length() + partFileHeader.length() + fileSize + closing.length();

  client.print("POST /v1/audio/transcriptions HTTP/1.1\r\n");
  client.print("Host: "); client.print(PROXY_HOST); client.print("\r\n");
  client.print("Authorization: Bearer "); client.print(SERVICE_TOKEN); client.print("\r\n");
  client.print("Content-Type: multipart/form-data; boundary="); client.print(boundary); client.print("\r\n");
  client.print("Connection: close\r\n");
  client.print("Content-Length: "); client.print((unsigned long)contentLength); client.print("\r\n\r\n");

  client.print(partModel);
  client.print(partFormat);
  client.print(partFileHeader);

  uint8_t buffer[1024];
  size_t sent = 0;
  while (file.available()) {
    size_t toRead = file.read(buffer, sizeof(buffer));
    if (toRead == 0) {
      break;
    }
    client.write(buffer, toRead);
    sent += toRead;
    if (sent % 4096 == 0) {
      setStatus(String("Отправляем аудио (") + formatSize(sent) + ")");
    }
    pumpLvgl(0);
  }
  file.close();

  client.print(closing);

  int statusCode;
  String contentType;
  String body;
  bool ok = readHttpResponse(client, statusCode, contentType, body, nullptr);
  client.stop();
  if (!ok) {
    setStatus("Не удалось получить ответ транскрипции");
    return false;
  }

  if (statusCode != 200) {
    Serial.printf("[transcribe] HTTP %d\n", statusCode);
    Serial.println(body.substring(0, 512));
    setStatus(String("Ошибка транскрипции (HTTP ") + statusCode + ")");
    return false;
  }

  outText = jsonExtractString(body, "text");
  outText.trim();
  if (outText.isEmpty()) {
    setStatus("OpenAI не вернул текст");
    return false;
  }

  return true;
}

bool requestAssistantReply(const String &userText, String &assistantReply) {
  assistantReply.clear();
  if (!openAiConfigured()) {
    setStatus("Укажите OPENAI_API_KEY в secrets.h");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(15000);
  client.setTimeout(15000);

  if (!client.connect(PROXY_HOST, 443)) {
    setStatus("Нет подключения к прокси (chat)");
    return false;
  }

  // Формируем JSON-пэйлоад
  String payload = "{";
  payload += "\"model\":\"gpt-4o-mini\",";
  payload += "\"temperature\":0.7,";
  payload += "\"messages\":[";
  payload += "{\"role\":\"system\",\"content\":\"Ты дружелюбный голосовой ассистент. Отвечай кратко и по-русски.\"},";
  payload += "{\"role\":\"user\",\"content\":\"" + escapeJson(userText) + "\"}";
  payload += "]}";

  // HTTP/1.1 запрос
  client.print("POST /v1/chat/completions HTTP/1.1\r\n");
  client.print("Host: "); client.print(PROXY_HOST); client.print("\r\n");
  client.print("Authorization: Bearer "); client.print(SERVICE_TOKEN); client.print("\r\n");
  client.print("Content-Type: application/json\r\n");
  client.print("Accept: application/json\r\n");
  client.print("Accept-Encoding: identity\r\n"); // просим не gzip'ить
  client.print("Expect: \r\n");                   // отключаем 100-continue
  client.print("Connection: close\r\n");
  client.print("Content-Length: "); client.print((unsigned long)payload.length()); client.print("\r\n\r\n");
  client.print(payload);

  int statusCode;
  String contentType;
  String body;
  bool ok = readHttpResponse(client, statusCode, contentType, body, nullptr);
  client.stop();

  if (!ok) {
    setStatus("Не удалось получить ответ чат-модели");
    return false;
  }

  if (statusCode != 200) {
    // покажем первые ~1 КБ ответа в сериал для диагностики
    Serial.println("[chat] Non-200 response:");
    Serial.println(body.substring(0, 1024));
    setStatus(String("Ошибка ChatCompletions (HTTP ") + statusCode + ")");
    return false;
  }

  // Пытаемся достать текст максимально устойчиво
  assistantReply = extractAssistantContent(body);
  assistantReply.trim();

  if (assistantReply.isEmpty()) {
    // дополнительный фоллбэк: иногда content=null, а массив content[].text присутствует
    int choicesPos = body.indexOf("\"choices\"");
    if (choicesPos >= 0) {
      int msgPos  = body.indexOf("\"message\"", choicesPos);
      if (msgPos >= 0) {
        int textPos = body.indexOf("\"text\":\"", msgPos);
        if (textPos >= 0) {
          // простое извлечение после "text":
          int q = body.indexOf('"', body.indexOf(':', textPos));
          if (q >= 0) {
            String s = decodeJsonString(body, q + 1);
            s.trim();
            if (!s.isEmpty()) assistantReply = s;
          }
        }
      }
    }
  }

  if (assistantReply.isEmpty()) {
    Serial.println("[chat] Failed to extract assistant content. Body preview:");
    Serial.println(body.substring(0, 1024));
    setStatus("Не удалось извлечь ответ ассистента");
    return false;
  }

  return true;
}


bool synthesizeSpeech(const String &text, const char *destPath) {
  if (!openAiConfigured()) {
    setStatus("Укажите OPENAI_API_KEY в secrets.h");
    return false;
  }

  if (SD_MMC.exists(destPath)) {
    SD_MMC.remove(destPath);
  }
  File outFile = SD_MMC.open(destPath, FILE_WRITE);
  if (!outFile) {
    setStatus("Не удалось создать файл ответа");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(15000);
  client.setTimeout(15000);
  if (!client.connect(PROXY_HOST, 443)) {
    setStatus("Нет подключения к прокси (speech)");
    outFile.close();
    return false;
  }

  String payload = "{";
  payload += "\"model\":\"gpt-4o-mini-tts\",";
  payload += "\"voice\":\"" + String(OPENAI_VOICE) + "\",";
  payload += "\"format\":\"mp3\",";
  payload += "\"input\":\"" + escapeJson(text) + "\"}";

  client.print("POST /v1/audio/speech HTTP/1.1\r\n");
  client.print("Host: "); client.print(PROXY_HOST); client.print("\r\n");
  client.print("Authorization: Bearer "); client.print(SERVICE_TOKEN); client.print("\r\n");
  client.print("Content-Type: application/json\r\n");
  client.print("Accept: audio/mpeg\r\n");
  client.print("Connection: close\r\n");
  client.print("Content-Length: "); client.print(payload.length()); client.print("\r\n\r\n");
  client.print(payload);

  int statusCode;
  String contentType;
  String body;
  bool ok = readHttpResponse(client, statusCode, contentType, body, &outFile);
  client.stop();
  outFile.close();
  if (!ok) {
    setStatus("Не удалось получить аудио ответ");
    return false;
  }

  if (statusCode != 200) {
    Serial.println(body);
    setStatus(String("Ошибка синтеза (HTTP ") + statusCode + ")");
    return false;
  }

  if (SD_MMC.exists(destPath)) {
    File check = SD_MMC.open(destPath, FILE_READ);
    size_t sz = check ? check.size() : 0;
    if (check) {
      check.close();
    }
    if (sz == 0) {
      setStatus("Полученный аудио файл пуст");
      return false;
    }
  }
  return true;
}

bool processVoicePipeline() {
  String transcript;
  setStatus("Расшифровка речи...");
  if (!transcribeRecording(transcript)) {
    return false;
  }
  last_user_transcript = transcript;
  setStatus(String("Вы сказали: \n") + transcript + "\nИдёт генерация ответа...");

  String assistantReply;
  if (!requestAssistantReply(transcript, assistantReply)) {
    return false;
  }
  last_assistant_reply = assistantReply;
  setStatus(String("Ответ: \n") + assistantReply + "\nСинтез аудио...");

  if (!synthesizeSpeech(assistantReply, ASSISTANT_RESPONSE_PATH)) {
    return false;
  }

  if (!playAudioFromSd(ASSISTANT_RESPONSE_PATH, "assistant.mp3")) {
    return false;
  }
  assistant_state = AssistantState::Speaking;
  updateAssistantButton();
  return true;
}

void handleVoiceAssistant() {
  if (assistant_state == AssistantState::Recording) {
    handleVoiceRecording();
    return;
  }

  if (assistant_state == AssistantState::Processing) {
    if (!assistant_task_running && upload_requested) {
      assistant_task_running = true;
      bool ok = processVoicePipeline();
      if (!ok) {
        assistant_state = AssistantState::Idle;
        updateAssistantButton();
      }
      upload_requested = false;
      assistant_task_running = false;
    }
    return;
  }

  if (assistant_state == AssistantState::Speaking) {
    if (!audio.isRunning()) {
      assistant_state = AssistantState::Idle;
      if (!last_assistant_reply.isEmpty()) {
        setStatus(last_assistant_reply);
      } else {
        setStatus("Готов к следующему запросу");
      }
      updateAssistantButton();
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);  // Даем время USB-UART
  Serial.println("BOOT OK - LVGL Wi-Fi image demo");

  I2C_Init();
  TCA9554PWR_Init();
  Backlight_Init();
  LCD_Init();
  Lvgl_Init();
  Audio_Init();

  lv_obj_clean(lv_scr_act());

  status_label = lv_label_create(lv_scr_act());
  lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 10);
  setStatus("Подготовка...");

  sd_ready = initializeSdCard();

  image_obj = lv_img_create(lv_scr_act());
  lv_obj_center(image_obj);
  lv_obj_add_flag(image_obj, LV_OBJ_FLAG_HIDDEN);

  assistant_button = lv_btn_create(lv_scr_act());
  lv_obj_set_size(assistant_button, 200, 60);
  lv_obj_align(assistant_button, LV_ALIGN_BOTTOM_MID, 0, -20);
  assistant_button_label = lv_label_create(assistant_button);
  lv_label_set_text(assistant_button_label, "Говорить");
  lv_obj_center(assistant_button_label);
  lv_obj_add_event_cb(assistant_button, assistant_button_event_cb, LV_EVENT_CLICKED, NULL);
  updateAssistantButton();

  if (!connectToWifi()) {
    return;
  }

/*
  setStatus("Загружается изображение...");
  bool downloaded = downloadImage(IMAGE_URL);
  bool decoded = downloaded ? convertBmpToLvglImage(image_data) : false;

  if (downloaded && decoded) {
    lv_obj_clear_flag(image_obj, LV_OBJ_FLAG_HIDDEN);
    lv_img_set_src(image_obj, &remote_image_dsc);
    lv_obj_center(image_obj);

    String bytes = String(image_data.size());
    String dims = String(decoded_width) + "x" + String(decoded_height);
    setStatus(String("Изображение загружено\n") + dims + " (" + bytes + " байт)");
  } else {
    lv_obj_add_flag(image_obj, LV_OBJ_FLAG_HIDDEN);
    if (!downloaded) {
      setStatus("Не удалось загрузить изображение");
    }
  }

  if (sd_ready && WiFi.status() == WL_CONNECTED) {
    setStatus("Скачиваем аудио...");
    bool audioFileReady = downloadFileToSD(MP3_URL, MP3_DEST_PATH, MP3_FILE_NAME);
    if (audioFileReady) {
      audio_playback_started = playAudioFromSd(MP3_DEST_PATH, MP3_FILE_NAME);
    }
  }
  */
}

void loop() {
  Lvgl_Loop();
  handleVoiceAssistant();
  delay(5);
}
