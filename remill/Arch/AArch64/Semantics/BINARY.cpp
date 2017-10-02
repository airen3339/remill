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

namespace {

template <typename D, typename S1, typename S2>
DEF_SEM(SUB, D dst, S1 src1, S2 src2) {
  WriteZExt(dst, USub(Read(src1), Read(src2)));
  return memory;
}

template <typename D, typename S1, typename S2>
DEF_SEM(ADD, D dst, S1 src1, S2 src2) {
  WriteZExt(dst, UAdd(Read(src1), Read(src2)));
  return memory;
}

}  // namespace

DEF_ISEL(ADD_32_ADDSUB_IMM) = ADD<R32W, R32, I32>;
DEF_ISEL(ADD_64_ADDSUB_IMM) = ADD<R64W, R64, I64>;
DEF_ISEL(ADD_32_ADDSUB_SHIFT) = ADD<R32W, R32, I32>;
DEF_ISEL(ADD_64_ADDSUB_SHIFT) = ADD<R64W, R64, I64>;
DEF_ISEL(ADD_32_ADDSUB_EXT) = ADD<R32W, R32, I32>;
DEF_ISEL(ADD_64_ADDSUB_EXT) = ADD<R64W, R64, I64>;

DEF_ISEL(SUB_32_ADDSUB_IMM) = SUB<R32W, R32, I32>;
DEF_ISEL(SUB_64_ADDSUB_IMM) = SUB<R64W, R64, I64>;
DEF_ISEL(SUB_32_ADDSUB_SHIFT) = SUB<R32W, R32, I32>;
DEF_ISEL(SUB_64_ADDSUB_SHIFT) = SUB<R64W, R64, I64>;
DEF_ISEL(SUB_32_ADDSUB_EXT) = SUB<R32W, R32, I32>;
DEF_ISEL(SUB_64_ADDSUB_EXT) = SUB<R64W, R64, I64>;

namespace {

template <typename T>
T AddWithCarryNZCV(State &state, T lhs, T rhs, T carry) {
  auto unsigned_result = UAdd(UAdd(ZExt(lhs), ZExt(rhs)), ZExt(carry));
  auto signed_result = SAdd(SAdd(SExt(lhs), SExt(rhs)), Signed(ZExt(carry)));
  auto result = TruncTo<T>(unsigned_result);
  FLAG_N = SignFlag(result);
  FLAG_Z = ZeroFlag(result);
  FLAG_C = UCmpNeq(ZExt(result), unsigned_result);
  FLAG_V = SCmpNeq(SExt(result), signed_result);
  return result;
}

template <typename D, typename S1, typename S2>
DEF_SEM(SUBS, D dst, S1 src1, S2 src2) {
  using T = typename BaseType<S2>::BT;
  auto lhs = Read(src1);
  auto rhs = Read(src2);
  auto res = AddWithCarryNZCV(state, lhs, UNot(rhs), T(1));
  WriteZExt(dst, res);
  return memory;
}

template <typename D, typename S1, typename S2>
DEF_SEM(ADDS, D dst, S1 src1, S2 src2) {
  using T = typename BaseType<S2>::BT;
  auto lhs = Read(src1);
  auto rhs = Read(src2);
  auto res = AddWithCarryNZCV(state, lhs, rhs, T(0));
  WriteZExt(dst, res);
  return memory;
}
}  // namespace

DEF_ISEL(SUBS_32_ADDSUB_SHIFT) = SUBS<R32W, R32, I32>;
DEF_ISEL(SUBS_64_ADDSUB_SHIFT) = SUBS<R64W, R64, I64>;
DEF_ISEL(SUBS_32S_ADDSUB_IMM) = SUBS<R32W, R32, I32>;
DEF_ISEL(SUBS_64S_ADDSUB_IMM) = SUBS<R64W, R64, I64>;
DEF_ISEL(SUBS_32S_ADDSUB_EXT) = SUBS<R32W, R32, I32>;
DEF_ISEL(SUBS_64S_ADDSUB_EXT) = SUBS<R64W, R64, I64>;

DEF_ISEL(ADDS_32_ADDSUB_SHIFT) = ADDS<R32W, R32, I32>;
DEF_ISEL(ADDS_64_ADDSUB_SHIFT) = ADDS<R64W, R64, I64>;
DEF_ISEL(ADDS_32S_ADDSUB_IMM) = ADDS<R32W, R32, I32>;
DEF_ISEL(ADDS_64S_ADDSUB_IMM) = ADDS<R64W, R64, I64>;
DEF_ISEL(ADDS_32S_ADDSUB_EXT) = ADDS<R32W, R32, I32>;
DEF_ISEL(ADDS_64S_ADDSUB_EXT) = ADDS<R64W, R64, I64>;

namespace {

DEF_SEM(UMADDL, R64W dst, R32 src1, R32 src2, R64 src3) {
  Write(dst, UAdd(Read(src3), UMul(ZExt(Read(src1)), ZExt(Read(src2)))));
  return memory;
}

DEF_SEM(SMADDL, R64W dst, R32 src1, R32 src2, R64 src3) {
  auto operand1 = SExt(Signed(Read(src1)));
  auto operand2 = SExt(Signed(Read(src2)));
  auto operand3 = Signed(Read(src3));
  Write(dst, Unsigned(SAdd(operand3, SMul(operand1, operand2))));
  return memory;
}

DEF_SEM(UMULH, R64W dst, R64 src1, R64 src2) {
  uint128_t lhs = ZExt(Read(src1));
  uint128_t rhs = ZExt(Read(src2));
  uint128_t res = UMul(lhs, rhs);
  Write(dst, Trunc(UShr(res, 64)));
  return memory;
}

DEF_SEM(SMULH, R64W dst, R64 src1, R64 src2) {
  int128_t lhs = SExt(Signed(Read(src1)));
  int128_t rhs = SExt(Signed(Read(src2)));
  uint128_t res = Unsigned(SMul(lhs, rhs));
  Write(dst, Trunc(UShr(res, 64)));
  return memory;
}

template <typename D, typename S>
DEF_SEM(UDIV, D dst, S src1, S src2) {
  using T = typename BaseType<S>::BT;
  auto lhs = Read(src1);
  auto rhs = Read(src2);
  if (!rhs) {
    WriteZExt(dst, T(0));
  } else {
    WriteZExt(dst, UDiv(lhs, rhs));
  }
  return memory;
}

template <typename D, typename S>
DEF_SEM(MADD, D dst, S src1, S src2, S src3) {
  WriteZExt(dst, UAdd(Read(src3), UMul(Read(src1), Read(src2))));
  return memory;
}

template <typename D, typename S>
DEF_SEM(MSUB, D dst, S src1, S src2, S src3) {
  WriteZExt(dst, USub(Read(src3), UMul(Read(src1), Read(src2))));
  return memory;
}

}  // namespace

DEF_ISEL(UMADDL_64WA_DP_3SRC) = UMADDL;
DEF_ISEL(SMADDL_64WA_DP_3SRC) = SMADDL;

DEF_ISEL(UMULH_64_DP_3SRC) = UMULH;
DEF_ISEL(SMULH_64_DP_3SRC) = SMULH;

DEF_ISEL(UDIV_32_DP_2SRC) = UDIV<R32W, R32>;
DEF_ISEL(UDIV_64_DP_2SRC) = UDIV<R64W, R64>;

DEF_ISEL(MADD_32A_DP_3SRC) = MADD<R32W, R32>;
DEF_ISEL(MADD_64A_DP_3SRC) = MADD<R64W, R64>;

DEF_ISEL(MSUB_32A_DP_3SRC) = MSUB<R32W, R32>;
DEF_ISEL(MSUB_64A_DP_3SRC) = MSUB<R64W, R64>;

namespace {

template <typename D, typename S>
DEF_SEM(SBC, D dst, S src1, S src2) {
  auto carry = ZExtTo<S>(Unsigned(FLAG_C));
  WriteZExt(dst, UAdd(UAdd(Read(src1), UNot(Read(src2))), carry));
  return memory;
}

template <typename D, typename S>
DEF_SEM(SBCS, D dst, S src1, S src2) {
  auto carry = ZExtTo<S>(Unsigned(FLAG_C));
  auto res = AddWithCarryNZCV(state, Read(src1), UNot(Read(src2)), carry);
  WriteZExt(dst, res);
  return memory;
}

}  // namespace

DEF_ISEL(SBC_32_ADDSUB_CARRY) = SBC<R32W, R32>;
DEF_ISEL(SBC_64_ADDSUB_CARRY) = SBC<R64W, R64>;

DEF_ISEL(SBCS_32_ADDSUB_CARRY) = SBCS<R32W, R32>;
DEF_ISEL(SBCS_64_ADDSUB_CARRY) = SBCS<R64W, R64>;
