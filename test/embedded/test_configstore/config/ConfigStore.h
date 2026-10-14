#pragma once
// Test shim: lets the embedded test compile src/config/ConfigStore.cpp without
// test_build_src (which would also link src/main.cpp and its setup()/loop()).
#include "../../../../src/config/ConfigStore.h"
