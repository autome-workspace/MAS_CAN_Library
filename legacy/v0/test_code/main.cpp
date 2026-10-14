#include <Arduino.h>
#include "MotorCAN.h"

MotorCAN motorCAN(GPIO_NUM_5, GPIO_NUM_4);//CAN_TX:GPIO5, CAN_RX:GPIO4

void setup()
{
    motorCAN.begin();
}

void loop()
{
    motorCAN.setCurrent(1, 20);//1番のモーターを20mAで駆動する
    delay(20);
}