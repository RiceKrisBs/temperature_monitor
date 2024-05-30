#include <DHT.h>
#include <ESP8266WiFi.h>
#include <ESP_Mail_Client.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

#include "temperature_config.h"

// D0 and RST pins must be connected for the board to wake itself up from deep sleep
// These pins *cannot* be connected when uploading a new sketch to the board

const long utcOffsetInSeconds = -14400;
const bool testMode = false;

SMTPSession smtp;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffsetInSeconds);
DHT dht(DHTPIN, DHTTYPE);

// For convenience in reading/reporting temp & humidity values
struct Conditions {
  float temperature;
  float humidity;
};

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("Begin setup...");
  delay(10000);

  connectToWiFi();
  timeClient.begin();

  if (!shouldSendEmail()) {
    goToDeepSleep();
  }

  Conditions conditions = readTemperatureAndHumidity();
  Session_Config config = createMailConfig();
  SMTP_Message message = createMailMessage(conditions.temperature, conditions.humidity);
  sendEmail(config, message, testMode);

  int window_duration = (LATE_WINDOW_HOUR - EARLY_WINDOW_HOUR) * 3600 + (LATE_WINDOW_MINUTE - EARLY_WINDOW_MINUTE) * 60;
  Serial.print("Time to wait before deep sleep: ");
  Serial.println(window_duration);
  delay(window_duration * 1000);
  goToDeepSleep();
}

void loop() {
  // Empty as email functionality is a one-time event and then we put the board into deep sleep
}

void connectToWiFi() {
  WiFi.hostname(WIFI_HOST_NAME);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  Serial.println();
}

bool shouldSendEmail() {
  timeClient.update();

  // Emails should not be sent on weekends
  int dayOfWeek = timeClient.getDay(); // 0 = Sunday, 1 = Monday, ...
  if (dayOfWeek == 0 || dayOfWeek == 6) {
    Serial.println("Emails are not sent on weekends");
    return false;
  }

  // Emails should only be sent between EARLY_WINDOW and LATE_WINDOW times.
  // This protects against power disruption and sending an email on startup.
  uint32_t now = timeClient.getEpochTime();
  uint32_t earlyCutoffTime = now - (now % 86400) + EARLY_WINDOW_HOUR * 3600 + EARLY_WINDOW_MINUTE * 60;
  uint32_t lateCutoffTime = now - (now % 86400) + LATE_WINDOW_HOUR * 3600 + LATE_WINDOW_MINUTE * 60;

  bool isEarlyMorning = (now >= earlyCutoffTime && now <= lateCutoffTime);
  if (!isEarlyMorning) {
    Serial.println("It is outside the allocated window");
  }
  return isEarlyMorning;
}

void goToDeepSleep() {
  uint32_t sleepDuration = calculateSleepDuration();
  if (sleepDuration < 0) {
    sleepDuration += 24 * 3600;
  }
  Serial.print("Seconds to sleep: ");
  Serial.println(sleepDuration);
  ESP.deepSleep(sleepDuration * 1000000, WAKE_RF_DEFAULT);
}

uint32_t calculateSleepDuration() {
  timeClient.update();
  uint32_t now = timeClient.getEpochTime();
  uint32_t currentMidnight = (now / 86400) * 86400;
  uint32_t nextMidnight = currentMidnight + 86400;
  uint32_t midnight;

  if (timeClient.getHours() < SEND_HOUR) {
    midnight = currentMidnight;
  }
  else if (timeClient.getHours() > SEND_HOUR) {
    midnight = nextMidnight;
  }
  else {
    // it's during the send hour
    if (timeClient.getMinutes() < SEND_MINUTE) {
      midnight = currentMidnight;
    }
    else {
      // it can't be equal to the send minute, otherwise it would actually send
      midnight = nextMidnight;
    }
  }

  uint32_t wakeupEpoch = midnight + SEND_HOUR * 3600 + SEND_MINUTE * 60;
  int32_t sleepDuration = wakeupEpoch - now;

  return sleepDuration;
}

Conditions readTemperatureAndHumidity() {
  Conditions conditions;
  dht.begin();
  delay(3000);

  float temperatureCelsiusReading = dht.readTemperature();
  conditions.humidity = dht.readHumidity();

  if (isnan(temperatureCelsiusReading) || isnan(conditions.humidity)) {
    Serial.println("Failed to read from DHT sensor.");
    return conditions; 
  }

  conditions.temperature = (temperatureCelsiusReading * 9.0 / 5.0) + 32.0;
  return conditions;
}

Session_Config createMailConfig() {
  Session_Config config;

  MailClient.networkReconnect(true);

  // Enable the debug via Serial port
  // 0 for no debugging
  // 1 for basic level debugging
  // Debug port can be changed via ESP_MAIL_DEFAULT_DEBUG_PORT in ESP_Mail_FS.h
  smtp.debug(0);

  // set callback for sending results 
  smtp.callback(smtpCallback);

  // set session config
  config.server.host_name = SMTP_HOST;
  config.server.port = SMTP_PORT;
  config.login.email = AUTHOR_EMAIL;
  config.login.password = AUTHOR_APP_PASSWORD;
  config.login.user_domain = "";

  // set NTP config time for email
  config.time.ntp_server = F("pool.ntp.org,time.nist.gov");
  config.time.gmt_offset = GMT_OFFSET;
  config.time.day_light_offset = DAY_LIGHT_OFFSET;

  return config;
}

void smtpCallback(SMTP_Status status){
  // Print the current status
  Serial.println(status.info());

  // Print the sending result
  if (status.success()){
    Serial.println("----------------");
    Serial.printf("Message sent success: %d\n", status.completedCount());
    Serial.printf("Message sent failed: %d\n", status.failedCount());
    Serial.println("----------------\n");

    for (size_t i = 0; i < smtp.sendingResult.size(); i++)
    {
      // Get the result item
      SMTP_Result result = smtp.sendingResult.getItem(i);

      // In case, ESP32, ESP8266 and SAMD device, the timestamp get from result.timestamp should be valid if
      // your device time was synched with NTP server.
      // Other devices may show invalid timestamp as the device time was not set i.e. it will show Jan 1, 1970.
      // You can call smtp.setSystemTime(xxx) to set device time manually. Where xxx is timestamp (seconds since Jan 1, 1970)
      
      Serial.printf("Message No: %d\n", i + 1);
      Serial.printf("Status: %s\n", result.completed ? "success" : "failed");
      Serial.printf("Date/Time: %s\n", MailClient.Time.getDateTimeString(result.timestamp, "%B %d, %Y %H:%M:%S").c_str());
      Serial.printf("Recipient: %s\n", result.recipients.c_str());
      Serial.printf("Subject: %s\n", result.subject.c_str());
    }
    Serial.println("----------------\n");

    // You need to clear sending result as the memory usage will grow up.
    smtp.sendingResult.clear();
  }
}

SMTP_Message createMailMessage(float temperature, float humidity) {
  SMTP_Message message;

  message.sender.name = AUTHOR_NAME;
  message.sender.email = AUTHOR_EMAIL;
  message.subject = EMAIL_SUBJECT;
  message.addRecipient(RECIPIENT_NAME, RECIPIENT_EMAIL);

  String textMsg = createEmailBody(temperature, humidity);

  message.text.content = textMsg.c_str();
  message.text.charSet = "us-ascii";
  message.text.transfer_encoding = Content_Transfer_Encoding::enc_7bit;
  
  message.priority = esp_mail_smtp_priority::esp_mail_smtp_priority_low;
  message.response.notify = esp_mail_smtp_notify_success | esp_mail_smtp_notify_failure | esp_mail_smtp_notify_delay;

  return message;
}

String createEmailBody(float temperature, float humidity) {
  String message = "";
  message += "The current temperature in the Manchester office is " + String(temperature, 0) + "*F.";
  // message += "\nThe current humidity is " + String(humidity, 0) + "%."; // commented out as DHT11 is too imprecise for measuring the humidity at the office
  return message;
}

void sendEmail(Session_Config &config, SMTP_Message &message, bool testMode) {
  if (testMode) {
    Serial.println("In test mode. Simulated Email sent.");
    return;
  }
  // connect to server
  if (!smtp.connect(&config)){
    Serial.printf("Connection error, Status Code: %d, Error Code: %d, Reason: %s", smtp.statusCode(), smtp.errorCode(), smtp.errorReason().c_str());
    return;
  }

  if (!smtp.isLoggedIn()){
    Serial.println("\nNot yet logged in.");
  }
  else{
    if (smtp.isAuthenticated())
      Serial.println("\nSuccessfully logged in.");
    else
      Serial.println("\nConnected with no Auth.");
  }

  // Start sending Email and close the session
  if (!MailClient.sendMail(&smtp, &message))
    Serial.printf("Error, Status Code: %d, Error Code: %d, Reason: %s", smtp.statusCode(), smtp.errorCode(), smtp.errorReason().c_str());
}
