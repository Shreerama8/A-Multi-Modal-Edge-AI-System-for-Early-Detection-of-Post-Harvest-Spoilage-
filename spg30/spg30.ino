 #include "SparkFun_SGP30_Arduino_Library.h" // Click here to get the library: http://librarymanager/All#SparkFun_SGP30
#include <Wire.h>

SGP30 mySensor; //create an object of the SGP30 class
long t1, t2;
void setup() {
  Serial.begin(9600);
  Wire.begin();
  //Sensor supports I2C speeds up to 400kHz
  Wire.setClock(400000);
  //Initialize sensor
  if (mySensor.begin() == false) {
    Serial.println("No SGP30 Detected. Check connections.");
    while (1);
  }
  //Initializes sensor for air quality readings
  //measureAirQuality should be called in one second increments after a call to initAirQuality
  mySensor.initAirQuality();
  t1 = millis();
}

void loop() {
  //First fifteen readings will be
  //CO2: 400 ppm  TVOC: 0 ppb
  t2 = millis();
  if ( t2 >= t1 + 1000) //only will occur if 1 second has passed
  {
    t1 = t2;
    //measure CO2 and TVOC levels
    mySensor.measureAirQuality();
    Serial.print("CO2: ");
    Serial.print(mySensor.CO2);
    Serial.print(" ppm\tTVOC: ");
    Serial.print(mySensor.TVOC);
    Serial.println(" ppb");
    //get raw values for H2 and Ethanol
    mySensor.measureRawSignals();
    Serial.print("Raw H2: ");
    Serial.print(mySensor.H2);
    Serial.print(" \tRaw Ethanol: ");
    Serial.println(mySensor.ethanol);
  }
}
