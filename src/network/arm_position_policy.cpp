#include "network/arm_position_policy.h"

#include <cstring>

bool armPositionSourceRequiresArrivalTaskId(const char* source) {
  if (source == nullptr) {
    return false;
  }
  return std::strcmp(source, "camera_capture") == 0 ||
    std::strcmp(source, "camera_batch_return") == 0;
}
