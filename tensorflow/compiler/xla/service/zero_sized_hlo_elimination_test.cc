/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/xla/service/zero_sized_hlo_elimination.h"

#include <memory>
#include <vector>

#include "tensorflow/compiler/xla/hlo/ir/hlo_computation.h"
#include "tensorflow/compiler/xla/hlo/ir/hlo_instruction.h"
#include "tensorflow/compiler/xla/hlo/ir/hlo_module.h"
#include "tensorflow/compiler/xla/hlo/ir/hlo_opcode.h"
#include "tensorflow/compiler/xla/literal.h"
#include "tensorflow/compiler/xla/service/shape_inference.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/compiler/xla/test.h"
#include "tensorflow/compiler/xla/test_helpers.h"
#include "tensorflow/compiler/xla/tests/hlo_test_base.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"
#include "tensorflow/tsl/platform/logging.h"

namespace xla {
namespace {
class ZeroSizedHloEliminationTest : public HloTestBase {
 protected:
  ZeroSizedHloEliminationTest()
      : HloTestBase(),
        builder_("zero_sized_computation"),
        zero_sized_param_(
            builder_.AddInstruction(HloInstruction::CreateParameter(
                0, ShapeUtil::MakeShape(F32, {3, 0}), "zero sized param"))) {}

  StatusOr<bool> RunZeroSizedElimination() {
    auto module = CreateNewVerifiedModule("zero_sized_elimination_test_module");
    module->AddEntryComputation(builder_.Build());
    return ZeroSizedHloElimination{}.Run(module.get());
  }

  HloComputation::Builder builder_;
  HloInstruction* zero_sized_param_;
};

TEST_F(ZeroSizedHloEliminationTest, EliminatedZeroSizedOp) {
  builder_.AddInstruction(HloInstruction::CreateUnary(
      zero_sized_param_->shape(), HloOpcode::kTanh, zero_sized_param_));
  TF_ASSERT_OK_AND_ASSIGN(bool changed, RunZeroSizedElimination());
  EXPECT_TRUE(changed);
}

TEST_F(ZeroSizedHloEliminationTest, DoesNotEliminateParameter) {
  TF_ASSERT_OK_AND_ASSIGN(bool changed, RunZeroSizedElimination());
  EXPECT_FALSE(changed);
}

TEST_F(ZeroSizedHloEliminationTest, DoesNotEliminateSideEffects) {
  auto token = builder_.AddInstruction(HloInstruction::CreateToken());
  auto send = builder_.AddInstruction(
      HloInstruction::CreateSend(zero_sized_param_, token, 0));
  builder_.AddInstruction(HloInstruction::CreateSendDone(send));
  TF_ASSERT_OK_AND_ASSIGN(bool changed, RunZeroSizedElimination());
  EXPECT_FALSE(changed);
}

TEST_F(ZeroSizedHloEliminationTest, DoesNotEliminateConstant) {
  builder_.AddInstruction(
      HloInstruction::CreateConstant(LiteralUtil::CreateR1({})));
  TF_ASSERT_OK_AND_ASSIGN(bool changed, RunZeroSizedElimination());
  EXPECT_FALSE(changed);
}

TEST_F(ZeroSizedHloEliminationTest, ZeroSizedInstructionWithoutLayoutFolded) {
  Shape op_shape = ShapeUtil::MakeShape(F32, {4, 0});
  op_shape.clear_layout();
  HloInstruction* param1 = builder_.AddInstruction(
      HloInstruction::CreateParameter(1, op_shape, "zero sized param 1"));
  HloInstruction* param2 = builder_.AddInstruction(
      HloInstruction::CreateParameter(2, op_shape, "zero sized param 2"));
  builder_.AddInstruction(
      HloInstruction::CreateBinary(op_shape, HloOpcode::kAdd, param1, param2));
  TF_ASSERT_OK_AND_ASSIGN(bool changed, RunZeroSizedElimination());
  EXPECT_TRUE(changed);
}

}  // namespace
}  // namespace xla
