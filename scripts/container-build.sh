#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C
printf '==> Installing build tools inside the disposable container\n'
dnf -y -q install gcc gcc-c++ cmake make python3 curl tar gzip binutils patch
mkdir -p /work /export/licenses /export/third-party-source
printf '==> Fetching and verifying FDK-AAC source\n'
python3 /source/scripts/fetch.py
cmake -S /work/fdk -B /work/fdk-build -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF -DBUILD_PROGRAMS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_INSTALL_PREFIX=/work/fdk-install -DCMAKE_INSTALL_LIBDIR=lib
cmake --build /work/fdk-build --parallel "${AAC_BUILD_JOBS:-4}"
cmake --install /work/fdk-build
printf '==> Building the decoder plugin and running codec tests\n'
cmake -S /source -B /work/plugin-build -DCMAKE_BUILD_TYPE=Release \
    -DRESOLVE_SDK=/sdk -DFDK_ROOT=/work/fdk-install -DCMAKE_INSTALL_PREFIX=/export
cmake --build /work/plugin-build --parallel "${AAC_BUILD_JOBS:-4}"
(cd /work/plugin-build && ctest --output-on-failure)
cmake --install /work/plugin-build
strip --strip-unneeded /export/aac_decoder_probe.dvcp.bundle/Contents/Linux-x86-64/aac_decoder_probe.dvcp
strip --strip-unneeded /export/tools/check-resolve
if [ "${AAC_WITH_ENCODER:-0}" = 1 ]; then
    printf '==> Building the optional AAC export plugin\n'
    (cd /work/encoder && patch -p1 < /source/packaging/encoder-esds.patch)
    mkdir -p /export/aac_fdk_plugin.dvcp.bundle/Contents/Linux-x86-64
    (cd /work/encoder/src && g++ -std=c++11 -O2 -fPIC -shared -I. -Iinclude -I/work/fdk-install/include \
      plugin.cpp aac_encoder.cpp wrapper/host_api.cpp wrapper/plugin_api.cpp \
      /work/fdk-install/lib/libfdk-aac.a -Wl,-z,defs -lpthread -lm \
      -o /export/aac_fdk_plugin.dvcp.bundle/Contents/Linux-x86-64/aac_fdk_plugin.dvcp)
    strip --strip-unneeded /export/aac_fdk_plugin.dvcp.bundle/Contents/Linux-x86-64/aac_fdk_plugin.dvcp
    cp /work/encoder/LICENSE /export/licenses/aac-encoder.txt
    cp /source/packaging/encoder-esds.patch /export/third-party-source/
fi
cp /work/fdk/NOTICE /export/licenses/fdk-aac.txt
cp /source/packaging/README.txt /export/README.txt
cp /source/packaging/install.py /export/install.py
cp /source/NOTICE.txt /export/NOTICE.txt
if [ -f /source/LICENSE ]; then cp /source/LICENSE /export/LICENSE; fi
cp /source/sources.lock.json /export/sources.lock.json
python3 /source/scripts/validate.py
printf '==> Portable package built and verified\n'
