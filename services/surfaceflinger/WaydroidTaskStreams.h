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
#include <utils/Timers.h>

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

    struct TaskStream {
        std::shared_ptr<renderengine::ExternalTexture> texture;
        nsecs_t renderTotalNs = 0;
        int renderedFrames = 0;
        int emptyFrames = 0;
    };

    void threadMain();
    void renderTasks(const std::vector<TaskCapture>& tasks);
    void renderTask(const TaskCapture& task, TaskStream& stream, bool dump);
    void dumpStream(int32_t taskId, const TaskStream& stream);

    SurfaceFlinger& mFlinger;

    // Render thread only.
    std::unordered_map<int32_t, TaskStream> mStreams;

    std::mutex mMutex;
    std::condition_variable mCondition;
    std::vector<TaskCapture> mPending GUARDED_BY(mMutex);
    bool mFramePending GUARDED_BY(mMutex) = false;
    bool mRunning GUARDED_BY(mMutex) = true;
    std::thread mThread;
};

} // namespace android
