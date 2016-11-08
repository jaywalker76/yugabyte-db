// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "yb/common/timestamp.h"

#include "yb/util/faststring.h"
#include "yb/util/memcmpable_varint.h"
#include "yb/util/status.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/gutil/mathlimits.h"

using strings::Substitute;

namespace yb {

const Timestamp Timestamp::kMin(MathLimits<Timestamp::val_type>::kMin);
const Timestamp Timestamp::kMax(MathLimits<Timestamp::val_type>::kMax);
const Timestamp Timestamp::kInitialTimestamp(MathLimits<Timestamp::val_type>::kMin + 1);
const Timestamp Timestamp::kInvalidTimestamp(MathLimits<Timestamp::val_type>::kMax - 1);

bool Timestamp::DecodeFrom(Slice *input) {
  return GetMemcmpableVarint64(input, &v);
}

void Timestamp::EncodeTo(faststring *dst) const {
  PutMemcmpableVarint64(dst, v);
}

string Timestamp::ToString() const {
  return strings::Substitute("$0", v);
}

string Timestamp::ToDebugString() const {
  return Substitute("$0($1)", kTimestampDebugStrPrefix,
                    v == kMax.v ? "Max" : ToString());
}

uint64_t Timestamp::ToUint64() const {
  return v;
}

Status Timestamp::FromUint64(uint64_t value) {
  v = value;
  return Status::OK();
}

const char* const Timestamp::kTimestampDebugStrPrefix = "TS";

}  // namespace yb
