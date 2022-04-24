// Copyright 2018 The Dawn Authors
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

#ifndef WEBNN_NATIVE_WEBNNPLATFORM_H_
#define WEBNN_NATIVE_WEBNNPLATFORM_H_

// Use webnn_cpp to have the enum and bitfield definitions
#include <webnn/webnn_cpp.h>

// Use our autogenerated version of the webnn structures that point to webnn_native
// object types
#include <webnn_native/webnn_platform_autogen.h>
#include <webnn_native/webnn_structs_autogen.h>

namespace webnn::native {
    // kEnumCount is a constant specifying the number of enums in a WebGPU enum type,
    // if the enums are contiguous, making it suitable for iteration.
    // It is defined in dawn_platform_autogen.h
    template <typename T>
    constexpr uint32_t kEnumCount = EnumCount<T>::value;
}  // namespace webnn::native

#endif  // WEBNN_NATIVE_WEBNNPLATFORM_H_
