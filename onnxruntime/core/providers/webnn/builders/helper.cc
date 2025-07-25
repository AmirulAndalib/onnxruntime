// Copyright (c) Microsoft Corporation. All rights reserved.
// Copyright (c) Intel Corporation. All rights reserved.
// Licensed under the MIT License.

#include <vector>

#include "helper.h"
#include <core/graph/graph_viewer.h>

#include "op_builder_factory.h"

namespace onnxruntime {
namespace webnn {

WebnnDeviceType DeviceTypeFromString(const std::string_view& device_type) {
  if (device_type == "gpu") {
    return WebnnDeviceType::GPU;
  }
  if (device_type == "cpu") {
    return WebnnDeviceType::CPU;
  }
  if (device_type == "npu") {
    return WebnnDeviceType::NPU;
  }
  ORT_THROW("Unknown WebNN deviceType.");
}

InitializedTensorSet CollectAllInitializedTensors(const GraphViewer& graph_viewer) {
  InitializedTensorSet all_initializers;
  if (graph_viewer.IsSubgraph()) {
    const Graph* cur_graph = &graph_viewer.GetGraph();
    // Traverse up to the top-level graph, collecting all initializers.
    while (cur_graph->IsSubgraph()) {
      const auto& current_initializers = cur_graph->GetAllInitializedTensors();
      all_initializers.insert(current_initializers.begin(), current_initializers.end());
      cur_graph = cur_graph->ParentGraph();
    }
    // Collect initializers in top-level graph.
    const auto& current_initializers = cur_graph->GetAllInitializedTensors();
    all_initializers.insert(current_initializers.begin(), current_initializers.end());
  }

  return all_initializers;
}

bool GetShape(const NodeArg& node_arg, std::vector<int64_t>& shape, const logging::Logger& logger) {
  const auto* shape_proto = node_arg.Shape();
  if (!shape_proto) {
    LOGS(logger, WARNING) << "NodeArg [" << node_arg.Name() << "] has no shape info";
    return false;
  }

  // We already checked the shape has no dynamic dimension.
  for (const auto& dim : shape_proto->dim()) {
    shape.push_back(dim.dim_value());
  }

  return true;
}

bool IsNodeSupported(const GraphViewer& graph_viewer, const Node& node, const WebnnDeviceType device_type,
                     const emscripten::val& wnn_limits, const logging::Logger& logger) {
  const auto& op_builders = GetOpBuilders();
  if (Contains(op_builders, node.OpType())) {
    const auto* op_builder = op_builders.at(node.OpType());
    return op_builder->IsOpSupported(graph_viewer, node, device_type, wnn_limits, logger);
  } else {
    return false;
  }
}

bool IsTensorShapeSupported(const NodeArg& node_arg, const std::string& parent_name,
                            const logging::Logger& logger, bool allow_empty_input) {
  const auto& node_arg_name = node_arg.Name();
  const auto* shape_proto = node_arg.Shape();
  // Optional tensors can be indicated by an empty name, just ignore it.
  if (node_arg_name.empty()) {
    return true;
  }
  // We do not support input/output with no shape.
  if (!shape_proto) {
    LOGS(logger, VERBOSE) << "Node arg [" << node_arg_name << "] of [" << parent_name << "] has not shape";
    return false;
  }

  for (const auto& dim : shape_proto->dim()) {
    // WebNN doesn't support dynamic shape - use sessionOptions.freeDimensionOverrides to fix the shape.
    if (!dim.has_dim_value()) {
      LOGS(logger, VERBOSE) << "Dynamic shape is not supported, "
                            << "use sessionOptions.FreeDimensionOverrides to set a fixed shape: " << node_arg_name;
      return false;
    }
    if (dim.dim_value() == 0 && !allow_empty_input) {
      LOGS(logger, VERBOSE) << "The shape of [" << node_arg_name << "] has 0 dimension which is not supported by WebNN";
      return false;
    }
  }

  return true;
}

// Check if a single input's rank of an ONNX op is supported by corresponding WebNN op.
bool IsInputRankSupported(const emscripten::val& wnn_limits,
                          const std::string_view webnn_op_type,
                          const std::string_view input_name,
                          const size_t input_rank,
                          const std::string_view node_name,
                          const logging::Logger& logger) {
  const std::string webnn_op_type_str(webnn_op_type);
  const std::string input_name_str(input_name);

  if (wnn_limits[webnn_op_type_str].isUndefined()) {
    LOGS(logger, VERBOSE) << "WebNN op type: [" << webnn_op_type
                          << "] is not defined in WebNN MLOpSupportLimits.";
    return false;
  }

  const emscripten::val input_limits = wnn_limits[webnn_op_type_str][input_name_str];

  if (input_limits.isUndefined()) {
    LOGS(logger, VERBOSE) << "Node name: [" << node_name
                          << "], WebNN op type: [" << webnn_op_type
                          << "], input [" << input_name
                          << "]: limits are not defined in WebNN MLOpSupportLimits.";
    return false;
  }

  const emscripten::val rank_range = input_limits["rankRange"];
  if (rank_range.isUndefined()) {
    LOGS(logger, VERBOSE) << "WebNN op type [" << webnn_op_type
                          << "] input [" << input_name
                          << "]: missing 'rankRange' attribute.";
    return false;
  }

  const emscripten::val min_val = rank_range["min"];
  const emscripten::val max_val = rank_range["max"];
  if (min_val.isUndefined() || max_val.isUndefined()) {
    LOGS(logger, VERBOSE) << "WebNN op type [" << webnn_op_type
                          << "] input [" << input_name
                          << "]: its 'rankRange' limits is missing valid 'min' or 'max' attributes.";
    return false;
  }

  size_t min_rank = min_val.as<size_t>();
  size_t max_rank = max_val.as<size_t>();
  if (input_rank < min_rank || input_rank > max_rank) {
    LOGS(logger, VERBOSE) << "Node name: [" << node_name
                          << "] WebNN op type [" << webnn_op_type
                          << "] input [" << input_name << "] rank " << input_rank
                          << " is not in supported range [" << min_rank << ", " << max_rank << "]";
    return false;
  }

  return true;
}

bool IsInputRankSupportedByOp(const Node& node, const emscripten::val& wnn_limits, const logging::Logger& logger) {
  const std::string_view onnx_op_type = node.OpType();
  const std::string_view webnn_op_type = GetWebNNOpType(onnx_op_type);

  if (webnn_op_type.empty()) {
    LOGS(logger, VERBOSE) << "ONNX op type: [" << onnx_op_type << "]'s corresponding WebNN op is not found.";
    return false;
  }

  std::vector<InputInfo> inputs;
  if (!GetWebNNOpInputs(onnx_op_type, inputs, logger)) {
    return false;
  }

  const auto& input_defs = node.InputDefs();

  for (const auto& input : inputs) {
    // If it is an optional input and is absent, skip.
    if (!TensorExists(input_defs, input.index)) {
      continue;
    }

    std::vector<int64_t> shape;
    if (!GetShape(*input_defs[input.index], shape, logger) ||
        !IsInputRankSupported(wnn_limits, webnn_op_type, input.name,
                              shape.size(),
                              node.Name(), logger)) {
      return false;
    }
  }

  return true;
}

std::unordered_set<const Node*> GetSupportedNodes(const GraphViewer& graph_viewer,
                                                  const emscripten::val& wnn_builder,
                                                  const WebnnDeviceType device_type,
                                                  const emscripten::val& wnn_limits,
                                                  const logging::Logger& logger) {
  std::unordered_set<const Node*> supported_nodes;

  for (const auto& node : graph_viewer.Nodes()) {
    const bool supported = IsNodeSupported(graph_viewer, node, device_type, wnn_limits, logger);
    LOGS(logger, VERBOSE) << "Operator type: [" << node.OpType()
                          << "] index: [" << node.Index()
                          << "] name: [" << node.Name()
                          << "] supported: [" << supported
                          << "]";
    if (supported) {
      supported_nodes.insert(&node);
    }
  }

  return supported_nodes;
}

bool AreDataTypesSame(const std::string_view op_type,
                      gsl::span<const int32_t> data_types,
                      const logging::Logger& logger) {
  for (size_t i = 1; i < data_types.size(); i++) {
    if (data_types[0] != data_types[i]) {
      LOGS(logger, VERBOSE) << "[" << op_type
                            << "] data types should be the same, but ["
                            << data_types[0] << "] does not match "
                            << data_types[i] << "].";
      return false;
    }
  }
  return true;
}

bool IsSupportedDataType(const int32_t onnx_data_type, const emscripten::val& webnn_supported_data_types) {
  auto it = onnx_to_webnn_data_type_map.find(static_cast<ONNX_NAMESPACE::TensorProto_DataType>(onnx_data_type));
  if (it == onnx_to_webnn_data_type_map.end())
    return false;

  const std::string_view webnn_data_type = it->second;

  // Check if WebNN supports the data type.
  bool is_supported = webnn_supported_data_types.call<emscripten::val>("includes",
                                                                       emscripten::val(std::string(webnn_data_type)))
                          .as<bool>();

  if (webnn_data_type == "int64" &&
      !is_supported &&
      webnn_supported_data_types.call<emscripten::val>("includes", emscripten::val("int32")).as<bool>()) {
    // Current context doesn't support int64, but int32 is supported.
    // We can use int32 as a workaround.
    is_supported = true;
  }

  return is_supported;
}

// Check if the input or output data type of ONNX node is supported by the WebNN operator.
bool IsDataTypeSupportedByOp(const std::string_view onnx_op_type,
                             const int32_t onnx_data_type,
                             const emscripten::val& wnn_limits,
                             const std::string_view webnn_input_output_name,
                             const std::string_view onnx_input_output_name,
                             const logging::Logger& logger) {
  const std::string_view webnn_op_type = GetWebNNOpType(onnx_op_type);

  return !webnn_op_type.empty() &&
         IsDataTypeSupportedByWebNNOp(onnx_op_type, webnn_op_type, onnx_data_type, wnn_limits,
                                      webnn_input_output_name, onnx_input_output_name, logger);
}

bool IsDataTypeSupportedByWebNNOp(const std::string_view onnx_op_type,
                                  const std::string_view webnn_op_type,
                                  const int32_t onnx_data_type,
                                  const emscripten::val& wnn_limits,
                                  const std::string_view webnn_input_output_name,
                                  const std::string_view onnx_input_output_name,
                                  const logging::Logger& logger) {
  if (wnn_limits[std::string(webnn_op_type)].isUndefined()) {
    LOGS(logger, VERBOSE) << "[" << onnx_op_type << "] WebNN op [" << webnn_op_type << "] is not supported for now";
    return false;
  }

  if (wnn_limits[std::string(webnn_op_type)][std::string(webnn_input_output_name)].isUndefined()) {
    LOGS(logger, VERBOSE) << "[" << onnx_op_type << "] WebNN op [" << webnn_op_type << "] doesn't have parameter ["
                          << webnn_input_output_name << "]";
    return false;
  }
  if (!IsSupportedDataType(
          onnx_data_type, wnn_limits[std::string(webnn_op_type)][std::string(webnn_input_output_name)]["dataTypes"])) {
    LOGS(logger, VERBOSE) << "[" << onnx_op_type << "] " << onnx_input_output_name << "'s data type: ["
                          << onnx_data_type << "] is not supported by WebNN op [" << webnn_op_type << "] for now";
    return false;
  }
  return true;
}

bool GetBidirectionalBroadcastShape(std::vector<int64_t>& shape_a,
                                    std::vector<int64_t>& shape_b,
                                    std::vector<int64_t>& output_shape) {
  size_t size_a = shape_a.size();
  size_t size_b = shape_b.size();
  size_t smaller_size = std::min(size_a, size_b);
  size_t larger_size = std::max(size_a, size_b);

  output_shape.resize(larger_size);

  for (size_t i = 0; i < larger_size; i++) {
    // right alignment
    size_t axis_a = size_a - i - 1;
    size_t axis_b = size_b - i - 1;

    if (i < smaller_size) {
      // Broadcastable tensors must either have each dimension the same size or equal to one.
      if (shape_a[axis_a] != shape_b[axis_b] && shape_a[axis_a] != 1 && shape_b[axis_b] != 1) {
        return false;
      }
      output_shape[larger_size - i - 1] = std::max(shape_a[axis_a], shape_b[axis_b]);
    } else {
      // For the remaining dimensions in the larger tensor, copy the dimension size directly to the output shape.
      output_shape[larger_size - i - 1] = (size_a > size_b) ? shape_a[axis_a] : shape_b[axis_b];
    }
  }

  return true;
}

bool SetWebnnDataType(emscripten::val& desc, const int32_t data_type) {
  switch (data_type) {
    case ONNX_NAMESPACE::TensorProto_DataType_INT4:
      desc.set("dataType", emscripten::val("int4"));
      return true;
    case ONNX_NAMESPACE::TensorProto_DataType_UINT4:
      desc.set("dataType", emscripten::val("uint4"));
      return true;
    case ONNX_NAMESPACE::TensorProto_DataType_BOOL:
    case ONNX_NAMESPACE::TensorProto_DataType_UINT8:
      desc.set("dataType", emscripten::val("uint8"));
      return true;
    case ONNX_NAMESPACE::TensorProto_DataType_INT8:
      desc.set("dataType", emscripten::val("int8"));
      return true;
    case ONNX_NAMESPACE::TensorProto_DataType_FLOAT16:
      desc.set("dataType", emscripten::val("float16"));
      return true;
    case ONNX_NAMESPACE::TensorProto_DataType_FLOAT:
      desc.set("dataType", emscripten::val("float32"));
      return true;
    case ONNX_NAMESPACE::TensorProto_DataType_INT32:
      desc.set("dataType", emscripten::val("int32"));
      return true;
    case ONNX_NAMESPACE::TensorProto_DataType_INT64:
      desc.set("dataType", emscripten::val("int64"));
      return true;
    case ONNX_NAMESPACE::TensorProto_DataType_UINT32:
      desc.set("dataType", emscripten::val("uint32"));
      return true;
    case ONNX_NAMESPACE::TensorProto_DataType_UINT64:
      desc.set("dataType", emscripten::val("uint64"));
      return true;
    default:
      return false;
  }
}

bool IsMLTensorSupported() {
  static bool is_supported = !emscripten::val::global("MLTensor").isUndefined();
  return is_supported;
}

// Convert int8 to uint4/int4 (stored as uint8), used for creating WebNN Constant
// with same value in both high and low nibbles for uint4/int4 data type.
uint8_t PackInt8ToUint8DoubledNibbles(int8_t value, const int32_t& data_type) {
  if (data_type == ONNX_NAMESPACE::TensorProto_DataType_UINT4) {
    if (value < 0 || value > 15) {
      ORT_THROW("Value cannot be safely converted to uint4.");
    }
  } else {
    if (value < -8 || value > 7) {
      ORT_THROW("Value cannot be safely converted to int4.");
    }
  }

  // Explicit conversion + truncate to lower 4 bits
  const uint8_t result = static_cast<uint8_t>(value) & 0x0F;
  // Duplicate the 4-bit value to both high and low nibbles
  return (result << 4) | result;
}

// Convert float32 to float16 (stored as uint16)
uint16_t PackFloat32ToUint16AsFloat16(float value) {
  uint32_t float32_bits;

  // Safely copy the float bits into an integer
  std::memcpy(&float32_bits, &value, sizeof(float));

  // Extract the sign, exponent, and mantissa from the float32 bits
  uint32_t sign = (float32_bits >> 31) & 0x1;
  uint32_t exponent = (float32_bits >> 23) & 0xFF;
  uint32_t mantissa = float32_bits & 0x7FFFFF;

  // Shift the sign for float16
  uint16_t sign_float16 = sign << 15;

  // Handle special cases: Infinity and NaN
  if (exponent == 255) {
    return sign_float16 | (0x1F << 10) | (mantissa ? 0x200 : 0);
  }
  // Handle zero and subnormal numbers in float32
  if (exponent == 0) {
    return sign_float16 | (mantissa >> 13);
  }

  // Adjust the exponent for float16 (subtract bias difference: 127 - 15 = 112)
  int exponent_float16 = exponent - 112;

  // Handle exponent overflow (larger than float16 can represent)
  if (exponent_float16 >= 0x1F) {
    return sign_float16 | (0x1F << 10);
  }
  // Handle exponent underflow (smaller than float16 can represent)
  if (exponent_float16 <= 0) {
    mantissa = (mantissa | 0x800000) >> (1 - exponent_float16);
    return sign_float16 | (mantissa >> 13);
  }

  // Adjust the mantissa by shifting it to fit float16 format (round to nearest even)
  uint16_t mantissa_float16 = (mantissa + 0x1000) >> 13;

  // Combine sign, exponent, and mantissa into the final float16 representation
  return sign_float16 | (exponent_float16 << 10) | mantissa_float16;
}

}  // namespace webnn
}  // namespace onnxruntime
