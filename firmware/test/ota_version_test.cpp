#include "ota_version.h"
#include <cassert>
#include <cstring>
#include <initializer_list>
int main() {
  for (const char *v : {"1.0.1", "1.10.0", "2.0.0"})
    assert(ota_version_higher(v, strlen(v)+1, "1.0.0", 6));
  for (const char *v : {"1.0.0", "0.99.99", "1.0", "01.0.1", "1.0.1-rc1", "4294967296.0.0", "", "-1.0.0"})
    assert(!ota_version_higher(v, strlen(v)+1, "1.0.0", 6));
  assert(!ota_version_higher("1.0.1", 5, "1.0.0", 6));
  assert(!ota_version_higher("1.0.1", 6, "bad", 4));
  assert(ota_version_higher("1.10.0", 7, "1.9.99", 7));
}
