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

// This transformation pass transforms functional control flow operations in the
// standard TensorFlow dialect to MLIR Control Flow Graph (CFG) form.

#include "mlir/IR/Builders.h"  // TF:local_config_mlir
#include "mlir/IR/Operation.h"  // TF:local_config_mlir
#include "mlir/IR/Value.h"  // TF:local_config_mlir
#include "mlir/Pass/Pass.h"  // TF:local_config_mlir
#include "mlir/Pass/PassRegistry.h"  // TF:local_config_mlir
#include "mlir/StandardOps/Ops.h"  // TF:local_config_mlir
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops.h"
#include "tensorflow/compiler/mlir/tensorflow/transforms/passes.h"

using ::mlir::Block;
using ::mlir::BranchOp;
using ::mlir::CallOp;
using ::mlir::CondBranchOp;
using ::mlir::ExtractElementOp;
using ::mlir::Function;
using ::mlir::FunctionPass;
using ::mlir::FunctionPassBase;
using ::mlir::FunctionType;
using ::mlir::Location;
using ::mlir::OpBuilder;
using ::mlir::Operation;
using ::mlir::OpOperand;
using ::mlir::PassRegistration;
using ::mlir::TensorCastOp;
using ::mlir::TensorType;
using ::mlir::Type;
using ::mlir::Value;

namespace TF = ::mlir::TF;

namespace {
struct FunctionalControlFlowToCFG
    : public FunctionPass<FunctionalControlFlowToCFG> {
  void runOnFunction() override;
};
}  // end anonymous namespace

FunctionPassBase* mlir::createTFFunctionalControlFlowToCFG() {
  return new FunctionalControlFlowToCFG();
}

// Lower a general tensor argument that is used as a condition to a functional
// control flow op into an i1 value.  This needs to implement the general
// TensorFlow semantics, which are:
//
//   If the tensor is a scalar of non-boolean type, the scalar is converted to a
//   boolean according to the following rule: if the scalar is a numerical
//   value, non-zero means True and zero means False; if the scalar is a string,
//   non-empty means True and empty means False. If the tensor is not a scalar,
//   being empty means False and being non-empty means True.
//
static Value* LowerCondition(Location loc, Value* value, OpBuilder* builder) {
  // TODO: Right now we just handle zero-D tensors of boolean values.
  // FIXME: This is almost all wrong, but is a placeholder to unblock the one
  // testcases, later patches will build on this once I build the right infra to
  // support it.
  TensorType type = value->getType().cast<TensorType>();
  if (!type.hasRank() || type.getRank() != 0 ||
      !type.getElementType().isInteger(1)) {
    return emitError(loc, "only supports zero-D bool tensors now"), nullptr;
  }

  auto scalar = builder->create<ExtractElementOp>(loc, value);
  return scalar.getResult();
}

/// Call the function `fn` with arguments provided by the given function and
/// return the CallOp. Arguments are cast to the required type before calling
/// the function.
///
/// Requires the function to provide arguments for each of the `fn` operands
/// that is compatible for tensor cast.
///
static Operation* CallFn(Location loc,
                         const std::function<Value*(int)>& get_arg,
                         Function* fn, OpBuilder* builder) {
  FunctionType fn_type = fn->getType();
  llvm::SmallVector<Value*, 4> operands;
  int num_operands = fn_type.getNumInputs();
  operands.reserve(num_operands);
  for (int i = 0; i < num_operands; ++i) {
    Value* val = get_arg(i);
    Type expected = fn_type.getInput(i);
    if (val->getType() != expected)
      val = builder->create<TensorCastOp>(loc, val, expected);
    operands.push_back(val);
  }
  return builder->create<CallOp>(loc, fn, operands).getOperation();
}

/// Prepare for jump to the given block by introducing necessary tensor_cast
/// operations and returning Values of types required by the block.
///
/// Requires the function to provide values for each of the block arguments and
/// they should be pair-wise compatible for tensor cast.
static llvm::SmallVector<Value*, 4> PrepareValsForJump(
    Location loc, const std::function<Value*(int)>& get_val, Block* block,
    OpBuilder* builder) {
  llvm::SmallVector<Value*, 4> result;
  int num_vals = block->getNumArguments();
  result.reserve(num_vals);
  for (int i = 0; i < num_vals; ++i) {
    Value* val = get_val(i);
    Type expected = block->getArgument(i)->getType();
    if (val->getType() != expected)
      val = builder->create<TensorCastOp>(loc, val, expected);
    result.push_back(val);
  }
  return result;
}

/// Jump to the given block with arguments provided by the function. Arguments
/// are cast to the required type before the jump.
///
/// Requires the function to provide values for each of the block arguments and
/// they should be pair-wise compatible for tensor cast.
static void JumpToBlock(Location loc, const std::function<Value*(int)>& get_arg,
                        Block* block, OpBuilder* builder) {
  auto operands = PrepareValsForJump(loc, get_arg, block, builder);
  builder->create<BranchOp>(loc, block, operands);
}

// Replace all uses of the operation results in this block with block arguments.
//
// Requires that the block has same number of arguments as number of results of
// the operation and either they have same types or are more generic types and
// it is possible to cast them to results' types.
//
static void ReplaceOpResultWithBlockArgs(Location loc, Operation* op,
                                         Block* block, OpBuilder* builder) {
  assert(op->getNumResults() == block->getNumArguments());
  for (unsigned i = 0, e = op->getNumResults(); i != e; ++i) {
    Value* arg = block->getArgument(i);
    Value* result = op->getResult(i);
    if (arg->getType() != result->getType())
      arg = builder->create<TensorCastOp>(loc, arg, result->getType());
    result->replaceAllUsesWith(arg);
  }
}

// Given a functional IfOp, transform the enclosing code to eliminate it
// completely from the IR, breaking it into operations to evaluate the condition
// as a bool, plus some branches.
//
// This returns true on failure.
//
static bool LowerIfOp(TF::IfOp op) {
  Operation* op_inst = op.getOperation();
  Location loc = op_inst->getLoc();

  OpBuilder builder(op_inst);

  // Lower the condition to a boolean value (i1).
  Value* cond_i1 = LowerCondition(loc, op.getCondition(), &builder);
  if (!cond_i1) return true;

  auto* module = op_inst->getFunction()->getModule();
  auto* then_fn = module->getNamedFunction(op.getThen());
  auto* else_fn = module->getNamedFunction(op.getElse());

  // Split the basic block before the 'if'.  The new dest will be our merge
  // point.
  Block* orig_block = op_inst->getBlock();
  Block* merge_block = orig_block->splitBlock(op);

  // Add the block arguments to the merge point, and replace all uses of the
  // original operation results with them.
  for (Value* value : op_inst->getResults())
    merge_block->addArgument(value->getType());
  ReplaceOpResultWithBlockArgs(loc, op_inst, merge_block, &builder);

  // Get arguments to the branches after dropping the condition which is the
  // first operand.
  auto get_operand = [&](int i) { return op_inst->getOperand(i + 1); };

  // Set up the 'then' block.
  Block* then_block = builder.createBlock(merge_block);
  Operation* call_op = CallFn(loc, get_operand, then_fn, &builder);

  auto get_then_result = [&](int i) { return call_op->getResult(i); };
  JumpToBlock(loc, get_then_result, merge_block, &builder);

  // Set up the 'else' block.
  Block* else_block = builder.createBlock(merge_block);
  call_op = CallFn(loc, get_operand, else_fn, &builder);

  auto get_else_result = [&](int i) { return call_op->getResult(i); };
  JumpToBlock(loc, get_else_result, merge_block, &builder);

  // Now that we have the then and else blocks, replace the terminator of the
  // orig_block with a conditional branch.
  builder.setInsertionPointToEnd(orig_block);
  builder.create<CondBranchOp>(loc, cond_i1, then_block,
                               llvm::ArrayRef<Value*>(), else_block,
                               llvm::ArrayRef<Value*>());

  // Finally, delete the op in question.
  op_inst->erase();
  return false;
}

// Given a functional WhileOp, transform the enclosing code to eliminate it
// completely from the IR, breaking it into operations to execute the loop body
// repeatedly while the loop condition is true.
//
// This returns true on failure.
//
static bool LowerWhileOp(TF::WhileOp op) {
  Operation* op_inst = op.getOperation();
  Location loc = op_inst->getLoc();

  OpBuilder builder(op_inst);

  auto* module = op_inst->getFunction()->getModule();
  auto* cond_fn = module->getNamedFunction(op.getCond());
  auto* body_fn = module->getNamedFunction(op.getBody());

  // Split the block containing the While op into two blocks.  One containing
  // operations before the While op and other containing the rest.  Create two
  // new blocks to call condition and body functions.
  //
  // The final control flow graph would be as follows:
  //
  // ...
  // orig_block_head(...):
  //   ...
  //   br cond_block(...)
  // cond_block(...):
  //   %A = call @cond(...)
  //   cond br %A, body_block(...), orig_block_tail(...)
  // body_block(...):
  //   %B = call @body(...)
  //   br cond_block(...)
  // orig_block_tail(...):
  //   ...
  //
  Block* orig_block_head = op_inst->getBlock();
  Block* orig_block_tail = orig_block_head->splitBlock(op);
  Block* cond_block = builder.createBlock(orig_block_tail);
  Block* body_block = builder.createBlock(orig_block_tail);

  // Set argument types for the cond_block to be same as the types of the
  // condition function and argument types for the other two blocks to be same
  // as the input types of the body function.  Note that it is always possible
  // for body_block and orig_block_tail to have arguments of the same types as
  // they have exactly one call-site and they are sharing the operands.
  for (Type type : cond_fn->getType().getInputs()) {
    cond_block->addArgument(type);
  }
  for (Type type : body_fn->getType().getInputs()) {
    body_block->addArgument(type);
    orig_block_tail->addArgument(type);
  }

  auto get_operand = [&](int i) { return op_inst->getOperand(i); };

  // Unconditionally branch from the original block to the block containing the
  // condition.
  builder.setInsertionPointToEnd(orig_block_head);
  JumpToBlock(loc, get_operand, cond_block, &builder);

  // Call condition function in the condition block and then branch to the body
  // block or remainder of the original block depending on condition function
  // result.
  builder.setInsertionPointToEnd(cond_block);

  auto get_cond_arg = [&](int i) { return cond_block->getArgument(i); };
  Operation* cond_call_op = CallFn(loc, get_cond_arg, cond_fn, &builder);

  assert(cond_call_op->getNumResults() == 1);
  Value* condition = LowerCondition(loc, cond_call_op->getResult(0), &builder);
  auto br_operands =
      PrepareValsForJump(loc, get_cond_arg, body_block, &builder);
  builder.create<CondBranchOp>(loc, condition, body_block, br_operands,
                               orig_block_tail, br_operands);

  // Call body function in the body block and then unconditionally branch back
  // to the condition block.
  builder.setInsertionPointToEnd(body_block);
  auto get_body_arg = [&](int i) { return body_block->getArgument(i); };
  Operation* body_call_op = CallFn(loc, get_body_arg, body_fn, &builder);

  auto get_body_result = [&](int i) { return body_call_op->getResult(i); };
  JumpToBlock(loc, get_body_result, cond_block, &builder);

  // Replace use of the while loop results with block inputs in the remainder of
  // the original block and then delete the original While operation.
  builder.setInsertionPoint(&orig_block_tail->front());
  ReplaceOpResultWithBlockArgs(loc, op_inst, orig_block_tail, &builder);
  op_inst->erase();

  return false;
}

void FunctionalControlFlowToCFG::runOnFunction() {
  // Scan the function looking for these ops.
  for (Block& block : getFunction()) {
    for (Operation& op : block) {
      // If the operation is one of the control flow ops we know, lower it.
      // If we lower an operation, then the current basic block will be split,
      // and the operation will be removed, so we should continue looking at
      // subsequent blocks.
      //
      // TODO: Use PatternRewriter to eliminate these function control flow ops.
      if (TF::IfOp if_op = llvm::dyn_cast<TF::IfOp>(op)) {
        if (LowerIfOp(if_op)) return signalPassFailure();
        break;
      }
      if (TF::WhileOp while_op = llvm::dyn_cast<TF::WhileOp>(op)) {
        if (LowerWhileOp(while_op)) return signalPassFailure();
        break;
      }
    }
  }
}

static PassRegistration<FunctionalControlFlowToCFG> pass(
    "tf-functional-control-flow-to-cfg",
    "Transform functional control flow Ops to MLIR Control Form Graph "
    "(CFG) form");