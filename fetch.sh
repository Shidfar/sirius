#!/usr/bin/env bash
# Downloads ONNX Runtime (C headers + shared library) and the GigaAM-v3 int8 models.
# Needs: curl, unzip. Works on x86_64 and aarch64 (64-bit Raspberry Pi OS).
set -euo pipefail
cd "$(dirname "$0")"

ORT_VERSION="${ORT_VERSION:-1.31.1}"
case "$(uname -m)" in
  x86_64)  RID=linux-x64 ;;
  aarch64) RID=linux-arm64 ;;
  *) echo "Unsupported CPU $(uname -m): this ONNX Runtime package only ships x86_64 and aarch64 (use 64-bit Pi OS)." >&2; exit 1 ;;
esac

if [ ! -f ort/lib/libonnxruntime.so ]; then
  echo "== ONNX Runtime $ORT_VERSION ($RID)"
  tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
  curl -fL --retry 3 -o "$tmp/ort.nupkg" \
    "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/$ORT_VERSION/microsoft.ml.onnxruntime.$ORT_VERSION.nupkg"
  mkdir -p ort/include ort/lib
  unzip -q -j "$tmp/ort.nupkg" 'build/native/include/*' -d ort/include
  unzip -q -j "$tmp/ort.nupkg" "runtimes/$RID/native/libonnxruntime.so" -d ort/lib
fi
ln -sf libonnxruntime.so ort/lib/libonnxruntime.so.1   # the library's SONAME; also repairs an interrupted run

HF_REV="${HF_REV:-322c3b29492673eb7d0b434bfa9dfb8653e34d02}"   # tested revision; set HF_REV=main for the latest
HF=https://huggingface.co/istupakov/gigaam-v3-onnx/resolve/$HF_REV
mkdir -p models
for f in v3_e2e_rnnt_encoder.int8.onnx v3_e2e_rnnt_decoder.int8.onnx v3_e2e_rnnt_joint.int8.onnx v3_e2e_rnnt_vocab.txt; do
  if [ ! -f "models/$f" ]; then echo "== $f"; curl -fL --retry 3 -o "models/$f.part" "$HF/$f" && mv "models/$f.part" "models/$f"; fi
done
[ -f models/gigaam_v3.onnx ] || { echo "models/gigaam_v3.onnx (feature extractor) is missing - it ships with this project." >&2; exit 1; }
echo "done. Build with: make"
