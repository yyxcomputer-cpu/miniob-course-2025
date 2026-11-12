#!/usr/bin/env bash
# Build directly from the delivered source archive without a .git directory.
set -euo pipefail
course_root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
course_prefix="$course_root/deps/3rd/usr/local"
course_jobs=${COURSE_JOBS:-4}
course_asan=${COURSE_ASAN:-OFF}
course_cmake=(-DCMAKE_POLICY_VERSION_MINIMUM=3.10 -DCMAKE_BUILD_TYPE=Release)

for course_dep in libevent jsoncpp replxx; do
  test -f "$course_root/deps/3rd/$course_dep/CMakeLists.txt" || {
    echo "Missing bundled dependency: $course_dep" >&2; exit 1;
  }
  course_opts=()
  case "$course_dep" in
    libevent) course_opts=(-DEVENT__DISABLE_OPENSSL=ON -DEVENT__DISABLE_TESTS=ON -DEVENT__DISABLE_REGRESS=ON -DEVENT__DISABLE_SAMPLES=ON -DEVENT__LIBRARY_TYPE=BOTH) ;;
    jsoncpp) course_opts=(-DJSONCPP_WITH_TESTS=OFF -DJSONCPP_WITH_POST_BUILD_UNITTEST=OFF) ;;
    replxx) course_opts=(-DREPLXX_BUILD_EXAMPLES=OFF -DREPLXX_BUILD_PACKAGE=OFF) ;;
  esac
  cmake -S "$course_root/deps/3rd/$course_dep" -B "$course_root/build-course-deps/$course_dep" \
    "${course_cmake[@]}" -DCMAKE_INSTALL_PREFIX="$course_prefix" "${course_opts[@]}"
  cmake --build "$course_root/build-course-deps/$course_dep" -j "$course_jobs"
  cmake --install "$course_root/build-course-deps/$course_dep"
done
cmake -S "$course_root" -B "$course_root/build-course" "${course_cmake[@]}" \
  -DENABLE_ASAN="$course_asan" -DWITH_MEMTRACER=OFF -DWITH_UNIT_TESTS=OFF -DWITH_CPPLINGS=OFF
cmake --build "$course_root/build-course" --target observer -j "$course_jobs"
echo "Built $course_root/build-course/bin/observer"
