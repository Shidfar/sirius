#pragma once
#include "onnxruntime_c_api.h"

/* The OrtApiBase of the ONNX Runtime library (built in, or from the system); exits with a message if it can't be loaded. */
const OrtApiBase *sirius_ort_base(void);
