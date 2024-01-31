// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "upsample.h"
#include "upsample_impl.h"
#include "core/providers/cuda/tensor/resize_impl.h"
#include "core/providers/cpu/tensor/utils.h"

using namespace onnxruntime::common;

namespace onnxruntime {
namespace cuda {

#define REGISTER_VERSIONED_TYPED_KERNEL(T, start, end)            \
  ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_EX(                        \
      Upsample,                                                   \
      kOnnxDomain,                                                \
      start,                                                      \
      end,                                                        \
      T,                                                          \
      kCudaExecutionProvider,                                     \
      (*KernelDefBuilder::Create())                               \
          .InputMemoryType(OrtMemTypeCPUInput, 1)                 \
          .TypeConstraint("T", DataTypeImpl::GetTensorType<T>()), \
      Upsample<T>)

REGISTER_VERSIONED_TYPED_KERNEL(float, 7, 8);
REGISTER_VERSIONED_TYPED_KERNEL(double, 7, 8);
REGISTER_VERSIONED_TYPED_KERNEL(MLFloat16, 7, 8);
REGISTER_VERSIONED_TYPED_KERNEL(int32_t, 7, 8);
REGISTER_VERSIONED_TYPED_KERNEL(uint8_t, 7, 8);

// Upsample was deprecated in opset 10
REGISTER_VERSIONED_TYPED_KERNEL(float, 9, 9);
REGISTER_VERSIONED_TYPED_KERNEL(double, 9, 9);
REGISTER_VERSIONED_TYPED_KERNEL(MLFloat16, 9, 9);
REGISTER_VERSIONED_TYPED_KERNEL(int32_t, 9, 9);
REGISTER_VERSIONED_TYPED_KERNEL(uint8_t, 9, 9);

/// <summary>
/// Compute scaled support value for a given dimension inverse scale
/// </summary>
/// <param name="support_value"></param>
/// <param name="inv_scale"></param>
/// <returns></returns>
inline float ComputeScaledSupportValue(float support_value, float inv_scale) {
  const float scale = 1.0f / inv_scale;
  float support = (scale >= 1.0f) ? (support_value * 0.5f) * scale : support_value * 0.5f;
  return support;
}

/// <summary>
/// Computes scale buffer size in number of elements for allocation purposes.
/// </summary>
/// <typeparam name="T"></typeparam>
/// <param name="output_size"></param>
/// <param name="window_size"></param>
/// <returns>Number of elements to fit in the buffer</returns>
inline SafeInt<int64_t> ComputeScaleBufferSize(int64_t output_size, int32_t window_size) {
  SafeInt<int64_t> buffer_size(output_size);
  return buffer_size * window_size;
}

/// <summary>
/// Compute a buffer for bilinear data for CUDA antialias resizing.
/// </summary>
/// <param name="output_height"></param>
/// <param name="output_width"></param>
/// <param name="inv_height_scale"></param>
/// <param name="inv_width_scale"></param>
/// <param name="support_value"></param>
/// <returns>Cumulative buffer size for y and x scales in number of elements</returns>
static int64_t ComputeBilinearScaleBufferSize(int64_t output_height, int64_t output_width,
                                              float inv_height_scale, float inv_width_scale, float support_value) {
  const float scaled_support_height = ComputeScaledSupportValue(support_value, inv_height_scale);
  const float scaled_support_width = ComputeScaledSupportValue(support_value, inv_width_scale);
  const int32_t window_size_height = ComputeWindowSize(scaled_support_height);
  const int32_t window_size_width = ComputeWindowSize(scaled_support_width);

  auto height_buffer_size = ComputeScaleBufferSize(output_height, window_size_height);
  auto width_buffer_size = ComputeScaleBufferSize(output_width, window_size_width);
  return height_buffer_size + width_buffer_size;
}

static int64_t ComputeTrilinearScaleBufferSize(int64_t output_height, int64_t output_width, int64_t output_depth,
                                               float inv_height_scale, float inv_width_scale, float inv_depth_scale,
                                               float support_value) {
  const float scaled_support_depth = ComputeScaledSupportValue(support_value, inv_depth_scale);
  const int32_t window_size_depth = ComputeWindowSize(scaled_support_depth);
  auto depth_buffer_size = ComputeScaleBufferSize(output_depth, window_size_depth);

  depth_buffer_size += ComputeBilinearScaleBufferSize(output_height, output_width, inv_height_scale, inv_width_scale, support_value);
  return depth_buffer_size;
}

template <typename T>
Status Upsample<T>::BaseCompute(OpKernelContext* context,
                                gsl::span<const float> roi,
                                gsl::span<const float> scales,
                                gsl::span<const int64_t> output_dims) const {
  const Tensor* X = context->Input<Tensor>(0);
  auto X_dims = X->Shape().GetDims();
  int32_t rank = static_cast<int32_t>(X_dims.size());

  ORT_ENFORCE(static_cast<int32_t>(output_dims.size()) == rank, "Rank of input and output tensor should be same.");
  if (rank == 0)
    return Status(ONNXRUNTIME, INVALID_ARGUMENT,
                  is_resize_ ? "Resize: input tensor cannot be scalar." : "Upsample: input tensor cannot be scalar.");
  if (rank != static_cast<int32_t>(scales.size()))
    return Status(ONNXRUNTIME, INVALID_ARGUMENT,
                  is_resize_ ? "Resize: input tensor's dimension does not match the scales." : "Upsample: input tensor's dimension does not match the scales.");
  if (roi.size() != 2 * X_dims.size())
    return Status(ONNXRUNTIME, INVALID_ARGUMENT,
                  "Resize: size of roi array should be 2 * N where N is the rank of input tensor X.");

  Tensor* Y = context->Output(0, output_dims);

  // Return early if the output tensor is going to be of size 0
  if (Y->Shape().Size() == 0) {
    return Status::OK();
  }

  typedef typename ToCudaType<T>::MappedType CudaT;

  // kernel
  TensorPitches input_pitches(X_dims);
  TArray<int64_t> input_strides(input_pitches);

  TensorPitches output_pitches(output_dims);
  TArray<fast_divmod> output_div_pitches(rank);

  for (int32_t i = 0; i < rank; ++i) {
    output_div_pitches[i] = fast_divmod(gsl::narrow_cast<int>(output_pitches[i]));
  }
  size_t output_count = Y->Shape().Size();

  if (is_resize_) {
    TArray<int64_t> input_shape(X_dims);
    TArray<int64_t> output_shape(output_dims);
    TArray<float, 10> roi_vals(roi);
    TArray<float> scales_vals(scales);

    size_t temp_buffer_size = CalcResizeBufferSize(mode_, output_dims);
    auto dims_mapping_buffer = GetScratchBuffer<unsigned char>(temp_buffer_size, context->GetComputeStream());
    void* dims_mapping = reinterpret_cast<void*>(dims_mapping_buffer.get());
    ResizeImpl(Stream(context), mode_, (int)rank, input_shape, output_shape,
               input_strides, output_div_pitches, scales_vals, roi_vals,
               reinterpret_cast<const CudaT*>(X->Data<T>()),
               reinterpret_cast<CudaT*>(Y->MutableData<T>()),
               output_count, use_extrapolation_, ToCudaType<T>::FromFloat(extrapolation_value_),
               cubic_coeff_a_, exclude_outside_,
               coordinate_transform_mode_, nearest_mode_,
               dims_mapping);
  } else {
    TArray<fast_divmod> scales_div(rank);

    for (int32_t i = 0; i < rank; ++i) {
      scales_div[i] = fast_divmod(gsl::narrow_cast<int>(ceil(scales[i])));
    }

    UpampleImpl(Stream(context),
                mode_,
                rank,
                (UpsampleMode::LINEAR == mode_) ? (rank == 2 ? X_dims[0] : X_dims[2]) : 0,
                input_strides,
                output_div_pitches,
                scales_div,
                reinterpret_cast<const CudaT*>(X->Data<T>()),
                reinterpret_cast<CudaT*>(Y->MutableData<T>()),
                output_count);
  }

  return Status::OK();
}

template <typename T>
Status Upsample<T>::ComputeInternal(OpKernelContext* context) const {
  const Tensor* X = context->Input<Tensor>(0);
  ORT_ENFORCE(X != nullptr);
  auto input_dims = X->Shape().GetDims();

  TensorShapeVector output_dims(input_dims.size());
  InlinedVector<float> roi_array(input_dims.size() * 2, 0.0f);
  if (!roi_cached_) {
    bool use_default_roi = true;
    if (need_roi_input_) {
      ORT_ENFORCE(roi_input_idx_ > 0, "Invalid roi input index.");
      const auto* roi = context->Input<Tensor>(roi_input_idx_);
      if (roi != nullptr) {
        ParseRoiData(roi, roi_array);
        use_default_roi = false;
      }
    }
    if (use_default_roi) {
      // default roi includes ensures all the values in that axis are included in the roi
      // normalized roi is thus : [start, end] = [0, 1]
      size_t input_rank = input_dims.size();
      roi_array.resize(input_rank * 2);
      for (size_t i = 0; i < input_rank; ++i) {
        roi_array[i] = 0;
        roi_array[i + input_rank] = 1;
      }
    }
  }

  ComputeROIWithAxes(roi_array, input_dims.size());

  InlinedVector<float> scales_array;
  // opset < 10
  if (OpKernel::Node().InputDefs().size() == 1) {
    // Compute output shape from scales attributes and input dims
    scales_array = scales_;

    ComputeOutputShape(scales_array, input_dims, output_dims);
    return BaseCompute(context, roi_array, scales_, output_dims);
  }

  const Tensor* scales = context->Input<Tensor>(scales_input_idx_);
  const Tensor* sizes = context->Input<Tensor>(sizes_input_idx_);

  // This is when scales are obtained and cached from a constant initializer
  if (scales_cached_) {
    ORT_RETURN_IF_NOT(sizes == nullptr, "Only one of scales or sizes must be provided as input.");
    scales_array = scales_;
    // Compute output shape from scales and input dims
    ComputeOutputShape(scales_array, input_dims, output_dims);
    return BaseCompute(context, roi_array, scales_array, output_dims);
  }

  // Scales an sizes are input to the node
  if (scales != nullptr && scales->Shape().Size() != 0) {
    // use scales input data
    ORT_ENFORCE(sizes == nullptr, "Only one of scales or sizes must be provided as input.");
    ORT_RETURN_IF_ERROR(ParseScalesData(scales, scales_array, input_dims.size()));

    // Compute output shape from scales and input dims
    ComputeOutputShape(scales_array, input_dims, output_dims);
  } else {
    // When sizes input is available directly populate it into the output_dims array.
    ORT_ENFORCE(sizes != nullptr && sizes->Shape().Size() != 0,
                "Either scales or sizes MUST be provided as input.");
    ORT_RETURN_IF_ERROR(ParseSizesData(sizes, output_dims, input_dims));
    ORT_RETURN_IF_ERROR(ParseScalesDataAndAdjustOutputSize(output_dims, input_dims, scales_array));
  }

  return BaseCompute(context, roi_array, scales_array, output_dims);
}

}  // namespace cuda
}  // namespace onnxruntime
