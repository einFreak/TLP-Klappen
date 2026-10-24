#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

// Registers API routes, static file handler and SSE endpoint.
void startWebServer();
void stopWebServer();
// Periodically pushes live status over SSE.
void streamApStatus();
// Prints AP diagnostics to serial output.
void printApStatus();
