#ifndef PHI_KERNEL
#define PHI_KERNEL

include "paddle/infrt/dialect/phi/infrt_phi_tensor.td"

def PHI_KernelDialect : Dialect {
  let name = "phi_kernel";

  let description = [{
    The PHI Kernel dialect.
  }];

  let cppNamespace = "::infrt::phi";
}

// PHI Kernel related ops.
class PDT_Kernel<string mnemonic, list<OpTrait> traits = []> : Op<PHI_KernelDialect, mnemonic, !listconcat(traits, [IsolatedFromAbove])> {
}

def FakeKernelOp : PDT_Kernel<"phi.matmul.host.fp32"> {
  let arguments = (ins CPU_Context:$dev_ctx, DenseTensor:$x, DenseTensor:$y, BoolAttr:$transpose_x, BoolAttr:$transpose_y);
  let results = (outs DenseTensor:$output);
}

def PDCK_AbsOp : PDT_Kernel<"phi.abs.host.fp32"> {
  let arguments = (ins CPU_Context:$dev_ctx, DenseTensor:$x);
  let results = (outs DenseTensor:$output);
}

#endif

