#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <MQ135.h>

/************ LCD ************/
LiquidCrystal_I2C lcd(0x27, 16, 2);

/************ PINS ************/
#define DHT_PIN   12
#define FAN_PIN   14
#define DHTTYPE   DHT11
#define MQ135_PIN A0

/************ OBJECTS ************/
DHT dht(DHT_PIN, DHTTYPE);
MQ135 gasSensor(MQ135_PIN);

/************ VARIABLES ************/
const int screenDelay = 2000;   // 2 sec per screen


/************************************************
   SETUP
************************************************/
void setup() {

  Serial.begin(115200);
lcd.init();  // Initialize the LCD
lcd.backlight();  // Already have this
  lcd.clear();

  dht.begin();

  pinMode(FAN_PIN, OUTPUT);
  pinMode(MQ135_PIN, INPUT);

  Serial.println("MQ135 warming up...");
  delay(2000);   // MQ warm-up
}


/************************************************
   LOOP (3 screens one by one)
************************************************/
void loop() {

  showWelcomeScreen();
  delay(screenDelay);

  showDHTScreen();
  delay(screenDelay);

  showGasScreen();
  delay(screenDelay);
}


/************************************************
   SCREEN 1 : WELCOME
************************************************/
void showWelcomeScreen() {

  lcd.clear();
  lcd.setCursor(3,0);
  lcd.print("WELCOME TO");

  lcd.setCursor(0,1);
 lcd.print("AgriSafe System");
}


/************************************************
   SCREEN 2 : DHT + FAN ONLY
************************************************/
void showDHTScreen() {

  float t = dht.readTemperature();
  float h = dht.readHumidity();

  lcd.clear();

  lcd.setCursor(0,0);
  lcd.print("Temp:");
  lcd.print(t,1);
  lcd.print("C");

  lcd.setCursor(0,1);
  lcd.print("Hum :");
  lcd.print(h,1);
  lcd.print("%");

  // FAN CONTROL ONLY BY TEMP
  if (t > 30) {
    digitalWrite(FAN_PIN, HIGH);
    Serial.println("Fan ON");
  }
  else {
    digitalWrite(FAN_PIN, LOW);
    Serial.println("Fan OFF");
  }
}


/************************************************
   SCREEN 3 : MQ135 AIR QUALITY ONLY
************************************************/
void showGasScreen() {

  float ppm = gasSensor.getPPM();

  lcd.clear();

  lcd.setCursor(5,0);
  lcd.print("PPM:");
  lcd.print(ppm,0);

  lcd.setCursor(0,1);

  if (ppm > 120) {
    lcd.print("Bad Air Quality");
  }
  else {
    lcd.print("Good Air Quality");
  }

  Serial.print("Gas PPM: ");
  Serial.println(ppm);
}
