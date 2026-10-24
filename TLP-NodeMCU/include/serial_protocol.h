#pragma once

#include <stddef.h>
#include <string.h>

inline bool parseUnsignedValue(const char *text, unsigned long maximum, unsigned long &value) {
  if (text == nullptr || *text == '\0') return false;
  value = 0;
  for (const char *digit = text; *digit; digit++) {
    if (*digit < '0' || *digit > '9') return false;
    const unsigned long next = static_cast<unsigned long>(*digit - '0');
    if (next > maximum || value > (maximum - next) / 10) return false;
    value = value * 10 + next;
  }
  return true;
}

struct SerialRequest {
  unsigned long id;
  unsigned long session;
  const char *operation;
  const char *arguments[3];
  size_t argumentCount;
};

inline bool parseSerialRequest(char *line, SerialRequest &request) {
  char *tokens[7];
  size_t count = 0;
  char *cursor = line;
  while (*cursor) {
    while (*cursor == ' ' || *cursor == '\r') cursor++;
    if (!*cursor) break;
    if (count == 7) return false;
    tokens[count++] = cursor;
    while (*cursor && *cursor != ' ' && *cursor != '\r') cursor++;
    if (*cursor) *cursor++ = '\0';
  }
  if (count < 4 || strcmp(tokens[0], "TLP1") != 0 ||
      !parseUnsignedValue(tokens[1], 2147483647UL, request.id) || request.id == 0 ||
      !parseUnsignedValue(tokens[3], 4294967295UL, request.session)) return false;
  request.operation = tokens[2];
  request.argumentCount = count - 4;
  for (size_t index = 0; index < request.argumentCount; index++) request.arguments[index] = tokens[index + 4];
  return true;
}