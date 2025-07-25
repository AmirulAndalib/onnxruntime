// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once
#include <memory>
#include <vector>
#include <gsl/gsl>

#include "core/providers/qnn/ort_api.h"
#include "QnnTypes.h"

namespace onnxruntime {
namespace qnn {

class QnnModelWrapper;  // Forward-declare

class QnnQuantParamsWrapper {
 public:
  QnnQuantParamsWrapper() : params_(QNN_QUANTIZE_PARAMS_INIT) {}

  QnnQuantParamsWrapper(const QnnQuantParamsWrapper& other);
  QnnQuantParamsWrapper& operator=(const QnnQuantParamsWrapper& other);

  QnnQuantParamsWrapper(QnnQuantParamsWrapper&& other) = default;
  QnnQuantParamsWrapper& operator=(QnnQuantParamsWrapper&& other) = default;

  // Construct a per-tensor quantization param (SCALE_OFFSET)
  QnnQuantParamsWrapper(float scale, int32_t offset);

  // Construct a per-channel quantization param.
  QnnQuantParamsWrapper(gsl::span<const float> scales, gsl::span<const int32_t> offsets, int32_t axis, bool is_int4);

  // Construct a LPBQ quantization param.
  QnnQuantParamsWrapper(gsl::span<const float> per_channel_float_scales, gsl::span<const uint8_t> per_block_int_scales,
                        gsl::span<const int32_t> offsets, int64_t axis, int64_t block_size, bool is_int4);

  Qnn_QuantizeParams_t& Get() { return params_; }
  const Qnn_QuantizeParams_t& Get() const { return params_; }

  // Initialize this object from a raw Qnn_QuantizeParam_t object.
  Status Init(const Qnn_QuantizeParams_t& params, const size_t lpbq_num_scaleoffsets = 0);

  // Initialize this object from a (potentially) quantized ONNX tensor.
  // QnnModelWrapper provides utilities for unpacking scale and zero-point ONNX initializers.
  Status Init(const QnnModelWrapper& qnn_model_wrapper, const NodeUnitIODef& io_def);

  QnnQuantParamsWrapper Copy() const;

  bool IsQuantized() const {
    return params_.encodingDefinition == QNN_DEFINITION_DEFINED;
  }

  bool IsPerTensor(bool include_bw = false) const {
    return params_.encodingDefinition == QNN_DEFINITION_DEFINED &&
           (params_.quantizationEncoding == QNN_QUANTIZATION_ENCODING_SCALE_OFFSET ||
            (include_bw && params_.quantizationEncoding == QNN_QUANTIZATION_ENCODING_BW_SCALE_OFFSET));
  }

  bool IsPerChannel() const {
    return params_.encodingDefinition == QNN_DEFINITION_DEFINED &&
           (params_.quantizationEncoding == QNN_QUANTIZATION_ENCODING_AXIS_SCALE_OFFSET ||
            (params_.quantizationEncoding == QNN_QUANTIZATION_ENCODING_BW_AXIS_SCALE_OFFSET));
  }

  bool IsLPBQ() const {
    return params_.encodingDefinition == QNN_DEFINITION_DEFINED &&
           (params_.quantizationEncoding == QNN_QUANTIZATION_ENCODING_BLOCKWISE_EXPANSION);
  }

  // Get a copy of scales. Works for both per-tensor and per-channel.
  Status GetScales(/*out*/ std::vector<float>& scales) const;

  // Handle transposing of a per-channel quantized tensor. The quantization parameter's axis
  // must be transposed using the inverse permutation of the Transpose.
  template <typename IntType>
  Status HandleTranspose(gsl::span<const IntType> perm) {
    if (!IsPerChannel()) {
      return Status::OK();
    }

    if (params_.quantizationEncoding == QNN_QUANTIZATION_ENCODING_AXIS_SCALE_OFFSET) {
      ORT_RETURN_IF_NOT(static_cast<size_t>(params_.axisScaleOffsetEncoding.axis) < perm.size(),
                        "Axis value is out of range of the provided permutation");
      const int32_t new_axis = static_cast<int32_t>(perm[params_.axisScaleOffsetEncoding.axis]);
      params_.axisScaleOffsetEncoding.axis = new_axis;
    } else if (params_.quantizationEncoding == QNN_QUANTIZATION_ENCODING_BW_AXIS_SCALE_OFFSET) {
      ORT_RETURN_IF_NOT(static_cast<size_t>(params_.bwAxisScaleOffsetEncoding.axis) < perm.size(),
                        "Axis value is out of range of the provided permutation");
      const int32_t new_axis = static_cast<int32_t>(perm[params_.bwAxisScaleOffsetEncoding.axis]);
      params_.bwAxisScaleOffsetEncoding.axis = new_axis;
    }

    return Status::OK();
  }

  // Handle "unsqueeze" of a per-channel quantized tensor. The quantization parameter's axis
  // may need to be shifted if the unsqueeze inserted 1s before the quantization axis.
  template <typename IntType>
  Status HandleUnsqueeze(gsl::span<const IntType> orig_shape,
                         gsl::span<const IntType> new_shape) {
    if (!IsPerChannel()) {
      return Status::OK();
    }

    ORT_RETURN_IF_NOT(orig_shape.size() < new_shape.size(), "Expected unsqueezed shape to have a greater rank.");

    // Get the axis value.
    int32_t axis = 0;
    if (params_.quantizationEncoding == QNN_QUANTIZATION_ENCODING_AXIS_SCALE_OFFSET) {
      axis = params_.axisScaleOffsetEncoding.axis;
    } else if (params_.quantizationEncoding == QNN_QUANTIZATION_ENCODING_BW_AXIS_SCALE_OFFSET) {
      axis = params_.bwAxisScaleOffsetEncoding.axis;
    } else {
      return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL,
                             "Unhandled quantization encoding: ", params_.quantizationEncoding);
    }

    // Find where the axis was moved to after unsqueeze.
    size_t num_found = 0;
    size_t j = 0;
    for (size_t i = 0; i < orig_shape.size() && j < new_shape.size(); i++) {
      while (orig_shape[i] != new_shape[j] && j < new_shape.size()) {
        assert(new_shape[j] == 1);
        j++;
      }
      assert(orig_shape[i] == new_shape[j]);
      if (num_found == static_cast<size_t>(axis)) {
        break;
      }
      num_found += 1;
      j++;
    }

    if (j == static_cast<size_t>(axis)) {
      return Status::OK();
    }

    // Set new axis.
    if (params_.quantizationEncoding == QNN_QUANTIZATION_ENCODING_AXIS_SCALE_OFFSET) {
      params_.axisScaleOffsetEncoding.axis = static_cast<int32_t>(j);
    } else if (params_.quantizationEncoding == QNN_QUANTIZATION_ENCODING_BW_AXIS_SCALE_OFFSET) {
      params_.bwAxisScaleOffsetEncoding.axis = static_cast<int32_t>(j);
    } else {
      return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL,
                             "Unhandled quantization encoding: ", params_.quantizationEncoding);
    }

    return Status::OK();
  }

 private:
  Qnn_QuantizeParams_t params_;

  // Stores arrays of per-channel scales and offsets. Fields in params_ point to this data.
  //
  // Use an opaque array of bytes because QNN uses different data layouts depending on the quantization encoding:
  // - QNN_QUANTIZATION_ENCODING_AXIS_SCALE_OFFSET: array of scale/zp pairs [{scale0, zp0}, {scale1, zp1}, ...]
  // - QNN_QUANTIZATION_ENCODING_BW_AXIS_SCALE_OFFSET: parallel arrays for scales and zps [scale0, ...] [zp0, zp1, ...]
  std::unique_ptr<char[]> per_channel_data_;

  // Stores LowPowerBlockQuant encodings meta like number of per_channel_scales, per-block scales,
  // and blockwise_expansion_data
  uint32_t per_channel_scales_size_;
  std::unique_ptr<uint8_t[]> block_scales_data_;
  std::unique_ptr<char[]> blockwise_expansion_data_;
};

}  // namespace qnn
}  // namespace onnxruntime
