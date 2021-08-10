// (c) Copyright 2021 Xilinx Inc. All Rights Reserved.

#include "XTenToLinalgPass.h"

#include "mlir/Dialect/Linalg/EDSC/Intrinsics.h"
#include "mlir/Dialect/Linalg/IR/LinalgOps.h"
#include "mlir/Dialect/Linalg/Transforms/Transforms.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/SCF.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#include "XTenDialect.h"
#include "XTenOps.h"

#define DEBUG_TYPE "xten-to-linalg-pass"

using namespace mlir;
using namespace xilinx;

namespace {

#include "XTenToLinalg.cpp.inc"

Value typeCast(PatternRewriter &builder, Value val, Type destTy) {
  if (val.getType() == destTy)
    return val;
  return builder.create<NPCOMP::aten::TypeCastOp>(val.getLoc(), destTy, val)
      .getResult();
}

/// Create a type cast to memref
Value MemRefTypeCast(PatternRewriter &builder, Value val) {
  if (val.getType().isa<MemRefType>())
    return val;
  auto tensorTy = val.getType().dyn_cast<TensorType>();
  if (!tensorTy)
    return val;
  auto memRefType = mlir::MemRefType::get(tensorTy.getShape(), tensorTy.getElementType(), {}, 0);
  return typeCast(builder, val, memRefType);
}

template <class T>
class XTenBinaryOpConversion : public ConversionPattern {
public:
  XTenBinaryOpConversion(StringRef rootName, PatternBenefit benefit, MLIRContext *ctx)
      : ConversionPattern(rootName, benefit, ctx) {}

  LogicalResult
  matchAndRewrite(Operation *op, ArrayRef<Value > operands,
                  ConversionPatternRewriter &rewriter) const override {
    auto loc = op->getLoc();

    auto A = MemRefTypeCast(rewriter, operands[0]);
    auto B = MemRefTypeCast(rewriter, operands[1]);

    auto resultTy = op->getResult(0).getType();
    auto tensorResultTy = resultTy.cast<TensorType>();
    auto elementTy = tensorResultTy.getElementType();
    auto rank = tensorResultTy.getRank();
    auto memRefResultTy = mlir::MemRefType::get(tensorResultTy.getShape(),
                                                tensorResultTy.getElementType(),
                                                {}, 0);

    auto C = rewriter.create<memref::AllocOp>(loc, memRefResultTy);

    SmallVector<Value, 2> inputTensors{A,B};
    SmallVector<Value, 1> outputTensors{C};

    auto identMap = rewriter.getMultiDimIdentityMap(rank);
    SmallVector<AffineMap, 4> indexMap(3, identMap);

    /*auto linalgOp =*/ rewriter.create<linalg::GenericOp>(
      loc, ArrayRef<Type>(), inputTensors, outputTensors, indexMap,
      SmallVector<StringRef>(rank, getParallelIteratorTypeName()),
      "", static_cast<const T*>(this)->getDefaultLibraryFunc(),
      [&](OpBuilder &nestedBuilder, Location nestedLoc, ValueRange blockArgs) {
        auto result
          = static_cast<const T*>(this)->emitBinaryOp(op,
                                                      elementTy,
                                                      rewriter,
                                                      blockArgs[0],
                                                      blockArgs[1]);
        nestedBuilder.create<linalg::YieldOp>(loc, result);
      });

    auto tensor_cast
      = rewriter.create<NPCOMP::aten::TypeCastOp>(loc,
                                                  tensorResultTy,
                                                  C->getResult(0));

    rewriter.replaceOp(op, tensor_cast.getResult());
    return success();
  }
};

class XTenAddOpConversion : public XTenBinaryOpConversion<XTenAddOpConversion> {
public:
  explicit XTenAddOpConversion(MLIRContext *context)
      : XTenBinaryOpConversion(xten::AddOp::getOperationName(), 1, context) {}

  StringRef getDefaultLibraryFunc() const {
      return "air_add_op";
  }

  Value
  emitBinaryOp(Operation *op, Type elementTy,
               ConversionPatternRewriter &rewriter, Value a, Value b) const {
    if (elementTy.isa<FloatType>())
      return rewriter.create<AddFOp>(op->getLoc(), a, b);
    else
      return rewriter.create<AddIOp>(op->getLoc(), a, b);
  }
};

class XTenMulOpConversion : public XTenBinaryOpConversion<XTenMulOpConversion> {
public:
  explicit XTenMulOpConversion(MLIRContext *context)
      : XTenBinaryOpConversion(xten::MulOp::getOperationName(), 1, context) {}

  StringRef getDefaultLibraryFunc() const {
      return "air_mul_op";
  }

  Value
  emitBinaryOp(Operation *op, Type elementTy,
               ConversionPatternRewriter &rewriter, Value a, Value b) const {
    if (elementTy.isa<FloatType>())
      return rewriter.create<MulFOp>(op->getLoc(), a, b);
    else
      return rewriter.create<MulIOp>(op->getLoc(), a, b);
  }
};

class XTenMMOpConversion : public ConversionPattern {
public:
  explicit XTenMMOpConversion(MLIRContext *context)
      : ConversionPattern(xten::MMOp::getOperationName(), 1, context) {}

  LogicalResult
  matchAndRewrite(Operation *op, ArrayRef<Value > operands,
                  ConversionPatternRewriter &rewriter) const override {
    auto mmult = cast<xten::MMOp>(op);
    auto loc = mmult.getLoc();

    edsc::ScopedContext scope(rewriter, loc);

    auto A = MemRefTypeCast(rewriter, operands[0]);
    auto B = MemRefTypeCast(rewriter, operands[1]);

    auto resultTy = op->getResult(0).getType();
    auto tensorResultTy = resultTy.cast<TensorType>();
    auto memRefResultTy = mlir::MemRefType::get(tensorResultTy.getShape(),
                                                tensorResultTy.getElementType(),
                                                {}, 0);

    auto C = rewriter.create<memref::AllocOp>(loc, memRefResultTy);

    edsc::intrinsics::linalg_matmul(ValueRange{A, B}, ValueRange{C});

    auto tensor_cast
      = rewriter.create<NPCOMP::aten::TypeCastOp>(loc,
                                                  tensorResultTy,
                                                  C->getResult(0));

    rewriter.replaceOp(op, tensor_cast.getResult());
    return success();
  }
};

class XTenConv2dOpConversion : public ConversionPattern {
public:
  explicit XTenConv2dOpConversion(MLIRContext *context)
      : ConversionPattern(xten::Conv2dOp::getOperationName(), 1, context) {}

  LogicalResult
  matchAndRewrite(Operation *op, ArrayRef<Value > operands,
                  ConversionPatternRewriter &rewriter) const override {
    auto mmult = cast<xten::Conv2dOp>(op);
    auto loc = mmult.getLoc();

    edsc::ScopedContext scope(rewriter, loc);

    auto A = MemRefTypeCast(rewriter, operands[0]);
    auto B = MemRefTypeCast(rewriter, operands[1]);

    auto resultTy = op->getResult(0).getType();
    auto tensorResultTy = resultTy.cast<TensorType>();
    auto memRefResultTy = mlir::MemRefType::get(tensorResultTy.getShape(),
                                                tensorResultTy.getElementType(),
                                                {}, 0);

    auto C = rewriter.create<memref::AllocOp>(loc, memRefResultTy);

    rewriter.create<linalg::ConvNCHWOp>(loc, ValueRange{A, B}, ValueRange{C});

    auto tensor_cast
      = rewriter.create<NPCOMP::aten::TypeCastOp>(loc,
                                                  tensorResultTy,
                                                  C->getResult(0));

    rewriter.replaceOp(op, tensor_cast.getResult());
    return success();
  }
};

class XTenPartialConv2dReLUOpConversion : public ConversionPattern {
public:
  explicit XTenPartialConv2dReLUOpConversion(MLIRContext *context)
      : ConversionPattern(xten::PartialConv2dReLUOp::getOperationName(), 1, context) {}

  LogicalResult
  matchAndRewrite(Operation *op, ArrayRef<Value > operands,
                  ConversionPatternRewriter &rewriter) const override {
    auto mmult = cast<xten::PartialConv2dReLUOp>(op);
    auto loc = mmult.getLoc();

    edsc::ScopedContext scope(rewriter, loc);

    auto A = MemRefTypeCast(rewriter, operands[0]);
    auto B = MemRefTypeCast(rewriter, mmult.weight());

    auto resultTy = op->getResult(0).getType();
    auto tensorResultTy = resultTy.cast<TensorType>();
    auto memRefResultTy = mlir::MemRefType::get(tensorResultTy.getShape(),
                                                tensorResultTy.getElementType(),
                                                {}, 0);

    Value C;
    if(mmult.PartialIn()) {
      C = mmult.PartialIn();
    } else {
      C = rewriter.create<memref::AllocOp>(loc, memRefResultTy).getResult();
    }

    rewriter.create<linalg::ConvNCHWOp>(loc, ValueRange{A, B}, ValueRange{C});

    auto tensor_cast
      = rewriter.create<NPCOMP::aten::TypeCastOp>(loc,
                                                  tensorResultTy,
                                                  C);

    if(mmult.getNumResults() == 1)
      rewriter.replaceOp(op, tensor_cast.getResult());
    else
      rewriter.replaceOp(op, {tensor_cast.getResult(), operands[0]});

    return success();
  }
};

class XTenToLinalgPass : public PassWrapper<XTenToLinalgPass,
                                           OperationPass<ModuleOp>> {

public:
  XTenToLinalgPass() {}

  void getDependentDialects(::mlir::DialectRegistry &registry) const override {
     registry.insert<NPCOMP::aten::ATenDialect>();
     registry.insert<memref::MemRefDialect>();
     registry.insert<linalg::LinalgDialect>();
  }

  void runOnOperation() override {

    auto module = getOperation();
    auto context = module.getContext();

    TypeConverter typeConverter;

    // tablegen patterns
    OwningRewritePatternList patterns(&getContext());
    //populateWithGenerated(context, patterns);

    patterns.insert<XTenMMOpConversion,
                    XTenAddOpConversion,
                    XTenMulOpConversion,
                    XTenConv2dOpConversion,
                    XTenPartialConv2dReLUOpConversion>(context);

    // populateFuncOpTypeConversionPattern(patterns,
    //                                     context,
    //                                     typeConverter);

    ConversionTarget target(*context);

    target.addLegalDialect<AffineDialect, linalg::LinalgDialect,
                           memref::MemRefDialect,
                           StandardOpsDialect, scf::SCFDialect>();

    target.addLegalOp<NPCOMP::aten::TypeCastOp>();

    target.addDynamicallyLegalOp<FuncOp>([&](FuncOp op) {
       return typeConverter.isSignatureLegal(op.getType());
    });

    if (failed(applyPartialConversion(module, target, std::move(patterns)))) {
      emitError(UnknownLoc::get(context), "error lowering AIR to Linalg\n");
      signalPassFailure();
      //assert(0);
    }

    module.walk([&](linalg::MatmulOp op) {
      op->setAttr(
        linalg::LinalgTransforms::kLinalgTransformMarker,
        StringAttr::get(op->getContext(), "ACDC_mmult"));
    });
    module.walk([&](linalg::ConvNCHWOp op) {
      op->setAttr(
        linalg::LinalgTransforms::kLinalgTransformMarker,
        StringAttr::get( op->getContext(), "ACDC_conv2d"));
    });
  }

private:

};

} // namespace

namespace xilinx {
namespace air {

std::unique_ptr<Pass> createXTenToLinalgPass() {
  return std::make_unique<XTenToLinalgPass>();
}

} // namespace air
} // namespace xilinx

void xilinx::air::registerXTenToLinalgPass() {
    PassRegistration<XTenToLinalgPass>(
      "xten-to-linalg",
      "Lower XTen dialect to Linalg dialect");
}