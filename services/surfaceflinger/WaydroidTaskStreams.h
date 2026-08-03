/*
 * Copyright (C) 2026 The Waydroid Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <android-base/thread_annotations.h>
#include <renderengine/ExternalTexture.h>
#include <ui/Fence.h>
#include <ui/GraphicBuffer.h>
#include <utils/Timers.h>
#include <vendor/waydroid/display/1.3/IWaydroidDisplay.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace android::surfaceflinger::frontend {
class LayerHierarchy;
} // namespace android::surfaceflinger::frontend

namespace android {

class SurfaceFlinger;

// Waydroid per-task content streams: after each present, re-render every
// task's layer subtree into that task's own buffer via the screenshot path,
// so each Wayland toplevel gets an independent content stream instead of a
// slice of the physical-display composition.
// Gated on persist.waydroid.task_streams. See PLAN-phase2-task-streams.md.
class WaydroidTaskStreams {
public:
    explicit WaydroidTaskStreams(SurfaceFlinger& flinger);
    ~WaydroidTaskStreams();

    static bool enabledByProp();

    // Main thread, after composite() has moved snapshots back into the
    // builder: group layers by task id and wake the render thread.
    void onCompositionPresented(const surfaceflinger::frontend::LayerHierarchy& hierarchy);

private:
    struct TaskCapture {
        int32_t taskId;
        std::unordered_set<uint32_t> layerIds;
    };

    // Three buffers per task: Mir holds current + previous, and a release
    // only arrives on the commit after that — with two slots no post ever
    // finds a free one after the first pair. Slot busy-ness follows the
    // HAL's wl_buffer.release reports.
    static constexpr uint32_t kSlotsPerTask = 3;

    struct TaskStream {
        struct Slot {
            std::shared_ptr<renderengine::ExternalTexture> texture;
            bool busy = false;
        };
        Slot slots[kSlotsPerTask];
        uint64_t lastSeenFrame = 0;
        uint32_t nextSlot = 0;
        int consecutiveStarved = 0;
        int consecutiveRefused = 0;
        uint64_t skipUntilFrame = 0;
        uint64_t geometrySig = 0;
        bool haveGeometrySig = false;
        int consecutiveUnstable = 0;
        nsecs_t renderTotalNs = 0;
        int renderedFrames = 0;
        int emptyFrames = 0;
        int starvedFrames = 0;
        int unstableFrames = 0;
        int postFailures = 0;
    };

    // What the HAL answered to updateTaskList: Active means mWanted is
    // authoritative, Inactive means task streams are off HAL-side (render
    // nothing), Unavailable means no answer (render everything, the
    // refused-post backoff still protects).
    enum class Gate { Unavailable, Active, Inactive };

    void threadMain();
    // Both return true when a task's frame was withheld by the geometry
    // stability gate and a follow-up flush pass is needed.
    bool renderTasks(const std::vector<TaskCapture>& tasks);
    bool renderTask(const TaskCapture& task, TaskStream& stream, bool dump);
    bool connectHal();
    Gate queryWantedTasks(const std::vector<TaskCapture>& tasks);
    void postBuffer(int32_t taskId, uint32_t slot, TaskStream& stream, const sp<Fence>& fence);
    void dumpBuffer(int32_t taskId, const sp<GraphicBuffer>& buffer);

    SurfaceFlinger& mFlinger;

    // Render thread only.
    std::unordered_map<int32_t, TaskStream> mStreams;
    uint64_t mFrame = 0;
    sp<vendor::waydroid::display::V1_3::IWaydroidDisplay> mHal;
    int mNoHalLogged = 0;
    std::unordered_set<int32_t> mWanted;
    Gate mGate = Gate::Unavailable;

    std::mutex mMutex;
    std::condition_variable mCondition;
    std::vector<TaskCapture> mPending GUARDED_BY(mMutex);
    std::vector<TaskCapture> mLastTasks GUARDED_BY(mMutex);
    bool mFlushPending GUARDED_BY(mMutex) = false;
    int mFlushRetries GUARDED_BY(mMutex) = 0;
    bool mFramePending GUARDED_BY(mMutex) = false;
    bool mRunning GUARDED_BY(mMutex) = true;
    std::thread mThread;
};

} // namespace android
