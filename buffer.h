#pragma once

#include <Arduino.h>

namespace buffer {

void   begin();
size_t count();
bool   push(const String &json);
bool   peekOldest(String &out);
bool   removeOldest();
void   clear();

}  // namespace buffer
