# vcpkg downstream fixture

This fixture consumes `video-server` through the local overlay port in `../../vcpkg/`.

```bash
cmake -S tests/vcpkg_downstream -B build/vcpkg_downstream \
  -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_OVERLAY_PORTS="${PWD}/vcpkg"
cmake --build build/vcpkg_downstream
```
