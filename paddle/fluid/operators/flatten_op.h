/* Copyright (c) 2019 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#pragma once
#include <vector>
#include "paddle/fluid/framework/op_registry.h"
#include "paddle/fluid/framework/pten_utils.h"
#include "paddle/fluid/operators/math/pooling.h"
#include "paddle/fluid/platform/device_context.h"
#include "paddle/phi/kernels/empty_kernel.h"
#include "paddle/phi/kernels/flatten_grad_kernel.h"
#include "paddle/phi/kernels/flatten_kernel.h"
#include "paddle/phi/kernels/funcs/blas/blas.h"
#include "paddle/phi/kernels/funcs/math_function.h"

namespace paddle {
namespace operators {

template <typename DeviceContext, typename T>
class FlattenKernel : public framework::OpKernel<T> {
 public:
  void Compute(const framework::ExecutionContext &context) const override {
    auto *in = context.Input<framework::LoDTensor>("X");
    auto *out = context.Output<framework::LoDTensor>("Out");

    auto &axes = context.Attr<int>("axis");
    auto x_dims = in->dims();
    auto out_dims = phi::make_ddim(GetOutputShape(axes, x_dims));

    out->mutable_data(context.GetPlace(), in->type());
    framework::TensorCopy(
        *in, context.GetPlace(),
        context.template device_context<platform::DeviceContext>(), out);
    out->Resize(out_dims);
  }

  static std::vector<int32_t> GetOutputShape(const int axis,
                                             const framework::DDim &in_dims) {
    int64_t outer = 1, inner = 1;
    for (int i = 0; i < in_dims.size(); ++i) {
      if (i < axis) {
        outer *= in_dims[i];
      } else {
        inner *= in_dims[i];
      }
    }
    std::vector<int32_t> out_shape(2);
    out_shape[0] = outer;
    out_shape[1] = inner;
    return out_shape;
  }
};

template <typename DeviceContext, typename T>
class FlattenGradKernel : public framework::OpKernel<T> {
 public:
  void Compute(const framework::ExecutionContext &ctx) const override {
    auto *d_x = ctx.Output<framework::LoDTensor>(framework::GradVarName("X"));
    auto *d_out =
        ctx.Input<framework::LoDTensor>(framework::GradVarName("Out"));
    auto in_dims = ctx.Input<framework::LoDTensor>("X")->dims();

    d_x->mutable_data(ctx.GetPlace(), d_out->type());
    framework::TensorCopy(
        *d_out, ctx.GetPlace(),
        ctx.template device_context<platform::DeviceContext>(), d_x);
    d_x->Resize(in_dims);
  }
};

template <typename DeviceContext, typename T>
class Flatten2Kernel : public framework::OpKernel<T> {
 public:
  void Compute(const framework::ExecutionContext &context) const override {
    auto &axes = context.Attr<int>("axis");

    auto *in = context.Input<framework::LoDTensor>("X");
    auto x_dims = in->dims();

    auto *out = context.Output<framework::LoDTensor>("Out");

    auto out_dims = phi::make_ddim(
        FlattenKernel<DeviceContext, T>::GetOutputShape(axes, x_dims));

    out->mutable_data(context.GetPlace(), in->type());
    framework::TensorCopy(
        *in, context.GetPlace(),
        context.template device_context<platform::DeviceContext>(), out);
    out->Resize(out_dims);
  }
};

template <typename DeviceContext, typename T>
class Flatten2GradKernel : public framework::OpKernel<T> {
 public:
  void Compute(const framework::ExecutionContext &ctx) const override {
    auto *d_x = ctx.Output<framework::LoDTensor>(framework::GradVarName("X"));
    auto *d_out =
        ctx.Input<framework::LoDTensor>(framework::GradVarName("Out"));

    auto xshape_dims = ctx.Input<framework::LoDTensor>("XShape")->dims();
    auto x_dims = phi::slice_ddim(xshape_dims, 1, xshape_dims.size());

    d_x->mutable_data(ctx.GetPlace(), d_out->type());
    framework::TensorCopy(
        *d_out, ctx.GetPlace(),
        ctx.template device_context<platform::DeviceContext>(), d_x);
    d_x->Resize(x_dims);
  }
};

template <typename DeviceContext, typename T>
class FlattenContiguousRangeKernel : public framework::OpKernel<T> {
 public:
  void Compute(const framework::ExecutionContext &context) const override {
    auto *in = context.Input<framework::LoDTensor>("X");
    auto *out = context.Output<framework::LoDTensor>("Out");
    out->mutable_data(context.GetPlace(), in->type());
    auto &start_axis = context.Attr<int>("start_axis");
    auto &stop_axis = context.Attr<int>("stop_axis");
    auto &dev_ctx = context.device_context<DeviceContext>();

    // call new kernel
    phi::FlattenKernel<T, typename paddle::framework::ConvertToPtenContext<
                              DeviceContext>::TYPE>(
        static_cast<const typename paddle::framework::ConvertToPtenContext<
            DeviceContext>::TYPE &>(dev_ctx),
        *in, start_axis, stop_axis, out);
  }
};

template <typename DeviceContext, typename T>
class FlattenContiguousRangeGradKernel : public framework::OpKernel<T> {
 public:
  void Compute(const framework::ExecutionContext &ctx) const override {
    auto *d_x = ctx.Output<framework::LoDTensor>(framework::GradVarName("X"));
    auto *d_out =
        ctx.Input<framework::LoDTensor>(framework::GradVarName("Out"));
    auto *xshape = ctx.Input<framework::LoDTensor>("XShape");

    d_x->mutable_data(ctx.GetPlace(), d_out->type());
    auto &dev_ctx = ctx.device_context<DeviceContext>();

    // call new kernel
    phi::FlattenGradKernel<T, typename paddle::framework::ConvertToPtenContext<
                                  DeviceContext>::TYPE>(
        static_cast<const typename paddle::framework::ConvertToPtenContext<
            DeviceContext>::TYPE &>(dev_ctx),
        *d_out, *xshape, d_x);
  }
};

}  // namespace operators
}  // namespace paddle
