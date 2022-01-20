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

#include "webnn_native/Context.h"

#include <sstream>

#include "webnn_native/ValidationUtils_autogen.h"
#include "webnn_native/webnn_platform.h"

namespace webnn_native {

    ContextBase::ContextBase(ContextOptions const* options) {
        if (options != nullptr) {
            mContextOptions = *options;
        }
        mRootErrorScope = AcquireRef(new ErrorScope());
        mCurrentErrorScope = mRootErrorScope.Get();
    }

    GraphBase* ContextBase::CreateGraph() {
        return CreateGraphImpl();
    }

    void ContextBase::InjectError(ml::ErrorType type, const char* message) {
        if (ConsumedError(ValidateErrorType(type))) {
            return;
        }

        // This method should only be used to make error scope reject.
        if (type != ml::ErrorType::Validation && type != ml::ErrorType::OutOfMemory) {
            HandleError(
                DAWN_VALIDATION_ERROR("Invalid injected error, must be Validation or OutOfMemory"));
            return;
        }

        HandleError(DAWN_MAKE_ERROR(FromMLErrorType(type), message));
    }

    void ContextBase::PushErrorScope(ml::ErrorFilter filter) {
        if (ConsumedError(ValidateErrorFilter(filter))) {
            return;
        }
        mCurrentErrorScope = AcquireRef(new ErrorScope(filter, mCurrentErrorScope.Get()));
    }

    bool ContextBase::PopErrorScope(ml::ErrorCallback callback, void* userdata) {
        if (DAWN_UNLIKELY(mCurrentErrorScope.Get() == mRootErrorScope.Get())) {
            return false;
        }
        mCurrentErrorScope->SetCallback(callback, userdata);
        mCurrentErrorScope = Ref<ErrorScope>(mCurrentErrorScope->GetParent());

        return true;
    }

    void ContextBase::SetUncapturedErrorCallback(ml::ErrorCallback callback, void* userdata) {
        mRootErrorScope->SetCallback(callback, userdata);
    }

    void ContextBase::HandleError(InternalErrorType type, const char* message) {
    }

    void ContextBase::HandleError(std::unique_ptr<ErrorData> error) {
        ASSERT(error != nullptr);
        std::ostringstream ss;
        ss << error->GetMessage();
        for (const auto& callsite : error->GetBacktrace()) {
            ss << "\n    at " << callsite.function << " (" << callsite.file << ":" << callsite.line
               << ")";
        }

        // Still forward context loss and internal errors to the error scopes so they
        // all reject.
        mCurrentErrorScope->HandleError(ToMLErrorType(error->GetType()), ss.str().c_str());
    }

    MaybeError ContextBase::ValidateObject(const ApiObjectBase* object) const {
        ASSERT(object != nullptr);
        DAWN_INVALID_IF(object->GetContext() != this,
                        "Object is not associated with this context.");

        // TODO(dawn:563): Preserve labels for error objects.
        DAWN_INVALID_IF(object->IsError(), "Object is invalid.");

        return {};
    }

    MaybeError ContextBase::ValidateIsAlive() const {
        DAWN_INVALID_IF(mState != State::Alive, "Context is lost.");
        return {};
    }

    ContextBase::State ContextBase::GetState() const {
        return mState;
    }

    bool ContextBase::IsLost() const {
        ASSERT(mState != State::BeingCreated);
        return mState != State::Alive;
    }

    void ContextBase::TrackObject(ApiObjectBase* object) {
        ApiObjectList& objectList = mObjectLists[object->GetType()];
        std::lock_guard<std::mutex> lock(objectList.mutex);
        object->InsertBefore(objectList.objects.head());
    }

    std::mutex* ContextBase::GetObjectListMutex(ObjectType type) {
        return &mObjectLists[type].mutex;
    }

    void ContextBase::APILoseForTesting() {
        if (mState != State::Alive) {
            return;
        }

        HandleError(InternalErrorType::Internal, "Context lost for testing");
    }

    const std::string& ContextBase::GetLabel() const {
        return mLabel;
    }

    void ContextBase::APISetLabel(const char* label) {
        mLabel = label;
        SetLabelImpl();
    }

    void ContextBase::SetLabelImpl() {
    }

    void ContextBase::APIDestroy() {
    }

    void ContextBase::DestroyObjects() {
    }

    void ContextBase::Destroy() {
    }

    ContextBase::ContextBase() : mState(State::Alive) {
    }

}  // namespace webnn_native
