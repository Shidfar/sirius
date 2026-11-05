#!/usr/bin/env bash
# Downloads what gets built into the binary: ONNX Runtime (C headers + the shared library) and the GigaAM-v3 int8 models.
#
#   ./fetch.sh              for this machine's CPU
#   ./fetch.sh aarch64      for a 64-bit Raspberry Pi (also when run on an x86_64 PC)
#   ./fetch.sh x86_64 aarch64
#
# Needs: curl, unzip. Result: ort/include/, ort/lib-<arch>/libonnxruntime.so, models/*.
set -euo pipefail
cd "$(dirname "$0")"

ORT_VERSION="${ORT_VERSION:-1.31.1}"
ARCHS=("$@"); [ ${#ARCHS[@]} -gt 0 ] || ARCHS=("$(uname -m)")

mkdir -p deps/src ort
NUPKG="deps/src/onnxruntime-$ORT_VERSION.nupkg"
need_pkg=0
[ -f ort/include/onnxruntime_c_api.h ] || need_pkg=1
for a in "${ARCHS[@]}"; do [ -f "ort/lib-$a/libonnxruntime.so" ] || need_pkg=1; done
if [ $need_pkg = 1 ] && [ ! -f "$NUPKG" ]; then
  echo "== ONNX Runtime $ORT_VERSION"
  curl -fL --retry 3 -o "$NUPKG.part" \
    "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/$ORT_VERSION/microsoft.ml.onnxruntime.$ORT_VERSION.nupkg"
  mv "$NUPKG.part" "$NUPKG"
fi
if [ ! -f ort/include/onnxruntime_c_api.h ]; then
  mkdir -p ort/include
  unzip -q -o -j "$NUPKG" 'build/native/include/*' -d ort/include
fi
for a in "${ARCHS[@]}"; do
  case "$a" in
    x86_64)  RID=linux-x64 ;;
    aarch64) RID=linux-arm64 ;;
    *) echo "Unsupported CPU $a: this ONNX Runtime package only ships x86_64 and aarch64 (use 64-bit Pi OS)." >&2; exit 1 ;;
  esac
  if [ ! -f "ort/lib-$a/libonnxruntime.so" ]; then
    mkdir -p "ort/lib-$a"
    unzip -q -o -j "$NUPKG" "runtimes/$RID/native/libonnxruntime.so" -d "ort/lib-$a"
  fi
done

HF_REV="${HF_REV:-322c3b29492673eb7d0b434bfa9dfb8653e34d02}"   # tested revision; set HF_REV=main for the latest
HF=https://huggingface.co/istupakov/gigaam-v3-onnx/resolve/$HF_REV
mkdir -p models
for f in v3_e2e_rnnt_encoder.int8.onnx v3_e2e_rnnt_decoder.int8.onnx v3_e2e_rnnt_joint.int8.onnx v3_e2e_rnnt_vocab.txt; do
  if [ ! -f "models/$f" ]; then echo "== $f"; curl -fL --retry 3 -o "models/$f.part" "$HF/$f" && mv "models/$f.part" "models/$f"; fi
done
[ -f models/gigaam_v3.onnx ] || { echo "models/gigaam_v3.onnx (feature extractor) is missing - it ships with this project." >&2; exit 1; }
echo "done."
