#ifndef CAFFE2_OPERATORS_PAD_OP_H_
#define CAFFE2_OPERATORS_PAD_OP_H_

#include "caffe2/core/context.h"
#include "caffe2/core/operator.h"
#include "caffe2/operators/conv_pool_op_base.h"
#include "caffe2/utils/math.h"
#include "caffe2/core/logging.h"

namespace caffe2 {

// Padding mode similar to numpy.
enum class PadMode {
  CONSTANT = 0, // pad constant values, with string "constant"
  REFLECT = 1, // pads with reflect values, with string "reflect"
  EDGE = 2, // pads with the edge values, with string "edge"
};

PadMode StringToPadMode(const string&);

template <typename T, class Context>
class PadImageOp final : public ConvPoolOpBase<Context> {
 public:
  USE_CONV_POOL_BASE_FUNCTIONS(Context);
  PadImageOp(const OperatorDef& operator_def, Workspace* ws)
      : ConvPoolOpBase<Context>(operator_def, ws),
        mode_(StringToPadMode(
            OperatorBase::GetSingleArgument<string>("mode", "constant"))),
        value_(static_cast<T>(
            OperatorBase::GetSingleArgument<float>("value", 0.0))) {
    CAFFE_ENFORCE(
        legacy_pad_ == LegacyPadding::NOTSET,
        "Padding layer only supports explicit pad values.");
    CAFFE_ENFORCE(
        dilation_h_ == 1 && dilation_w_ == 1,
        "Pooling op does not support dilation right now.");
    CAFFE_ENFORCE(
        stride_h_ == 1 && stride_w_ == 1,
        "Pooling op does not support stride right now.");
    // Pad op does not use kernel sizes, so we set it to 1 for computing the
    // output size.
    kernel_h_ = kernel_w_ = 1;
  }
  ~PadImageOp() {}

  bool RunOnDeviceWithOrderNCHW() override;
  bool RunOnDeviceWithOrderNHWC() override;

 private:
  PadMode mode_;
  T value_;

  // Input: X
  // Output: Y
};

template <typename T, class Context>
class PadImageGradientOp final : public ConvPoolOpBase<Context> {
 public:
  USE_CONV_POOL_BASE_FUNCTIONS(Context);
  PadImageGradientOp(const OperatorDef& operator_def, Workspace* ws)
      : ConvPoolOpBase<Context>(operator_def, ws),
        mode_(StringToPadMode(
            OperatorBase::GetSingleArgument<string>("mode", "constant"))) {
    CAFFE_ENFORCE(
        legacy_pad_ == LegacyPadding::NOTSET,
        "Padding layer only supports explicit pad values.");
    CAFFE_ENFORCE(
        dilation_h_ == 1 && dilation_w_ == 1,
        "Pooling op does not support dilation right now.");
    // Pad op does not use kernel sizes, so we set it to 1 for computing the
    // output size.
    kernel_h_ = kernel_w_ = 1;
  }
  ~PadImageGradientOp() {}

  bool RunOnDeviceWithOrderNCHW() override;
  bool RunOnDeviceWithOrderNHWC() override;

 private:
  PadMode mode_;
  // Input: dY
  // Output: dX
};

}  // namespace caffe2

#endif // CAFFE2_OPERATORS_PAD_OP_H_
