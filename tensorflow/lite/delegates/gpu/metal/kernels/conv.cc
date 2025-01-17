/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/lite/delegates/gpu/metal/kernels/conv.h"

#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/substitute.h"
#include "tensorflow/lite/delegates/gpu/common/data_type.h"
#include "tensorflow/lite/delegates/gpu/common/gpu_info.h"
#include "tensorflow/lite/delegates/gpu/common/model.h"
#include "tensorflow/lite/delegates/gpu/common/operations.h"
#include "tensorflow/lite/delegates/gpu/common/shape.h"
#include "tensorflow/lite/delegates/gpu/common/types.h"
#include "tensorflow/lite/delegates/gpu/common/util.h"
#include "tensorflow/lite/delegates/gpu/common/winograd_util.h"
#include "tensorflow/lite/delegates/gpu/metal/compute_task_descriptor.h"

namespace tflite {
namespace gpu {
namespace metal {

enum class WeightsUploadType {
  PRIVATE_MEM_SIMD8_BROADCAST,
  PRIVATE_MEM_SIMD16_BROADCAST,
  PRIVATE_MEM_SIMD32_BROADCAST,
  LOCAL_MEM_BY_THREADS,
  GLOBAL_MEM,
  CONSTANT_MEM,
};

enum class WeightsInnerBlockLayout {
  O4I4,
  I4O4,
};

struct ConvParams {
  int3 block_size;
  int3 work_group_size;
  int3 work_group_launch_order;
  int src_depth_loop_size;
  bool need_src_loop = true;
  bool need_dst_loop = true;
  bool linear_wh;
  bool linear_whs;
  WeightsUploadType weights_upload_type;
  WeightsInnerBlockLayout weight_layout;
  bool different_weights_for_height = false;
  bool x_kernel_is_1;
  bool y_kernel_is_1;
};

namespace {

int GetNumOutputSlices(int dst_channels) {
  const int dst_depth = DivideRoundUp(dst_channels, 4);
  if (dst_depth % 4 == 0 || dst_depth >= 16) {
    return 4;
  } else if (dst_depth % 2 == 0 || dst_depth >= 4) {
    return 2;
  } else {
    return 1;
  }
}

struct GlobalIdsParams {
  std::vector<std::string> global_ids;
  std::vector<std::string> group_ids;
  std::vector<std::string> local_sizes;
  std::vector<std::string> local_ids;
  int3 block_size;
  int3 launch_order;
  bool linear_wh;
  bool linear_whs;
  std::string task_size_w;  // must be filled if linear_wh or linear_whs enabled
  std::string task_size_wh;  // must be filled if linear_whs enabled
};

std::string GlobalIdsGen(const GlobalIdsParams& params) {
  std::string c;
  int3 launch_remap;
  launch_remap[params.launch_order.x] = 0;
  launch_remap[params.launch_order.y] = 1;
  launch_remap[params.launch_order.z] = 2;
  if (params.linear_whs) {
    c += "  int linear_whs = " + params.global_ids[0] + ";\n";
    c += "  int Z = (linear_whs / " + params.task_size_wh + ") * " +
         std::to_string(params.block_size.z) + ";\n";
    c += "  int linear_wh = linear_whs % " + params.task_size_wh + ";\n";
    c += "  int Y = (linear_wh / " + params.task_size_w + ") * " +
         std::to_string(params.block_size.y) + ";\n";
    c += "  int X = (linear_wh % " + params.task_size_w + ") * " +
         std::to_string(params.block_size.x) + ";\n";
  } else if (params.linear_wh) {
    if (params.launch_order.x == 0) {
      c += "  int linear_wh = " + params.global_ids[0] + ";\n";
    } else {
      c += "  int linear_wh = " + params.group_ids[launch_remap.x] + " * " +
           params.local_sizes[0] + " + " + params.local_ids[0] + ";\n";
    }
    c += "  int Y = (linear_wh / " + params.task_size_w + ") * " +
         std::to_string(params.block_size.y) + ";\n";
    c += "  int X = (linear_wh % " + params.task_size_w + ") * " +
         std::to_string(params.block_size.x) + ";\n";
    if (params.launch_order.y == 1) {
      c += "  int Z = " + params.global_ids[1] + " * " +
           std::to_string(params.block_size.z) + ";\n";
    } else {
      c += "  int Z = (" + params.group_ids[launch_remap.y] + " * " +
           params.local_sizes[1] + " + " + params.local_ids[1] + ") * " +
           std::to_string(params.block_size.z) + ";\n";
    }
  } else {
    if (params.launch_order.x == 0) {
      c += "  int X = " + params.global_ids[0] + " * " +
           std::to_string(params.block_size.x) + ";\n";
    } else {
      c += "  int X = (" + params.group_ids[launch_remap.x] + " * " +
           params.local_sizes[0] + " + " + params.local_ids[0] + ") * " +
           std::to_string(params.block_size.x) + ";\n";
    }
    if (params.launch_order.y == 1) {
      c += "  int Y = " + params.global_ids[1] + " * " +
           std::to_string(params.block_size.y) + ";\n";
    } else {
      c += "  int Y = (" + params.group_ids[launch_remap.y] + " * " +
           params.local_sizes[1] + " + " + params.local_ids[1] + ") * " +
           std::to_string(params.block_size.y) + ";\n";
    }
    if (params.launch_order.z == 2) {
      c += "  int Z = " + params.global_ids[2] + " * " +
           std::to_string(params.block_size.z) + ";\n";
    } else {
      c += "  int Z = (" + params.group_ids[launch_remap.z] + " * " +
           params.local_sizes[2] + " + " + params.local_ids[2] + ") * " +
           std::to_string(params.block_size.z) + ";\n";
    }
  }
  return c;
}

std::string GenerateUploadByThreads(const std::string& local_ptr_name,
                                    const std::string& global_ptr_name,
                                    const std::string& global_offset_name,
                                    const std::string& lid_name,
                                    int total_work_items,
                                    int elements_to_upload) {
  std::string c;
  std::string offset =
      global_offset_name.empty() ? "" : global_offset_name + " + ";
  const int groups = elements_to_upload / total_work_items;
  const int reminder = elements_to_upload % total_work_items;
  for (int i = 0; i < groups; ++i) {
    c += "    " + local_ptr_name + "[" + lid_name + " + " +
         std::to_string(total_work_items * i) + "] = " + global_ptr_name + "[" +
         offset + lid_name + " + " + std::to_string(total_work_items * i) +
         "];\n";
  }
  if (reminder != 0) {
    c += "    if (" + lid_name + " < " + std::to_string(reminder) + ") {\n";
    c += "      " + local_ptr_name + "[" + lid_name + " + " +
         std::to_string(total_work_items * groups) + "] = " + global_ptr_name +
         "[" + offset + lid_name + " + " +
         std::to_string(total_work_items * groups) + "];\n";
    c += "    }\n";
  }
  return c;
}

std::string GenerateConvolution(const ConvParams& params) {
  GlobalIdsParams ids_params;
  ids_params.group_ids = {"group_id.x", "group_id.y", "group_id.z"};
  ids_params.global_ids = {"ugid.x", "ugid.y", "ugid.z"};
  ids_params.local_ids = {"tid3d.x", "tid3d.y", "tid3d.z"};
  ids_params.local_sizes = {"lsize.x", "lsize.y", "lsize.z"};
  ids_params.linear_wh = params.linear_wh;
  ids_params.task_size_w = "args.task_size_x";
  ids_params.task_size_wh = "args.task_size_y";
  ids_params.linear_whs = params.linear_whs;
  ids_params.block_size = params.block_size;
  ids_params.launch_order = params.work_group_launch_order;

  std::string addr_space =
      params.weights_upload_type == WeightsUploadType::CONSTANT_MEM ? "constant"
                                                                    : "device";
  const bool use_local_mem =
      params.weights_upload_type == WeightsUploadType::LOCAL_MEM_BY_THREADS;
  const int local_mem_size =
      params.block_size.z * 4 * params.src_depth_loop_size;

  const bool use_simd_broadcast =
      params.weights_upload_type ==
          WeightsUploadType::PRIVATE_MEM_SIMD8_BROADCAST ||
      params.weights_upload_type ==
          WeightsUploadType::PRIVATE_MEM_SIMD16_BROADCAST ||
      params.weights_upload_type ==
          WeightsUploadType::PRIVATE_MEM_SIMD32_BROADCAST;
  int simd_size = 1;
  if (params.weights_upload_type ==
      WeightsUploadType::PRIVATE_MEM_SIMD8_BROADCAST) {
    simd_size = 8;
  } else if (params.weights_upload_type ==
             WeightsUploadType::PRIVATE_MEM_SIMD16_BROADCAST) {
    simd_size = 16;
  } else if (params.weights_upload_type ==
             WeightsUploadType::PRIVATE_MEM_SIMD32_BROADCAST) {
    simd_size = 32;
  }

  const bool use_filters_constants =
      !params.need_dst_loop && !params.need_src_loop && params.x_kernel_is_1 &&
      params.y_kernel_is_1;

  std::string channels[4] = {"x", "y", "z", "w"};
  std::string c;
  c.reserve(16 * 1024);  // Reserve large enough buffer.
  c += R"(
#include <metal_stdlib>
using namespace metal;

struct uniforms {
    int4 task_sizes;
};
$0

kernel void ComputeFunction(
    $1
    uint tid[[thread_index_in_threadgroup]],
    uint3 group_id[[threadgroup_position_in_grid]],
    uint3 tid3d[[thread_position_in_threadgroup]],
    uint3 lsize[[threads_per_threadgroup]],
)";
  if (use_simd_broadcast) {
    c += "    uint simd_id[[thread_index_in_simdgroup]],\n";
  }
  c += "    uint3 ugid[[thread_position_in_grid]]){\n";
  c += GlobalIdsGen(ids_params);
  c += "  if (Z >= args.dst_tensor.Slices()) return;\n";
  bool late_xy_check = use_local_mem || use_simd_broadcast;
  if (!late_xy_check && !params.linear_whs) {
    c += "  if (X >= args.dst_tensor.Width() || Y >= args.dst_tensor.Height()) "
         "return;\n";
  }
  for (int z = 0; z < params.block_size.z; ++z) {
    for (int y = 0; y < params.block_size.y; ++y) {
      for (int x = 0; x < params.block_size.x; ++x) {
        const std::string s_i =
            std::to_string(z) + std::to_string(y) + std::to_string(x);
        c +=
            "  ACCUM_FLT4 r" + s_i + " = ACCUM_FLT4(0.0f, 0.0f, 0.0f, 0.0f);\n";
      }
    }
  }
  auto for_every_yx =
      [&](std::function<std::string(const std::string&, const std::string&,
                                    const std::string&, int, int)>
              lambda) {
        for (int y = 0; y < params.block_size.y; ++y) {
          const std::string s_y = std::to_string(y);
          for (int x = 0; x < params.block_size.x; ++x) {
            const std::string s_x = std::to_string(x);
            const std::string s_yx = s_y + s_x;
            c += lambda(s_yx, s_x, s_y, x, y) + "\n";
          }
        }
      };
  if (!use_filters_constants) {
    std::string kern_x = params.x_kernel_is_1 ? "" : " * args.kernel_size_x";
    std::string kern_y = params.y_kernel_is_1 ? "" : " * args.kernel_size_y";
    std::string dst_offset =
        params.need_dst_loop ? " + Z * 4 * args.src_tensor.Slices()" : "";
    if (!params.need_dst_loop) {
      c += "  " + addr_space + " FLT4* tmp = args.weights.GetPtr();\n";
    } else {
      if (params.different_weights_for_height) {
        c += "  " + addr_space +
             " FLT4* tmp = args.weights.GetPtr() + (Z * "
             "args.src_tensor.Height() + Y * " +
             std::to_string(params.block_size.z) +
             ") * 4 * args.src_tensor.Slices();\n";
      } else {
        c += "  " + addr_space +
             " FLT4* tmp = args.weights.GetPtr() + Z * 4 * "
             "args.src_tensor.Slices()" +
             kern_x + kern_y + ";\n";
      }
    }
  }
  if (!params.x_kernel_is_1) {
    for (int x = 0; x < params.block_size.x; ++x) {
      const std::string s_x = std::to_string(x);
      c += "  int x" + s_x + " = (X + " + s_x +
           ") * args.stride_x + args.padding_x;\n";
    }
  }
  if (!params.y_kernel_is_1) {
    for (int y = 0; y < params.block_size.y; ++y) {
      const std::string s_y = std::to_string(y);
      c += "  int y" + s_y + " = (Y + " + s_y +
           ") * args.stride_y + args.padding_y;\n";
    }
  }
  if (use_local_mem) {
    c += "  threadgroup FLT4 weights_cache[" + std::to_string(local_mem_size) +
         "];\n";
  }
  if (!params.y_kernel_is_1) {
    c += "  int y = 0;\n";
    c += "  do {\n";
    for (int y = 0; y < params.block_size.y; ++y) {
      const std::string s_y = std::to_string(y);
      c += "  int c_y" + s_y + " = y * args.dilation_y + y" + s_y + ";\n";
      c += "  bool y" + s_y + "_out = c_y" + s_y + " < 0 || c_y" + s_y +
           " >= args.src_tensor.Height();\n";
      c += "  c_y" + s_y + " = clamp(c_y" + s_y +
           ", 0, args.src_tensor.Height() - 1);\n";
    }
  } else {
    for (int y = 0; y < params.block_size.y; ++y) {
      const std::string s_y = std::to_string(y);
      c += "  int c_y" + s_y + " = clamp(Y + " + s_y +
           ", 0, args.src_tensor.Height() - 1);\n";
    }
  }
  if (!params.x_kernel_is_1) {
    c += "  int x = 0;\n";
    c += "  do {\n";
    for (int x = 0; x < params.block_size.x; ++x) {
      const std::string s_x = std::to_string(x);
      c += "  int c_x" + s_x + " = x * args.dilation_x + x" + s_x + ";\n";
      c += "  bool x" + s_x + "_out = c_x" + s_x + " < 0 || c_x" + s_x +
           " >= args.src_tensor.Width();\n";
      c += "  c_x" + s_x + " = clamp(c_x" + s_x +
           ", 0, args.src_tensor.Width() - 1);\n";
    }
  } else {
    for (int x = 0; x < params.block_size.x; ++x) {
      const std::string s_x = std::to_string(x);
      c += "  int c_x" + s_x + " = clamp(X + " + s_x +
           ", 0, args.src_tensor.Width() - 1);\n";
    }
  }
  for (int y = 0; y < params.block_size.y; ++y) {
    const std::string s_y = std::to_string(y);
    for (int x = 0; x < params.block_size.x; ++x) {
      const std::string s_x = std::to_string(x);
      const std::string s_yx = s_y + s_x;
      if (!params.y_kernel_is_1 && !params.x_kernel_is_1) {
        c += "  FLT m" + s_yx + " = !(y" + s_y + "_out || x" + s_x + "_out);\n";
      } else if (!params.y_kernel_is_1) {
        c += "  FLT m" + s_yx + " = !y" + s_y + "_out;\n";
      } else if (!params.x_kernel_is_1) {
        c += "  FLT m" + s_yx + " = !x" + s_x + "_out;\n";
      }
    }
  }
  for (int y = 0; y < params.block_size.y; ++y) {
    const std::string s_y = std::to_string(y);
    for (int x = 0; x < params.block_size.x; ++x) {
      const std::string s_x = std::to_string(x);
      const std::string s_yx = s_y + s_x;
      c += "  device FLT4* src_loc_" + s_yx +
           " = args.src_tensor.GetHandle() + args.src_tensor.GetWHOffset(c_x" +
           s_x + ", c_y" + s_y + ");\n";
    }
  }
  c += "  int s = 0;\n";
  if (params.need_src_loop) {
    c += "  do {\n";
  }
  if (use_local_mem) {
    const int total_work_items = params.work_group_size.x *
                                 params.work_group_size.y *
                                 params.work_group_size.z;
    c += "    SIMDGROUP_BARRIER(mem_flags::mem_none);\n";
    c += GenerateUploadByThreads("weights_cache", "tmp",
                                 /*global_offset_name*/ "", "tid",
                                 total_work_items, local_mem_size);
    c += "    SIMDGROUP_BARRIER(mem_flags::mem_threadgroup);\n";
  } else if (use_simd_broadcast) {
    int parts = local_mem_size / simd_size;
    int reminder = local_mem_size % simd_size;
    for (int i = 0; i < parts; ++i) {
      c += "    FLT4 simd_w" + std::to_string(i) + " = tmp[simd_id + " +
           std::to_string(i * simd_size) + "];\n";
    }
    if (reminder) {
      c += "    FLT4 simd_w" + std::to_string(parts) + ";\n";
      c += "    if (simd_id < " + std::to_string(reminder) + ") {\n";
      c += "      simd_w" + std::to_string(parts) + " = tmp[simd_id + " +
           std::to_string(parts * simd_size) + "];\n";
      c += "    }\n";
    }
  }
  auto declare_src = [&]() {
    for (int y = 0; y < params.block_size.y; ++y) {
      for (int x = 0; x < params.block_size.x; ++x) {
        const std::string s_yx = std::to_string(y) + std::to_string(x);
        c += "    FLT4 src" + s_yx + ";\n";
      }
    }
  };
  auto read_src = [&]() {
    for (int y = 0; y < params.block_size.y; ++y) {
      for (int x = 0; x < params.block_size.x; ++x) {
        const std::string s_yx = std::to_string(y) + std::to_string(x);
        if (!params.y_kernel_is_1 || !params.x_kernel_is_1) {
          c += "    src" + s_yx + " = *src_loc_" + s_yx + " * m" + s_yx + ";\n";
        } else {
          c += "    src" + s_yx + " = *src_loc_" + s_yx + ";\n";
        }
      }
    }
    for (int y = 0; y < params.block_size.y; ++y) {
      for (int x = 0; x < params.block_size.x; ++x) {
        const std::string s_yx = std::to_string(y) + std::to_string(x);
        c += "    src_loc_" + s_yx + " += args.src_tensor.SliceStride();\n";
      }
    }
  };
  auto conv_core = [&](int offset) {
    std::string name = use_local_mem ? "weights_cache" : "tmp";
    if (use_filters_constants) {
      name = "args.weights.GetPtr()";
    }
    for (int z = 0; z < params.block_size.z; ++z) {
      for (int ch = 0; ch < 4; ++ch) {
        for (int y = 0; y < params.block_size.y; ++y) {
          for (int x = 0; x < params.block_size.x; ++x) {
            std::string s_id = std::to_string(y) + std::to_string(x);
            std::string r_id =
                std::to_string(z) + std::to_string(y) + std::to_string(x);
            std::string f_val =
                name + "[" + std::to_string(z * 4 + ch + offset) + "]";
            if (use_simd_broadcast) {
              int simd_id = (z * 4 + ch + offset) / simd_size;
              int thread_id = (z * 4 + ch + offset) % simd_size;
              f_val = "simd_broadcast(simd_w" + std::to_string(simd_id) + ", " +
                      std::to_string(thread_id) + "u)";
            }
            std::string s_val = "src" + s_id;
            std::string r_val = "r" + r_id;
            if (params.weight_layout == WeightsInnerBlockLayout::O4I4) {
              c += "    " + r_val + "." + channels[ch] + " += dot(" + f_val +
                   ", " + s_val + ");\n";
            } else {  // WeightsInnerBlockLayout::I404
              c += "    " + r_val + " += " + f_val + " * " + s_val + "." +
                   channels[ch] + ";\n";
            }
          }
        }
      }
    }
  };
  declare_src();
  read_src();
  c += "    s += 1;\n";
  conv_core(0);
  for (int i = 1; i < params.src_depth_loop_size; ++i) {
    read_src();
    conv_core(i * params.block_size.z * 4);
    c += "    s += 1;\n";
  }
  if (!use_filters_constants) {
    c += "    tmp += " +
         std::to_string(params.block_size.z * 4 * params.src_depth_loop_size) +
         ";\n";
  }
  if (params.need_src_loop) {
    c += "  } while (s < args.src_tensor.Slices());\n";
  }
  if (!params.x_kernel_is_1) {
    c += "  x++;\n";
    c += "  } while (x < args.kernel_size_x);\n";
  }
  if (!params.y_kernel_is_1) {
    c += "  y++;\n";
    c += "  } while (y < args.kernel_size_y);\n";
  }

  if (late_xy_check && !params.linear_whs) {
    c += "  if (X >= args.dst_tensor.Width() || Y >= args.dst_tensor.Height()) "
         "return;\n";
  }

  for_every_yx([](const std::string& s_yx, const std::string& s_x,
                  const std::string& s_y, int x, int y) {
    return "  args.dst_tensor.GetAddress(offset_" + s_yx + ", X + " + s_x +
           ", Y + " + s_y + ", Z);";
  });

  std::string bias_name = "args.biases.GetPtr()";
  if (params.need_dst_loop) {
    c += "  device FLT4* bias_loc = args.biases.GetPtr() + Z;\n";
    bias_name = "bias_loc";
  }
  for (int y = 0; y < params.block_size.y; ++y) {
    for (int x = 0; x < params.block_size.x; ++x) {
      for (int z = 0; z < params.block_size.z; ++z) {
        std::string r_id =
            std::to_string(z) + std::to_string(y) + std::to_string(x);
        c += "  r" + r_id + " += TO_ACCUM4_TYPE(" + bias_name + "[" +
             std::to_string(z) + "]);\n";
      }
    }
  }
  for (int z = 0; z < params.block_size.z; ++z) {
    const std::string s_z = std::to_string(z);
    c += "  if (Z + " + s_z + " < args.dst_tensor.Slices()) {\n";
    for (int y = 0; y < params.block_size.y; ++y) {
      const std::string s_y = std::to_string(y);
      for (int x = 0; x < params.block_size.x; ++x) {
        const std::string s_x = std::to_string(x);
        const std::string s_yx = s_y + s_x;
        const std::string s_zyx = s_z + s_yx;
        bool need_check_x = x >= 1;
        bool need_check_y = y >= 1;
        std::string check;
        if (need_check_x) {
          check += "(X + " + s_x + ") < args.dst_tensor.Width()";
        }
        if (need_check_y) {
          check += check.empty() ? "" : " && ";
          check += "(Y + " + s_y + ") < args.dst_tensor.Height()";
        }
        if (!check.empty()) {
          c += "    if (" + check + ") {\n";
        } else {
          c += "    {\n";
        }
        c += "      FLT4 value = FLT4(r" + s_zyx + ");\n";
        c += "      int linear_index = offset_" + s_yx +
             " + args.dst_tensor.SliceStride() * " + s_z + ";\n";
        c += "      args.dst_tensor.Linking(value, X + " + s_x + ", Y + " +
             s_y + ", Z + " + s_z + ");\n";
        c += "      args.dst_tensor.WriteLinear(value, linear_index);\n";
        c += "    }\n";
      }
    }
    c += "  }\n";
  }
  c += "}\n";
  return c;
}

std::vector<float> ReorderWeightsForConv(
    const tflite::gpu::Tensor<OHWI, DataType::FLOAT32>& weights,
    const ConvParams& params) {
  const int dst_depth = DivideRoundUp(weights.shape.o, 4);
  const int src_depth = DivideRoundUp(weights.shape.i, 4);
  std::vector<float> weights_reordered(
      weights.shape.w * weights.shape.h *
      AlignByN(dst_depth, params.block_size.z) * 4 * src_depth * 4);

  bool isO4I4 = params.weight_layout == WeightsInnerBlockLayout::O4I4;

  int counter = 0;
  for (int d = 0; d < DivideRoundUp(dst_depth, params.block_size.z); ++d) {
    for (int y = 0; y < weights.shape.h; ++y) {
      for (int x = 0; x < weights.shape.w; ++x) {
        for (int s = 0; s < src_depth; ++s) {
          for (int k = 0; k < params.block_size.z; ++k) {
            for (int j = 0; j < 4; ++j) {
              for (int i = 0; i < 4; ++i) {
                int src_ch;
                int dst_ch;
                if (isO4I4) {
                  src_ch = s * 4 + i;
                  dst_ch = (d * params.block_size.z + k) * 4 + j;
                } else {
                  src_ch = s * 4 + j;
                  dst_ch = (d * params.block_size.z + k) * 4 + i;
                }
                if (src_ch >= weights.shape.i || dst_ch >= weights.shape.o) {
                  weights_reordered[counter++] = 0.0f;
                } else {
                  const size_t f_index =
                      weights.shape.LinearIndex({dst_ch, y, x, src_ch});
                  weights_reordered[counter++] = weights.data[f_index];
                }
              }
            }
          }
        }
      }
    }
  }
  return weights_reordered;
}

int GetGroupsCount(const BHWC& dst_shape, const int3& wg_size,
                   const int3& block_size) {
  const int dst_slices = DivideRoundUp(dst_shape.c, 4);

  int grid_x = DivideRoundUp(dst_shape.w, block_size.x);
  int grid_y = DivideRoundUp(dst_shape.h, block_size.y);
  int grid_z = DivideRoundUp(dst_slices, block_size.z);

  return DivideRoundUp(grid_x, wg_size.x) * DivideRoundUp(grid_y, wg_size.y) *
         DivideRoundUp(grid_z, wg_size.z);
}

int GetGroupsCountForLinearWH(const BHWC& dst_shape, const int3& wg_size,
                              const int3& block_size) {
  const int dst_slices = DivideRoundUp(dst_shape.c, 4);

  int grid_x = DivideRoundUp(dst_shape.w, block_size.x);
  int grid_y = DivideRoundUp(dst_shape.h, block_size.y);
  int grid_z = DivideRoundUp(dst_slices, block_size.z);

  return DivideRoundUp(grid_x * grid_y, wg_size.x) *
         DivideRoundUp(grid_z, wg_size.y);
}

int GetGroupsCountForLinearWHS(const BHWC& dst_shape, const int3& wg_size,
                               const int3& block_size) {
  const int dst_slices = DivideRoundUp(dst_shape.c, 4);

  int grid_x = DivideRoundUp(dst_shape.w, block_size.x);
  int grid_y = DivideRoundUp(dst_shape.h, block_size.y);
  int grid_z = DivideRoundUp(dst_slices, block_size.z);

  return DivideRoundUp(grid_x * grid_y * grid_z, wg_size.x);
}

bool IsKernelXIs1(const Convolution2DAttributes& attr) {
  return attr.weights.shape.w == 1 && attr.strides.w == 1 &&
         attr.dilations.w == 1 && attr.padding.prepended.w == 0 &&
         attr.padding.appended.w == 0;
}

bool IsKernelYIs1(const Convolution2DAttributes& attr) {
  return attr.weights.shape.h == 1 && attr.strides.h == 1 &&
         attr.dilations.h == 1 && attr.padding.prepended.h == 0 &&
         attr.padding.appended.h == 0;
}

int GetMaximumPossibleWavesCount(const AppleInfo& apple_info,
                                 const BHWC& dst_shape) {
  if (apple_info.IsLocalMemoryPreferredOverGlobal()) {
    return GetGroupsCountForLinearWH(dst_shape, {32, 1, 1}, {1, 1, 1});
  } else {
    return GetGroupsCountForLinearWHS(dst_shape, {32, 1, 1}, {1, 1, 1});
  }
}

int GetRecommendedBlockSize(const AppleInfo& apple_info,
                            const BHWC& dst_shape) {
  const int max_waves = GetMaximumPossibleWavesCount(apple_info, dst_shape);
  const int cu_count = apple_info.GetComputeUnitsCount();
  if (max_waves >= cu_count * 64) {
    return 8;
  } else if (max_waves >= cu_count * 32) {
    return 4;
  } else if (max_waves >= cu_count * 16) {
    return 2;
  } else {
    return 1;
  }
}

ConvParams GetConvParamsForA7A8(const AppleInfo& apple_info,
                                const Convolution2DAttributes& attr,
                                const BHWC& dst_shape) {
  const int dst_slices = DivideRoundUp(dst_shape.c, 4);
  const int src_slices = DivideRoundUp(attr.weights.shape.i, 4);

  ConvParams params;
  params.weights_upload_type = WeightsUploadType::LOCAL_MEM_BY_THREADS;
  params.x_kernel_is_1 = IsKernelXIs1(attr);
  params.y_kernel_is_1 = IsKernelYIs1(attr);
  params.src_depth_loop_size = 1;
  params.block_size = int3(1, 1, 1);
  params.linear_wh = false;
  params.linear_whs = false;
  params.work_group_launch_order = int3(0, 1, 2);
  params.weight_layout = WeightsInnerBlockLayout::O4I4;

  int blk_total_size = GetRecommendedBlockSize(apple_info, dst_shape);

  if (blk_total_size >= 4 && (dst_slices % 4 == 0 || dst_slices >= 16)) {
    params.block_size.z = 4;
    blk_total_size /= 4;
  } else if (blk_total_size >= 2 && (dst_slices % 2 == 0 || dst_slices >= 4)) {
    params.block_size.z = 2;
    blk_total_size /= 2;
  }
  if (blk_total_size >= 4) {
    params.block_size.x = 2;
    params.block_size.y = 2;
    blk_total_size /= 4;
  } else if (blk_total_size >= 2) {
    if (dst_shape.w % 2 != 0 && dst_shape.h % 2 == 0) {
      params.block_size.y = 2;
    } else {
      params.block_size.x = 2;
    }
    blk_total_size /= 2;
  }

  params.work_group_size = params.block_size.x <= params.block_size.y
                               ? int3(8, 4, 1)
                               : int3(4, 8, 1);

  int g1 = GetGroupsCount(dst_shape, params.work_group_size, params.block_size);
  int g2 = GetGroupsCountForLinearWH(dst_shape, {32, 1, 1}, params.block_size);
  int g3 = GetGroupsCountForLinearWHS(dst_shape, {32, 1, 1}, params.block_size);

  if (g2 < g1) {
    params.linear_wh = true;
    params.work_group_size = int3(32, 1, 1);
    params.work_group_launch_order = int3(0, 1, 2);
  }
  float precise_threshold = 3.1f;
  float precise_ratio = static_cast<float>(g2) / static_cast<float>(g3);
  if (precise_ratio > precise_threshold) {
    params.linear_wh = false;
    params.linear_whs = true;
    params.work_group_size = int3(32, 1, 1);
    params.weights_upload_type = WeightsUploadType::GLOBAL_MEM;
  }

  if (params.src_depth_loop_size == src_slices) {
    params.need_src_loop = false;
  }
  if (params.block_size.z == dst_slices) {
    params.need_dst_loop = false;
  }
  const bool use_filters_constants =
      !params.need_dst_loop && !params.need_src_loop && params.x_kernel_is_1 &&
      params.y_kernel_is_1;
  if (use_filters_constants) {
    params.weights_upload_type = WeightsUploadType::CONSTANT_MEM;
  }

  return params;
}

ConvParams GetConvParamsForA9AndHigher(const AppleInfo& apple_info,
                                       const Convolution2DAttributes& attr,
                                       const BHWC& dst_shape) {
  const int dst_slices = DivideRoundUp(dst_shape.c, 4);
  const int src_slices = DivideRoundUp(attr.weights.shape.i, 4);
  int blk_total_size = GetRecommendedBlockSize(apple_info, dst_shape);
  int3 block_size = int3(1, 1, 1);
  if (blk_total_size >= 2 && apple_info.IsBionic()) {
    if (dst_shape.h % 2 != 0 && dst_shape.w % 2 == 0) {
      block_size.x = 2;
    } else {
      block_size.y = 2;
    }
    blk_total_size /= 2;
  }
  if (blk_total_size >= 4 && (dst_slices % 4 == 0 || dst_slices >= 16)) {
    block_size.z = 4;
    blk_total_size /= 4;
  } else if (blk_total_size >= 2 && (dst_slices % 2 == 0 || dst_slices >= 4)) {
    block_size.z = 2;
    blk_total_size /= 2;
  }
  if (blk_total_size >= 4 && dst_slices == 3) {
    block_size.z = 3;
    blk_total_size /= 4;
  }

  ConvParams params;
  params.weights_upload_type = WeightsUploadType::GLOBAL_MEM;
  params.x_kernel_is_1 = IsKernelXIs1(attr);
  params.y_kernel_is_1 = IsKernelYIs1(attr);
  params.src_depth_loop_size = 1;
  params.block_size = block_size;
  params.linear_wh = false;
  params.linear_whs = false;
  params.work_group_size = int3(8, 4, 1);
  params.work_group_launch_order = int3(2, 0, 1);
  params.weight_layout = WeightsInnerBlockLayout::O4I4;
  int g1 = GetGroupsCount(dst_shape, {8, 4, 1}, block_size);
  int g2 = GetGroupsCountForLinearWH(dst_shape, {32, 1, 1}, block_size);
  int g3 = GetGroupsCountForLinearWHS(dst_shape, {32, 1, 1}, block_size);
  if (g2 < g1) {
    params.linear_wh = true;
    params.work_group_size = int3(32, 1, 1);
    params.work_group_launch_order = int3(0, 1, 2);
  }
  float precise_threshold = apple_info.IsBionic() ? 1.0f : 1.04f;
  float precise_ratio = static_cast<float>(g2) / static_cast<float>(g3);
  if (precise_ratio > precise_threshold) {
    params.linear_wh = false;
    params.linear_whs = true;
    params.work_group_size = int3(32, 1, 1);
  }
  int total_elements =
      params.block_size.x * params.block_size.y * params.block_size.z;
  if (total_elements == 1) {
    if (src_slices % 4 == 0) {
      params.src_depth_loop_size = 4;
    } else if (src_slices % 2 == 0) {
      params.src_depth_loop_size = 2;
    }
  } else if (total_elements == 2) {
    if (src_slices % 2 == 0) {
      params.src_depth_loop_size = 2;
    }
  }
  if (params.src_depth_loop_size == src_slices) {
    params.need_src_loop = false;
  }
  if (params.block_size.z == dst_slices) {
    params.need_dst_loop = false;
  }
  const bool use_filters_constants =
      !params.need_dst_loop && !params.need_src_loop && params.x_kernel_is_1 &&
      params.y_kernel_is_1;
  if (use_filters_constants) {
    params.weights_upload_type = WeightsUploadType::CONSTANT_MEM;
  }

  return params;
}

ConvParams GetConvParamsForIntel(const Convolution2DAttributes& attr,
                                 CalculationsPrecision precision,
                                 const BHWC& dst_shape) {
  const int dst_slices = DivideRoundUp(dst_shape.c, 4);
  const int src_slices = DivideRoundUp(attr.weights.shape.i, 4);
  ConvParams params;
  params.weights_upload_type = WeightsUploadType::PRIVATE_MEM_SIMD8_BROADCAST;
  params.x_kernel_is_1 = IsKernelXIs1(attr);
  params.y_kernel_is_1 = IsKernelYIs1(attr);
  params.src_depth_loop_size = 1;
  params.linear_wh = false;
  params.linear_whs = false;
  params.work_group_launch_order = int3(2, 0, 1);
  params.block_size = int3(1, 1, 1);
  if (dst_slices % 4 == 0 || dst_slices >= 8) {
    params.block_size.z = 4;
  } else if (dst_slices % 2 == 0 || dst_slices >= 4) {
    params.block_size.z = 2;
  }
  params.work_group_size = int3(8, 2, 1);
  if (precision == CalculationsPrecision::F32_F16) {
    params.weight_layout = WeightsInnerBlockLayout::O4I4;
  } else {
    params.weight_layout = WeightsInnerBlockLayout::I4O4;
  }

  if (src_slices % 2 == 0) {
    params.src_depth_loop_size = 2;
  }

  int g1 = GetGroupsCount(dst_shape, params.work_group_size, params.block_size);
  int g2 = GetGroupsCountForLinearWH(dst_shape, {16, 1, 1}, params.block_size);

  if (g2 < g1) {
    params.linear_wh = true;
    params.work_group_size = int3(16, 1, 1);
    params.work_group_launch_order = int3(1, 0, 2);
  }

  return params;
}

ConvParams GetConvParamsForAMD(const Convolution2DAttributes& attr,
                               CalculationsPrecision precision,
                               const BHWC& dst_shape) {
  ConvParams params;
  params.block_size = int3(1, 1, 4);
  params.work_group_size = int3(8, 4, 1);
  params.work_group_launch_order = int3(2, 0, 1);
  params.src_depth_loop_size = 1;
  params.need_src_loop = true;
  params.need_dst_loop = true;
  params.linear_wh = false;
  params.linear_whs = false;
  params.weights_upload_type = WeightsUploadType::GLOBAL_MEM;
  params.different_weights_for_height = false;
  params.x_kernel_is_1 = IsKernelXIs1(attr);
  params.y_kernel_is_1 = IsKernelYIs1(attr);
  if (precision == CalculationsPrecision::F32_F16) {
    params.weight_layout = WeightsInnerBlockLayout::O4I4;
  } else {
    params.weight_layout = WeightsInnerBlockLayout::I4O4;
  }
  return params;
}

ConvParams GetConvParams(const GpuInfo& gpu_info,
                         const Convolution2DAttributes& attr,
                         CalculationsPrecision precision,
                         const BHWC& dst_shape) {
  if (gpu_info.IsApple()) {
    if (gpu_info.apple_info.IsLocalMemoryPreferredOverGlobal()) {
      return GetConvParamsForA7A8(gpu_info.apple_info, attr, dst_shape);
    } else {
      return GetConvParamsForA9AndHigher(gpu_info.apple_info, attr, dst_shape);
    }
  } else if (gpu_info.IsIntel()) {
    return GetConvParamsForIntel(attr, precision, dst_shape);
  } else if (gpu_info.IsAMD()) {
    return GetConvParamsForAMD(attr, precision, dst_shape);
  } else {
    ConvParams params;
    params.block_size = int3(1, 1, 4);
    params.work_group_size = int3(8, 4, 1);
    params.work_group_launch_order = int3(2, 0, 1);
    params.src_depth_loop_size = 1;
    params.need_src_loop = true;
    params.need_dst_loop = true;
    params.linear_wh = false;
    params.linear_whs = false;
    params.weights_upload_type = WeightsUploadType::GLOBAL_MEM;
    params.different_weights_for_height = false;
    params.x_kernel_is_1 = IsKernelXIs1(attr);
    params.y_kernel_is_1 = IsKernelYIs1(attr);
    params.weight_layout = WeightsInnerBlockLayout::O4I4;
    return params;
  }
}

std::pair<uint3, uint3> GetDispatchSizes(const ConvParams& params,
                                         const BHWC& shape) {
  const int dst_slices = DivideRoundUp(shape.c, 4);

  int grid_x = DivideRoundUp(shape.w, params.block_size.x);
  int grid_y = DivideRoundUp(shape.h, params.block_size.y);
  int grid_z = DivideRoundUp(dst_slices, params.block_size.z);

  const uint3 group_size(params.work_group_size.x, params.work_group_size.y,
                         params.work_group_size.z);
  int3 wg;
  uint3 groups_count;
  if (params.linear_whs) {
    wg.x = DivideRoundUp(grid_x * grid_y * grid_z, params.work_group_size.x);
    groups_count = uint3(wg.x, 1, 1);
  } else if (params.linear_wh) {
    wg.x = DivideRoundUp(grid_x * grid_y, params.work_group_size.x);
    wg.y = DivideRoundUp(grid_z, params.work_group_size.y);
    groups_count = uint3(wg[params.work_group_launch_order.x],
                         wg[params.work_group_launch_order.y], 1);
  } else {
    wg.x = DivideRoundUp(grid_x, params.work_group_size.x);
    wg.y = DivideRoundUp(grid_y, params.work_group_size.y);
    wg.z = DivideRoundUp(grid_z, params.work_group_size.z);
    groups_count = uint3(wg[params.work_group_launch_order.x],
                         wg[params.work_group_launch_order.y],
                         wg[params.work_group_launch_order.z]);
  }
  return std::make_pair(group_size, groups_count);
}

}  // namespace

ComputeTaskDescriptor ConvolutionGeneric(const OperationDef& definition,
                                         const BHWC& dst_shape,
                                         const Convolution2DAttributes& attr,
                                         const GpuInfo& gpu_info) {
  ConvParams params =
      GetConvParams(gpu_info, attr, definition.precision, dst_shape);

  ComputeTaskDescriptor desc(definition);
  desc.shader_source = GenerateConvolution(params);
  desc.AddSrcTensor("src_tensor", definition.src_tensors[0]);
  desc.AddDstTensor("dst_tensor", definition.dst_tensors[0]);

  desc.args.AddInt("kernel_size_x", attr.weights.shape.w);
  desc.args.AddInt("kernel_size_y", attr.weights.shape.h);
  desc.args.AddInt("dilation_x", attr.dilations.w);
  desc.args.AddInt("dilation_y", attr.dilations.h);
  desc.args.AddInt("stride_x", attr.strides.w);
  desc.args.AddInt("stride_y", attr.strides.h);
  desc.args.AddInt("padding_x", -attr.padding.prepended.w);
  desc.args.AddInt("padding_y", -attr.padding.prepended.h);

  auto weights_reordered = ReorderWeightsForConv(attr.weights, params);
  auto data_type = DeduceDataTypeFromPrecision(definition.precision);
  const int dst_depth = DivideRoundUp(attr.weights.shape.o, 4);

  MemoryType mem_type =
      params.weights_upload_type == WeightsUploadType::CONSTANT_MEM
          ? MemoryType::CONSTANT
          : MemoryType::GLOBAL;

  BufferDescriptor weights_desc;
  weights_desc.element_type = data_type;
  weights_desc.element_size = 4;
  weights_desc.memory_type = mem_type;
  weights_desc.data = GetByteBufferConverted(weights_reordered, data_type);
  weights_desc.size = weights_desc.data.size();
  desc.args.AddObject(
      "weights", absl::make_unique<BufferDescriptor>(std::move(weights_desc)));

  BufferDescriptor bias_desc;
  bias_desc.element_type = data_type;
  bias_desc.element_size = 4;
  bias_desc.memory_type = mem_type;
  bias_desc.data = GetByteBufferConvertedResized(
      attr.bias.data, data_type, AlignByN(dst_depth, params.block_size.z) * 4);
  bias_desc.size = bias_desc.data.size();
  desc.args.AddObject(
      "biases", absl::make_unique<BufferDescriptor>(std::move(bias_desc)));

  desc.args.AddInt("task_size_x");
  desc.args.AddInt("task_size_y");

  desc.update_function = {[params](const std::vector<BHWC>& src_shapes,
                                   const std::vector<BHWC>& dst_shapes,
                                   ArgumentsBinder* args) -> absl::Status {
    const int grid_x = DivideRoundUp(dst_shapes[0].w, params.block_size.x);
    const int grid_y = DivideRoundUp(dst_shapes[0].h, params.block_size.y);
    RETURN_IF_ERROR(args->SetInt("task_size_x", grid_x));
    RETURN_IF_ERROR(args->SetInt("task_size_y", grid_x * grid_y));
    return absl::OkStatus();
  }};

  desc.resize_function = [params](const std::vector<BHWC>& src_shapes,
                                  const std::vector<BHWC>& dst_shapes) {
    return GetDispatchSizes(params, dst_shapes[0]);
  };

  return desc;
}

ComputeTaskDescriptor ConvolutionWino4x4To6x6(
    const OperationDef& definition, const BHWC& dst_shape,
    const Convolution2DAttributes& attr, const GpuInfo& gpu_info) {
  const int dst_slices = DivideRoundUp(attr.weights.shape.o, 4);
  ConvParams params;
  params.work_group_launch_order = int3(2, 0, 1);
  params.src_depth_loop_size = 1;
  params.need_src_loop = true;
  params.need_dst_loop = true;
  params.linear_wh = false;
  params.linear_whs = false;
  params.different_weights_for_height = true;
  params.x_kernel_is_1 = true;
  params.y_kernel_is_1 = true;
  if (gpu_info.IsApple()) {
    params.weight_layout = WeightsInnerBlockLayout::O4I4;
    if (gpu_info.apple_info.IsLocalMemoryPreferredOverGlobal()) {
      params.weights_upload_type = WeightsUploadType::LOCAL_MEM_BY_THREADS;
      params.work_group_size = int3(32, 1, 1);
      params.block_size = int3(4, 1, 4);
    } else {
      params.weights_upload_type = WeightsUploadType::GLOBAL_MEM;
      params.work_group_size = int3(8, 4, 1);
      params.block_size = int3(4, 1, 4);
    }
  } else if (gpu_info.IsIntel()) {
    params.weight_layout = WeightsInnerBlockLayout::I4O4;
    params.weights_upload_type = WeightsUploadType::PRIVATE_MEM_SIMD8_BROADCAST;
    params.work_group_size = int3(16, 1, 1);
    params.block_size = int3(1, 1, 4);
  } else if (gpu_info.IsAMD()) {
    params.weight_layout = WeightsInnerBlockLayout::I4O4;
    params.weights_upload_type = WeightsUploadType::GLOBAL_MEM;
    params.work_group_size = int3(32, 1, 1);
    params.block_size = int3(2, 1, 4);
  } else {
    params.weight_layout = WeightsInnerBlockLayout::I4O4;
    params.weights_upload_type = WeightsUploadType::GLOBAL_MEM;
    params.work_group_size = int3(32, 1, 1);
    params.block_size = int3(2, 1, 4);
  }

  ComputeTaskDescriptor desc(definition);
  desc.shader_source = GenerateConvolution(params);
  desc.AddSrcTensor("src_tensor", definition.src_tensors[0]);
  desc.AddDstTensor("dst_tensor", definition.dst_tensors[0]);

  desc.args.AddInt("kernel_size_x", 1);
  desc.args.AddInt("kernel_size_y", 1);
  desc.args.AddInt("dilation_x", 1);
  desc.args.AddInt("dilation_y", 1);
  desc.args.AddInt("stride_x", 1);
  desc.args.AddInt("stride_y", 1);
  desc.args.AddInt("padding_x", 0);
  desc.args.AddInt("padding_y", 0);

  ::tflite::gpu::Tensor<OHWI, DataType::FLOAT32> wino_weights;
  RearrangeWeightsToWinograd4x4To6x6Weights(attr.weights, &wino_weights);
  auto weights_reordered = ReorderWeightsForConv(wino_weights, params);
  std::vector<float> dummy_biases(AlignByN(dst_slices, params.block_size.z) * 4,
                                  0.0f);

  auto data_type = DeduceDataTypeFromPrecision(definition.precision);

  BufferDescriptor weights_desc;
  weights_desc.element_type = data_type;
  weights_desc.element_size = 4;
  weights_desc.data = GetByteBufferConverted(weights_reordered, data_type);
  weights_desc.size = weights_desc.data.size();
  desc.args.AddObject(
      "weights", absl::make_unique<BufferDescriptor>(std::move(weights_desc)));

  BufferDescriptor bias_desc;
  bias_desc.element_type = data_type;
  bias_desc.element_size = 4;
  bias_desc.data = GetByteBufferConverted(dummy_biases, data_type);
  bias_desc.size = bias_desc.data.size();
  desc.args.AddObject(
      "biases", absl::make_unique<BufferDescriptor>(std::move(bias_desc)));

  desc.args.AddInt("task_size_x");
  desc.args.AddInt("task_size_y");

  desc.update_function = {[params](const std::vector<BHWC>& src_shapes,
                                   const std::vector<BHWC>& dst_shapes,
                                   ArgumentsBinder* args) -> absl::Status {
    const int grid_x = DivideRoundUp(dst_shapes[0].w, params.block_size.x);
    const int grid_y = DivideRoundUp(dst_shapes[0].h, params.block_size.y);
    RETURN_IF_ERROR(args->SetInt("task_size_x", grid_x));
    RETURN_IF_ERROR(args->SetInt("task_size_y", grid_x * grid_y));
    return absl::OkStatus();
  }};

  desc.resize_function = [params](const std::vector<BHWC>& src_shapes,
                                  const std::vector<BHWC>& dst_shapes) {
    return GetDispatchSizes(params, dst_shapes[0]);
  };

  return desc;
}

}  // namespace metal
}  // namespace gpu
}  // namespace tflite
