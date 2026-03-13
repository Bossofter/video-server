#include <exception>
#include <iostream>

int test_core();
int test_transforms();
int test_synthetic();
int test_webrtc_http();

int main() {
  try {
    if (test_core() != 0) return 1;
    if (test_transforms() != 0) return 1;
    if (test_synthetic() != 0) return 1;
    if (test_webrtc_http() != 0) return 1;
  } catch (const std::exception& e) {
    std::cerr << "Unhandled exception: " << e.what() << '\n';
    return 1;
  }
  return 0;
}
