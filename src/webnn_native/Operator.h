// Copyright 2021 The WebNN-native Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef WEBNN_NATIVE_OPERATOR_H_
#define WEBNN_NATIVE_OPERATOR_H_

#include "webnn_native/Forward.h"
#include "webnn_native/ObjectBase.h"

namespace webnn_native {

    enum class OperatorType : uint32_t {
        Clamp = 0x00000000,
        Relu = 0x00000001,
        Sigmoid = 0x00000002,
        LeakyRelu = 0x00000003,
    };

    class OperatorBase : public ObjectBase {
      public:
        explicit OperatorBase(GraphBuilderBase* GraphBuilder, OperatorType type);
        virtual ~OperatorBase() = default;

        OperatorType GetOperatorType() const {
            return mType;
        }

      private:
        OperatorBase(GraphBuilderBase* GraphBuilder, ObjectBase::ErrorTag tag);
        OperatorType mType;
    };
}  // namespace webnn_native

#endif  // WEBNN_NATIVE_OPERATOR_H_
