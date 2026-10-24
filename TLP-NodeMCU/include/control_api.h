#pragma once

#include "can_bus.h"
#include "serial_protocol.h"

PendingCommand *submitFlapCommand(uint8_t nodeId, const char *action, const char *&error);
PendingCommand *submitThresholdCommand(uint8_t nodeId, unsigned int value, const char *&error);