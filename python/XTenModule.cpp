// (c) Copyright 2021 Xilinx Inc. All Rights Reserved.

#include "xten/Transform/Passes.h"
#include "xten/Conversion/Passes.h"

#include <string>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace xten {
namespace {

void InitXTenModuleBindings(pybind11::module m)
{
  m.def("_register_all_passes", []() {
    xilinx::xten::registerTransformPasses();
    xilinx::xten::registerConversionPasses();
  }, "register all passes");

}

} // namespace

void InitXTenBindings(pybind11::module m) { InitXTenModuleBindings(m); }

}  // namespace xten

PYBIND11_MODULE(_xten, m) { xten::InitXTenBindings(m); }