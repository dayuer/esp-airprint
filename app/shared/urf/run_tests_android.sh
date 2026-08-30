#!/usr/bin/env bash
# 在连着的 Android 设备上跑 URF 编码器单测。
#
# 为什么要在真机上跑一遍：主机是 arm64 macOS，和 Android 同架构不同 libc、
# 不同文件系统。第一次跑就抓到了两个只在真机暴露的问题——测试产物写死在 /tmp
# （Android 没有这个目录），以及校验器把整个文件读进内存（A4 产物 35MB，
# 峰值 71MB）。
#
# 用法：  ./run_tests_android.sh [ABI]
# 默认 ABI 取设备自报的 ro.product.cpu.abi。
set -euo pipefail
cd "$(dirname "$0")"

SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}}"
NDK="${ANDROID_NDK_HOME:-$(ls -d "$SDK"/ndk/* 2>/dev/null | sort -V | tail -1)}"
[ -d "$NDK" ] || { echo "找不到 NDK，设 ANDROID_NDK_HOME"; exit 1; }

adb get-state >/dev/null 2>&1 || { echo "没有连接的设备。adb devices 看看"; exit 1; }
ABI="${1:-$(adb shell getprop ro.product.cpu.abi | tr -d '\r')}"
DEVDIR=/data/local/tmp/urf

echo "NDK $NDK"
echo "ABI $ABI"

cmake -S . -B build-android \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI="$ABI" -DANDROID_PLATFORM=android-24 \
  -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build-android --parallel

adb shell "mkdir -p $DEVDIR"
adb push build-android/urf_tests "$DEVDIR/" >/dev/null
adb push tests/fixtures/. "$DEVDIR/fixtures/" >/dev/null
adb shell "chmod 755 $DEVDIR/urf_tests"

# URF_TEST_TMPDIR 让测试把产物写到设备可写目录。
set +e
adb shell "cd $DEVDIR && URF_TEST_TMPDIR=$DEVDIR URF_FIXTURE_DIR=$DEVDIR/fixtures ./urf_tests"
rc=$?
set -e

# A4 整页产物 35MB，跑完就删，别留在设备上。
adb shell "rm -rf $DEVDIR/*.urf $DEVDIR/fixtures"
exit $rc
