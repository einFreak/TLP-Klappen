#include <cassert>
#include <cstring>
#include <initializer_list>
#include "serial_protocol.h"

int main() {
  unsigned long value;
  assert(parseUnsignedValue("0", 31, value) && value == 0);
  assert(parseUnsignedValue("31", 31, value) && value == 31);
  assert(parseUnsignedValue("1023", 1023, value) && value == 1023);
  assert(parseUnsignedValue("4294967295", 4294967295UL, value) && value == 4294967295UL);
  for (const char *invalid : {"", "-1", "+1", "1.5", "12x", " 1", "32", "999999999999999999999999"}) {
    assert(!parseUnsignedValue(invalid, 31, value));
  }
  assert(!parseUnsignedValue("4294967296", 4294967295UL, value));
  SerialRequest request = {};
  char hello[] = "TLP1 1 HELLO 0\r";
  assert(parseSerialRequest(hello, request));
  assert(request.id == 1 && request.session == 0 && request.argumentCount == 0);
  assert(strcmp(request.operation, "HELLO") == 0);
  char flap[] = "TLP1 2147483647 FLAP 4294967295 31 reset";
  assert(parseSerialRequest(flap, request));
  assert(request.id == 2147483647UL && request.session == 4294967295UL && request.argumentCount == 2);
  assert(strcmp(request.arguments[0], "31") == 0 && strcmp(request.arguments[1], "reset") == 0);
  const char *invalidLines[] = {"", "TLP0 1 HELLO 0", "TLP1 0 HELLO 0", "TLP1 -1 HELLO 0", "TLP1 2147483648 HELLO 0",
    "TLP1 1 HELLO -1", "TLP1 1 HELLO 4294967296", "TLP1 1 HELLO", "TLP1 1 FLAP 1 2 up extra overflow"};
  for (const char *invalid : invalidLines) {
    char line[128];
    strcpy(line, invalid);
    assert(!parseSerialRequest(line, request));
  }
}