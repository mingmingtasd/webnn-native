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

#ifndef WEBNN_NATIVE_CONTEXT_H_
#define WEBNN_NATIVE_CONTEXT_H_

#include "common/RefCounted.h"
#include "webnn_native/Error.h"
#include "webnn_native/ErrorScope.h"
#include "webnn_native/ObjectBase.h"
#include "webnn_native/ObjectType_autogen.h"
#include "webnn_native/webnn_platform.h"

#include <mutex>

namespace webnn_native {

    class ContextBase : public RefCounted {
      public:
        explicit ContextBase(ContextOptions const* options = nullptr);
        virtual ~ContextBase() = default;

        void HandleError(InternalErrorType type, const char* message);

        bool ConsumedError(MaybeError maybeError) {
            if (DAWN_UNLIKELY(maybeError.IsError())) {
                HandleError(maybeError.AcquireError());
                return true;
            }
            return false;
        }

        GraphBase* CreateGraph();

        // Dawn API
        void PushErrorScope(ml::ErrorFilter filter);
        bool PopErrorScope(ml::ErrorCallback callback, void* userdata);
        void SetUncapturedErrorCallback(ml::ErrorCallback callback, void* userdata);
        ContextOptions GetContextOptions() {
            return mContextOptions;
        }

        MaybeError ValidateObject(const ApiObjectBase* object) const;
        MaybeError ValidateIsAlive() const;
        // The context state which is a combination of creation state and loss state.
        //
        //   - BeingCreated: the context didn't finish creation yet and the frontend cannot be used
        //     (both for the application calling WebGPU, or re-entrant calls). No work exists on
        //     the GPU timeline.
        //   - Alive: the context is usable and might have work happening on the GPU timeline.
        //   - BeingDisconnected: the context is no longer usable because we are waiting for all
        //     work on the GPU timeline to finish. (this is to make validation prevent the
        //     application from adding more work during the transition from Available to
        //     Disconnected)
        //   - Disconnected: there is no longer work happening on the GPU timeline and the CPU data
        //     structures can be safely destroyed without additional synchronization.
        //   - Destroyed: the context is disconnected and resources have been reclaimed.
        enum class State {
            BeingCreated,
            Alive,
            BeingDisconnected,
            Disconnected,
            Destroyed,
        };
        State GetState() const;
        bool IsLost() const;
        void TrackObject(ApiObjectBase* object);
        std::mutex* GetObjectListMutex(ObjectType type);
        void APILoseForTesting();

        const std::string& GetLabel() const;
        void APISetLabel(const char* label);
        void APIDestroy();

      protected:
        // Constructor used only for mocking and testing.
        ContextBase();
        void DestroyObjects();
        void Destroy();

      private:
        // Create concrete model.
        virtual GraphBase* CreateGraphImpl() = 0;

        void HandleError(std::unique_ptr<ErrorData> error);

        // DestroyImpl is used to clean up and release resources used by device, does not wait for
        // GPU or check errors.
        virtual void DestroyImpl() = 0;

        virtual void SetLabelImpl();
        std::string mLabel;

        Ref<ErrorScope> mRootErrorScope;
        Ref<ErrorScope> mCurrentErrorScope;

        ContextOptions mContextOptions;

        State mState = State::BeingCreated;

        // Encompasses the mutex and the actual list that contains all live objects "owned" by the
        // device.
        struct ApiObjectList {
            std::mutex mutex;
            LinkedList<ApiObjectBase> objects;
        };
        PerObjectType<ApiObjectList> mObjectLists;
    };

}  // namespace webnn_native

#endif  // WEBNN_NATIVE_CONTEXT_H_
