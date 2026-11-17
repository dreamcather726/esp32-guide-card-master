#pragma once

// Optional, ignored local credentials file. Never copy the password to logs.
#if __has_include("position_mqtt_local.h")
#include "position_mqtt_local.h"
#endif
#ifndef JIUCHUAN_POSITION_MQTT_URI
#define JIUCHUAN_POSITION_MQTT_URI "mqtt://iot.suji.work:1883"
#endif
#ifndef JIUCHUAN_POSITION_MQTT_USERNAME
#define JIUCHUAN_POSITION_MQTT_USERNAME "test"
#endif
#ifndef JIUCHUAN_POSITION_MQTT_PASSWORD
#define JIUCHUAN_POSITION_MQTT_PASSWORD ""
#endif
