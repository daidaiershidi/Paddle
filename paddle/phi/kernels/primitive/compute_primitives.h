// Copyright (c) 2021 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#ifdef PADDLE_WITH_CUDA
#include <cuda_fp16.h>
#endif
#ifdef PADDLE_WITH_HIP
#include <hip/hip_fp16.h>
#endif

#include "paddle/fluid/platform/device/gpu/gpu_device_function.h"
#include "paddle/phi/common/float16.h"

namespace phi {
namespace kps {
namespace details {

#ifdef __HIPCC__
constexpr int kReduceMaxThread = 256;
constexpr int kWarpSize = 64;
#else
constexpr int kReduceMaxThread = 128;
constexpr int kWarpSize = 32;
#endif

// kGlobalMode: block reduce, each block gets an output;
// kLocalMode: thread reduce, each thread gets an output;
enum ReduceMode { kGlobalMode, kLocalMode };

template <typename T>
class MPTypeTrait {
 public:
  using Type = T;
};

template <>
class MPTypeTrait<phi::dtype::float16> {
 public:
  using Type = float;
};

/**
 * @brief Will be used in BlockYReduce, get the index of reduce_num in shared
 * memory.
 */
__device__ __forceinline__ int SharedMemoryIndex(int index) {
  return (threadIdx.y + index) * blockDim.x + threadIdx.x;
}

template <typename T, typename ReduceOp>
__device__ __forceinline__ T WarpReduce(T val, ReduceOp reducer) {
  unsigned mask = 0u;
  CREATE_SHFL_MASK(mask, true);
  for (int stride = details::kWarpSize / 2; stride > 0; stride >>= 1) {
    T temp = paddle::platform::CudaShuffleDownSync(mask, val, stride);
    val = reducer(val, temp);
  }
  return val;
}

/* e.g.
 * |---------block---------|
 * |warp0|warp1|warp2|warp3|
 * |0~31|32~63|64~95|96~127|  ---->blockDim.x = 128
 *  \|/  \|/   \|/    \|/     ---->1. First WarpReduce in each warp
 * res0  res1  res2  res3     ---->2. Store result of each warp to shared memory
 *   \    \    /     /        ---->3. Load the result above from shared memory
 *        res                         to warp0 and process the second WarpReduce
 */

/**
 * @brief BlockXReduce reduce along blockDim.x.
 */
template <typename T, typename ReduceOp>
__device__ __forceinline__ T BlockXReduce(T val, ReduceOp reducer) {
  __syncthreads();
  using details::kWarpSize;
  __shared__ T shared[2 * kWarpSize];
  int block_dim_x = blockDim.x;
  if (blockDim.x > kWarpSize) {
    block_dim_x = blockDim.x / kWarpSize;
    int lane = threadIdx.x % kWarpSize;
    int tid = threadIdx.y * blockDim.x + threadIdx.x;
    int wid = tid / kWarpSize;
    int bid = threadIdx.y;
    val = WarpReduce(val, reducer);
    if (lane == 0) {
      shared[wid] = val;
    }
    __syncthreads();
    val = shared[bid * block_dim_x + lane];
  }

  unsigned mask = 0u;
  CREATE_SHFL_MASK(mask, true);
  for (int stride = 1; stride < block_dim_x; stride <<= 1) {
    T temp = paddle::platform::CudaShuffleDownSync(mask, val, stride);
    val = reducer(val, temp);
  }
  if (threadIdx.x == 0) {
    shared[threadIdx.y] = val;
  }
  __syncthreads();
  return shared[threadIdx.y];
}

/**
 * @brief BlockYReduce reduce along blockDim.y.
 */
template <typename T, typename ReduceOp>
__device__ __forceinline__ T BlockYReduce(T val, ReduceOp reducer) {
  __shared__ T shared_memory[1024];
  shared_memory[SharedMemoryIndex(0)] = val;
  for (int stride = blockDim.y / 2; stride > 0; stride >>= 1) {
    __syncthreads();
    if (threadIdx.y < stride && threadIdx.y + stride < blockDim.y) {
      T temp = shared_memory[SharedMemoryIndex(stride)];
      val = reducer(val, temp);
    }
    shared_memory[SharedMemoryIndex(0)] = val;
  }
  __syncthreads();
  return shared_memory[threadIdx.x];
}

// Swap data
template <typename T>
__device__ __forceinline__ void Swap(T* first_value, T* second_value) {
  T t_value;
  t_value = (*first_value);
  (*first_value) = (*second_value);
  (*second_value) = t_value;
}

// swap with monotonic_type
template <typename T>
__device__ __forceinline__ void Comparator(T* first_value,
                                           T* second_value,
                                           int monotonic_type) {
  if (((*first_value) > (*second_value)) == monotonic_type) {
    Swap<T>(first_value, second_value);
  }
}

template <typename T, typename IndexType>
__device__ __forceinline__ void ComparatorWithIndex(T* first_value,

                                                    T* second_value,
                                                    IndexType* first_index,
                                                    IndexType* second_index,
                                                    int monotonic_type) {
  if ((*first_value > (*second_value)) == monotonic_type) {
    // swap value
    Swap<T>(first_value, second_value);
    // swap index
    Swap<IndexType>(first_index, second_index);
  }
}

}  // namespace details

/**
 * @brief Perform unary calculation according to OpFunc. Shape of input and
 * output are the same.
 *
 * @template paraments
 * InT: The data type of in.
 * OutT: The data type of out.
 * NX: The number of data columns loaded by each thread.
 * NY: The number of data rows loaded by each thread.
 * BlockSize: Identifies the current device thread index method. For GPU,
 * threadIdx.x is used as the thread index. Currently only GPU was supported.
 * OpFunc: Compute functor which has an operator() as following:
 *     template <typename InT, typename OutT>
 *     struct XxxFunctor {
 *       HOSTDEVICE OutT operator()(const InT& a) const {
 *         return ...;
 *       }
 *     };
 *
 * @param：
 * out: The register pointer of out, the size is NX * NY.
 * in: The register pointer of in, the size is NX * NY.
 * compute: Compute function which was declared like OpFunc<InT, OutT>().
 */
template <typename InT,
          typename OutT,
          int NX,
          int NY,
          int BlockSize,
          class OpFunc>
__device__ __forceinline__ void ElementwiseUnary(OutT* out,
                                                 const InT* in,
                                                 OpFunc compute) {
#pragma unroll
  for (int idx = 0; idx < NX * NY; idx++) {
    out[idx] = static_cast<OutT>(compute(in[idx]));
  }
}

/**
 * @brief Binary calculation according to OpFunc. Shape of The input and output
 * are the same.
 *
 * @template paraments
 * InT: The data type of in1 and in2.
 * OutT: The data type of out.
 * NX: The number of data columns computed by each thread.
 * NY: The number of data rows computed by each thread.
 * BlockSize: Identifies the current device thread index method. For GPU,
 * threadIdx.x is used as the thread index. Currently only GPU was supported.
 * OpFunc: Compute functor which has an operator() as following:
 *     template <typename InT>
 *     struct XxxFunctor {
 *       HOSTDEVICE InT operator()(const InT& a, const InT& b) const {
 *         return ...;
 *       }
 *     };
 *
 * @param：
 * out: The register pointer of out, the size is NX * NY.
 * in1: The register pointer of fist input, size is NX * NY.
 * in2: The register pointer of second input, size is NX * NY.
 * compute: Compute function which was declared like OpFunc<InT>().
 */
template <typename InT,
          typename OutT,
          int NX,
          int NY,
          int BlockSize,
          class OpFunc>
__device__ __forceinline__ void ElementwiseBinary(OutT* out,
                                                  const InT* in1,
                                                  const InT* in2,
                                                  OpFunc compute) {
#pragma unroll
  for (int idx = 0; idx < NX * NY; ++idx) {
    out[idx] = static_cast<OutT>(compute(in1[idx], in2[idx]));
  }
}

/**
 * @brief Ternary calculation according to OpFunc. Shape of input and output
 * are the same.
 *
 * @template paraments
 * InT: The data type of in1 and in2.
 * OutT: The data type of out.
 * NX: The number of data columns loaded by each thread.
 * NY: The number of data rows loaded by each thread.
 * BlockSize: Identifies the current device thread index method. For GPU,
 * threadIdx.x is used as the thread index. Currently only GPU was supported.
 * OpFunc: Compute functor which has an operator() as following
 *     template <typename InT>
 *     struct XxxFunctor {
 *       HOSTDEVICE InT operator()(const InT& a, const InT& b, const InT& c)
 * const {
 *         return ...;
 *       }
 *     };
 *
 * @param
 * out: The register pointer of out, the size is NX * NY.
 * in1: The register pointer of fist input, size is NX * NY.
 * in2: The register pointer of second input, size is NX * NY.
 * in3: The register pointer of third input, size is NX * NY.
 * compute: Compute function which was declared like OpFunc<InT>().
 */
template <typename InT,
          typename OutT,
          int NX,
          int NY,
          int BlockSize,
          class OpFunc>
__device__ __forceinline__ void ElementwiseTernary(
    OutT* out, const InT* in1, const InT* in2, const InT* in3, OpFunc compute) {
#pragma unroll
  for (int idx = 0; idx < NX * NY; ++idx) {
    out[idx] = static_cast<OutT>(compute(in1[idx], in2[idx], in3[idx]));
  }
}

/**
 * @brief Multivariate calculation according to OpFunc. Shape of inputs and
 * output are the same.
 *
 * @template paraments
 * InT: The data type of in1, in2 and in3.
 * OutT: The data type of out.
 * NX: The number of data columns loaded by each thread.
 * NY: The number of data rows loaded by each thread.
 * BlockSize: Identifies the current device thread index method. For GPU,
 * threadIdx.x is used as the thread index. Currently only GPU was supported.
 * Arity: The size of ins.
 * OpFunc: Compute functor which has an operator() as following:
 *     template <typename InT>
 *     struct XxxFunctor {
 *       HOSTDEVICE InT operator()(const InT* args) const {
 *         return ...;
 *       }
 *     };
 *
 * @param
 * out: The register pointer of out, the size is NX * NY.
 * ins: A pointers of array consisting of multiple inputs.
 * compute: Compute function which was declared like OpFunc<InT>().
 */
template <typename InT,
          typename OutT,
          int NX,
          int NY,
          int BlockSize,
          int Arity,
          class OpFunc>
__device__ __forceinline__ void ElementwiseAny(OutT* out,
                                               InT (*ins)[NX * NY],
                                               OpFunc compute) {
  InT args[Arity];
#pragma unroll
  for (int idx = 0; idx < NX * NY; ++idx) {
#pragma unroll
    for (int j = 0; j < Arity; ++j) {
      args[j] = ins[j][idx];
    }
    out[idx] = static_cast<OutT>(compute(args));
  }
}

/**
 * @brief Binary calculation according to OpFunc. Shape of in1 and in2 are the
 * different. Shape of in1 is [1, NX], but in2's shape is [NY, NX], the output
 * shape is [NY, NX].
 *
 * @template paraments
 * InT: The data type of in1 and in2.
 * OutT: The data type of out.
 * NX: The number of data columns loaded by each thread.
 * NY: The number of data rows loaded by each thread.
 * BlockSize: Identifies the current device thread index method. For GPU,
 * threadIdx.x is used as the thread index. Currently only GPU was supported.
 * OpFunc: Compute functor which has an operator() as following
 *     template <typename InT, typename OutT>
 *     struct XxxFunctor {
 *       HOSTDEVICE OutT operator()(const InT& a, const InT& b) const {
 *         return ...;
 *       }
 *     };
 *
 * @param
 * out: The register pointer of out, the size is NX * NY.
 * in1: The register pointer of fist input, size is NX * 1.
 * in2: The register pointer of second input, size is NX * NY.
 * compute: Compute function which was declared like OpFunc<InT, OutT>().
 */
template <typename InT,
          typename OutT,
          int NX,
          int NY,
          int BlockSize,
          class OpFunc>
__device__ __forceinline__ void CycleBinary(OutT* out,
                                            const InT* in1,
                                            const InT* in2,
                                            OpFunc compute) {
#pragma unroll
  for (int idx = 0; idx < NX; idx++) {
#pragma unroll
    for (int idy = 0; idy < NY; idy++) {
      out[idx + idy * NX] =
          static_cast<OutT>(compute(in1[idx], in2[idx + idy * NX]));
    }
  }
}

/**
 * @brief The Reduce provides collective methods for computing a parallel
 * reduction of items partitioned across a CUDA block and intra thread. When
 * ReduceMode == kLocalMode, thread reduce along nx. When ReduceMode ==
 * kGlobalMode, use shared memory to reduce between threads.
 *
 * @template paraments
 * T: The type of data.
 * NX: The number of data continuously loaded by each thread.
 * NY: The number of data rows loaded by each thread, only NY = 1 was supported.
 * BlockSize: Identifies the current device thread index method. For GPU,
 * threadIdx.x is used as the thread index. Currently only GPU was supported.
 * ReduceFunctor: Compute functor which has an operator() as following
 *     template <typename InT>
 *     struct ReduceFunctor {
 *       HOSTDEVICE InT operator()(const InT& a, const InT& b) const {
 *         return ...;
 *       }
 *     };
 * ReduceMode: Reduce mode, can be kLocalMode, kGlobalMode.
 *
 * @param
 * out: The register pointer of out, the size is NX * NY.
 * in: The register pointer of in, the size is NX * NY.
 * reducer: Compute function which was declared like ReduceFunctor<InT>().
 * reduce_last_dim: if the last dim gets involved in reduction.
 */
template <typename T,
          int NX,
          int NY,
          int BlockSize,
          class ReduceFunctor,
          details::ReduceMode Mode>
__device__ __forceinline__ void Reduce(T* out,
                                       const T* in,
                                       ReduceFunctor reducer,
                                       bool reduce_last_dim) {
  int block_index = blockDim.y;

  if (Mode == details::ReduceMode::kGlobalMode) {
    bool block_reduce_y = (!reduce_last_dim) && (block_index > 1);
    // when reduce is not required for the last dim, and reduce num has been
    // split into multiple threads
    if (block_reduce_y) {
#pragma unroll
      for (int i = 0; i < NY * NX; i++) {  // reduce along blockdim.y
        out[i] = details::BlockYReduce<T, ReduceFunctor>(out[i], reducer);
      }
    }

    // when last dimension need to be reduced
    if (reduce_last_dim) {
#pragma unroll
      for (int i = 0; i < NY * NX; i++) {  // reduce along blockDim.x
        out[i] = details::BlockXReduce<T, ReduceFunctor>(out[i], reducer);
      }
    }
  } else {  // else  kLocalMode
#pragma unroll
    for (int i = 0; i < NY; ++i) {
#pragma unroll
      for (int j = 0; j < NX; ++j) {
        out[i] = reducer(out[i], in[i * NX + j]);
      }
    }
  }
}

template <typename InT,
          typename OutT,
          int NX,
          int NY,
          int BlockSize,
          class OpFunc>
__device__ __forceinline__ void ElementwiseConstant(OutT* out, OpFunc compute) {
#pragma unroll
  for (int idx = 0; idx < NX * NY; idx++) {
    out[idx] = static_cast<OutT>(compute());
  }
}

template <typename StateType,
          typename OutT,
          int ReturnsCount,
          int BlockSize,
          class OpFunc>
__device__ __forceinline__ void ElementwiseRandom(OutT* out,
                                                  OpFunc compute,
                                                  StateType* state) {
  auto random_tuple = compute(state);
#pragma unroll
  for (int i = 0; i < ReturnsCount; i++) {
    out[i] = static_cast<OutT>((&random_tuple.x)[i]);
  }
}

// attention please set share_size = blockDim.x;
// data and b are the register pointer
#define shared_size 64
template <typename InT,
          typename OutT,
          int NX,
          int NY,
          int BlockSize,
          class OpFunc>
__device__ __forceinline__ void Cumsum(OutT* out,
                                       const InT* in,
                                       OpFunc compute) {
  __shared__ InT temp[shared_size * 2 + (shared_size * 2) / 32];
  int tidx = threadIdx.x;
  temp[tidx + tidx / 32] = in[0];
  temp[shared_size + tidx + (shared_size + tidx) / 32] = in[1];
  for (int stride = 1; stride <= blockDim.x; stride *= 2) {
    __syncthreads();
    int index = (tidx + 1) * 2 * stride - 1;
    if (index < (blockDim.x * 2)) {
      temp[index + index / 32] += temp[index - stride + (index - stride) / 32];
    }
  }
  for (int stride = (blockDim.x * 2) / 4; stride > 0; stride /= 2) {
    __syncthreads();
    int index = (tidx + 1) * 2 * stride - 1;
    if ((index + stride) < (blockDim.x * 2)) {
      temp[index + stride + (stride + index) / 32] +=
          temp[index + (index) / 32];
    }
  }

  __syncthreads();
  out[0] = static_cast<OutT>(temp[tidx + tidx / 32]);
  out[1] =
      static_cast<OutT>(temp[tidx + shared_size + (tidx + shared_size) / 32]);
}

#define SHARED_SIZE_LIMIT \
  1024  // each thread load 2 data from global memory so SHARED_SIZE_LIMIT must
        // larger than blockDim.x * 2
// if monotonic_type = 1 then increase
// if gridDim.x > 1 please set monotonic_type = blockIdx.x & 1; blockIdx.x % 2
// == 1 the increase
template <typename T>
__device__ __forceinline__ void Sort(T* dst,
                                     const T* src_data,
                                     int num,
                                     int monotonic_type) {
  // todo: set  num = Pow2(num)
  // shareMem for value and index  num must smaller than SHARED_SIZE_LIMIT / 2
  __shared__ T value[SHARED_SIZE_LIMIT];  // shareMem's size must larger than
                                          // blockDim * 2
  // Copy value and index from src and src_index
  value[threadIdx.x] = src_data[0];
  value[threadIdx.x + (SHARED_SIZE_LIMIT / 2)] = src_data[1];
  // make bitonicSort
  for (int size = 2; size < num; size <<= 1) {
    int bitonic_type = (threadIdx.x & (size / 2)) != 0;
    for (int stride = size / 2; stride > 0; stride >>= 1) {
      __syncthreads();
      int pos = 2 * threadIdx.x - (threadIdx.x & (stride - 1));
      details::Comparator<T>(&value[pos], &value[pos + stride], bitonic_type);
    }
  }
  // last sort
  for (int stride = SHARED_SIZE_LIMIT / 2; stride > 0; stride >>= 1) {
    __syncthreads();
    int pos = 2 * threadIdx.x - (threadIdx.x & (stride - 1));
    // last sort when monotonic_type = 1 then increase
    details::Comparator<T>(&value[pos], &value[pos + stride], monotonic_type);
  }
  __syncthreads();
  dst[0] = value[threadIdx.x];
  dst[1] = value[threadIdx.x + (SHARED_SIZE_LIMIT / 2)];
}

template <typename T, typename IndexType>
__device__ __forceinline__ void Sort(T* dst,
                                     IndexType* dst_index,
                                     const T* src_data,
                                     IndexType* src_index,
                                     int num,
                                     int monotonic_type) {
  // todo: set  num = Pow2(num)
  // shareMem for value and index  num must smaller than SHARED_SIZE_LIMIT / 2
  __shared__ T value[SHARED_SIZE_LIMIT];  // shareMem's size must larger than
                                          // blockDim * 2
  __shared__ IndexType index[SHARED_SIZE_LIMIT];
  // Copy value and index from src and src_index
  value[threadIdx.x] = src_data[0];
  value[threadIdx.x + (SHARED_SIZE_LIMIT / 2)] = src_data[1];
  // index
  index[threadIdx.x] = src_index[0];
  index[threadIdx.x + (SHARED_SIZE_LIMIT / 2)] = src_index[1];
  // make bitonicSort
  for (int size = 2; size < num; size <<= 1) {
    int bitonic_type = (threadIdx.x & (size / 2)) != 0;
    for (int stride = size / 2; stride > 0; stride >>= 1) {
      __syncthreads();
      int pos = 2 * threadIdx.x - (threadIdx.x & (stride - 1));
      details::ComparatorWithIndex<T, IndexType>(&value[pos],
                                                 &value[pos + stride],
                                                 &index[pos],
                                                 &index[pos + stride],
                                                 bitonic_type);
    }
  }

  for (int stride = SHARED_SIZE_LIMIT / 2; stride > 0; stride >>= 1) {
    __syncthreads();
    int pos = 2 * threadIdx.x - (threadIdx.x & (stride - 1));
    // last sort when monotonic_type = 1 then increase
    details::ComparatorWithIndex<T, IndexType>(&value[pos],
                                               &value[pos + stride],
                                               &index[pos],
                                               &index[pos + stride],
                                               monotonic_type);
  }

  __syncthreads();
  dst[0] = value[threadIdx.x];
  dst[1] = value[threadIdx.x + (SHARED_SIZE_LIMIT / 2)];
  dst_index[0] = index[threadIdx.x];
  dst_index[1] = index[threadIdx.x + (SHARED_SIZE_LIMIT / 2)];
}

}  // namespace kps
}  // namespace phi
