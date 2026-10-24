#pragma once

#include <Arduino.h>
#include "can_bus.h"
#include "node_state.h"

extern unsigned long maxLoopDurationUs;

void writeCentralStatusJson(Print &out, bool local, bool closeObject = true);
void writeNodeStatusJson(Print &out, const NodeState &state);
void writeStatusJson(Print &out, bool includeNodes);
void writeCommandResultJson(Print &out, const PendingCommand &entry);