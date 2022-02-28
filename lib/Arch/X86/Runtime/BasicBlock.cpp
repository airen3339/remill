/*
 * Copyright (c) 2017 Trail of Bits, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <algorithm>
#include <bitset>
#include <cmath>

#include "remill/Arch/Runtime/Float.h"
#include "remill/Arch/X86/Runtime/State.h"

extern "C" {

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-variable"

// Debug registers.
extern uint64_t DR0;
extern uint64_t DR1;
extern uint64_t DR2;
extern uint64_t DR3;
extern uint64_t DR4;
extern uint64_t DR5;
extern uint64_t DR6;
extern uint64_t DR7;

// Control regs.
extern CR0Reg CR0;
extern CR1Reg CR1;
extern CR2Reg CR2;
extern CR3Reg CR3;
extern CR4Reg CR4;
extern CR8Reg CR8;

#pragma clang diagnostic pop

}  // extern C
