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

#undef LOG_TAG
#define LOG_TAG "WaydroidTaskStreams"

#include "WaydroidTaskStreams.h"

#include <android-base/properties.h>
#include <cutils/properties.h>
#include <gui/LayerMetadata.h>
#include <renderengine/impl/ExternalTexture.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "FrontEnd/LayerHierarchy.h"
#include "Layer.h"
#include "SurfaceFlinger.h"

namespace android {

namespace {
constexpr int kLogEveryFrames = 300;
constexpr char kDumpProp[] = "waydroid.task_streams.dump";
constexpr char kDumpDir[] = "/data/task_streams";
} // namespace

WaydroidTaskStreams::WaydroidTaskStreams(SurfaceFlinger& flinger) : mFlinger(flinger) {
    mThread = std::thread([this]() { threadMain(); });
    pthread_setname_np(mThread.native_handle(), "WaydroidTaskStr");
    ALOGI("per-task content streams enabled");
}

WaydroidTaskStreams::~WaydroidTaskStreams() {
    {
        std::lock_guard lock(mMutex);
        mRunning = false;
        mCondition.notify_one();
    }
    if (mThread.joinable()) {
        mThread.join();
    }
}

bool WaydroidTaskStreams::enabledByProp() {
    return base::GetBoolProperty(std::string("persist.waydroid.task_streams"), false);
}

void WaydroidTaskStreams::onCompositionPresented(const frontend::LayerHierarchy& hierarchy) {
    std::vector<TaskCapture> tasks;
    std::unordered_set<int32_t> seenTasks;

    // Find the topmost layer of each task. Unlike FpsReporter we do not
    // descend into a task's subtree looking for more roots: nested (organized)
    // tasks belong to their root task's window.
    std::vector<const frontend::LayerHierarchy*> roots;
    hierarchy.traverse([&](const frontend::LayerHierarchy& node,
                           const frontend::LayerHierarchy::TraversalPath& traversalPath) {
        if (traversalPath.variant == frontend::LayerHierarchy::Variant::Detached) {
            return false;
        }
        const auto& metadata = node.getLayer()->metadata;
        if (metadata.has(gui::METADATA_TASK_ID)) {
            const int32_t taskId = metadata.getInt32(gui::METADATA_TASK_ID, 0);
            if (seenTasks.insert(taskId).second) {
                tasks.push_back({taskId, {}});
                roots.push_back(&node);
            }
            return false;
        }
        return true;
    });

    for (size_t i = 0; i < roots.size(); i++) {
        auto& layerIds = tasks[i].layerIds;
        roots[i]->traverse([&](const frontend::LayerHierarchy& node,
                               const frontend::LayerHierarchy::TraversalPath& traversalPath) {
            if (traversalPath.variant == frontend::LayerHierarchy::Variant::Detached) {
                return false;
            }
            layerIds.insert(node.getLayer()->id);
            return true;
        });
    }

    {
        std::lock_guard lock(mMutex);
        mPending = std::move(tasks);
        mFramePending = true;
    }
    mCondition.notify_one();
}

// NO_THREAD_SAFETY_ANALYSIS: std::unique_lock lacks thread safety annotations.
void WaydroidTaskStreams::threadMain() NO_THREAD_SAFETY_ANALYSIS {
    std::unique_lock<std::mutex> lock(mMutex);
    while (mRunning) {
        if (mFramePending) {
            mFramePending = false;
            std::vector<TaskCapture> tasks = std::move(mPending);
            mPending.clear();
            lock.unlock();
            renderTasks(tasks);
            lock.lock();
        }
        mCondition.wait(lock,
                        [this]() REQUIRES(mMutex) { return mFramePending || !mRunning; });
    }
}

void WaydroidTaskStreams::renderTasks(const std::vector<TaskCapture>& tasks) {
    const bool dump = base::GetBoolProperty(std::string(kDumpProp), false);

    // Drop streams of tasks that no longer exist.
    for (auto it = mStreams.begin(); it != mStreams.end();) {
        const int32_t taskId = it->first;
        const bool alive = std::any_of(tasks.begin(), tasks.end(),
                                       [taskId](const auto& t) { return t.taskId == taskId; });
        if (!alive) {
            ALOGI("task %d gone, dropping its stream", taskId);
            it = mStreams.erase(it);
        } else {
            ++it;
        }
    }

    for (const auto& task : tasks) {
        renderTask(task, mStreams[task.taskId], dump);
    }

    if (dump) {
        property_set(kDumpProp, "0");
    }
}

void WaydroidTaskStreams::renderTask(const TaskCapture& task, TaskStream& stream, bool dump) {
    // Pick a slot the host compositor is not holding.
    uint32_t slot = kSlotsPerTask;
    for (uint32_t i = 0; i < kSlotsPerTask; i++) {
        if (!stream.slots[i].busy) {
            slot = i;
            break;
        }
    }
    if (slot == kSlotsPerTask) {
        stream.starvedFrames++;
        return;
    }

    auto filterFn = [&task](const frontend::LayerSnapshot& snapshot,
                            bool& /*outStopTraversal*/) -> bool {
        return task.layerIds.count(snapshot.path.id) != 0;
    };

    SurfaceFlinger::ScreenshotArgs args{.captureTypeVariant = std::monostate{},
                                        .displayIdVariant = std::nullopt,
                                        .snapshotRequest =
                                                SurfaceFlinger::SnapshotRequestArgs{
                                                        .uid = gui::Uid::INVALID,
                                                        .snapshotFilterFn = filterFn},
                                        .dataspace = ui::Dataspace::V0_SRGB,
                                        .disableBlur = true,
                                        .isGrayscale = false,
                                        .isSecure = true,
                                        .preserveDisplayColors = false,
                                        .debugName = "WaydroidTaskStream"};

    const nsecs_t start = systemTime();
    auto result = mFlinger.setScreenshotSnapshotsAndDisplayState(args, ui::PixelFormat::RGBA_8888);
    if (!result.ok()) {
        ALOGW("task %d: snapshot collection failed (%d)", task.taskId, result.error());
        return;
    }
    if (args.layers.empty()) {
        stream.emptyFrames++;
        return;
    }

    auto& texture = stream.slots[slot].texture;
    if (!texture ||
        texture->getBuffer()->getWidth() != static_cast<uint32_t>(args.size.getWidth()) ||
        texture->getBuffer()->getHeight() != static_cast<uint32_t>(args.size.getHeight())) {
        const uint32_t usage = GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_HW_RENDER |
                GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_HW_COMPOSER;
        sp<GraphicBuffer> buffer =
                sp<GraphicBuffer>::make(static_cast<uint32_t>(args.size.getWidth()),
                                        static_cast<uint32_t>(args.size.getHeight()),
                                        PIXEL_FORMAT_RGBA_8888, 1u, usage, "WaydroidTaskStream");
        if (buffer->initCheck() != OK) {
            ALOGE("task %d: buffer allocation %dx%d failed (%d)", task.taskId,
                  args.size.getWidth(), args.size.getHeight(), buffer->initCheck());
            return;
        }
        texture = std::make_shared<
                renderengine::impl::ExternalTexture>(buffer, mFlinger.getRenderEngine(),
                                                     renderengine::impl::ExternalTexture::Usage::
                                                             WRITEABLE);
        ALOGI("task %d: allocated %dx%d stream buffer (slot %u)", task.taskId,
              args.size.getWidth(), args.size.getHeight(), slot);
    }

    FenceResult fenceResult = mFlinger.captureScreenshot(args, texture, nullptr).get();
    if (!fenceResult.ok()) {
        ALOGW("task %d: render failed (%d)", task.taskId, fenceResult.error());
        return;
    }

    postBuffer(task.taskId, slot, stream, fenceResult.value());

    stream.renderTotalNs += systemTime() - start;
    if (++stream.renderedFrames % kLogEveryFrames == 0) {
        ALOGI("task %d: %d frames rendered, avg %.2f ms, %d empty, %d starved, %d post failures,"
              " %zu layers last frame",
              task.taskId, stream.renderedFrames,
              stream.renderTotalNs / 1e6 / stream.renderedFrames, stream.emptyFrames,
              stream.starvedFrames, stream.postFailures, args.layers.size());
    }

    if (dump) {
        fenceResult.value()->waitForever(LOG_TAG);
        dumpBuffer(task.taskId, texture->getBuffer());
    }
}

void WaydroidTaskStreams::postBuffer(int32_t taskId, uint32_t slot, TaskStream& stream,
                                     const sp<Fence>& fence) {
    using ::android::hardware::hidl_handle;
    using ::android::hardware::hidl_vec;
    using ::android::hardware::graphics::composer::V2_1::Error;
    using ::vendor::waydroid::display::V1_3::IWaydroidDisplay;

    if (!mHal) {
        mHal = IWaydroidDisplay::tryGetService();
        if (!mHal) {
            if (++mNoHalLogged % kLogEveryFrames == 1) {
                ALOGW("display@1.3 HAL not up yet, dropping posts");
            }
            return;
        }
        ALOGI("connected to display@1.3");
    }

    const sp<GraphicBuffer>& buffer = stream.slots[slot].texture->getBuffer();
    hidl_handle bufferHandle;
    bufferHandle.setTo(const_cast<native_handle_t*>(buffer->handle), false /*shouldOwn*/);

    hidl_handle fenceHandle;
    if (fence && fence->isValid()) {
        const int fd = fence->dup();
        if (fd >= 0) {
            native_handle_t* h = native_handle_create(1, 0);
            h->data[0] = fd;
            fenceHandle.setTo(h, true /*shouldOwn*/);
        }
    }

    Error error = Error::NO_RESOURCES;
    auto ret = mHal->postTaskBuffer(static_cast<uint32_t>(taskId), slot, bufferHandle,
                                    buffer->getWidth(), buffer->getHeight(),
                                    buffer->getStride(),
                                    static_cast<int32_t>(buffer->getPixelFormat()), fenceHandle,
                                    [&](Error e, const hidl_vec<uint32_t>& releasedSlots) {
                                        error = e;
                                        for (uint32_t released : releasedSlots) {
                                            if (released < kSlotsPerTask) {
                                                stream.slots[released].busy = false;
                                            }
                                        }
                                    });
    if (!ret.isOk()) {
        ALOGW("display@1.3 died (%s), reconnecting", ret.description().c_str());
        mHal = nullptr;
        return;
    }
    if (error == Error::NONE) {
        stream.slots[slot].busy = true;
    } else {
        // BAD_DISPLAY: task streams inactive HAL-side; BAD_LAYER: task not in
        // the HAL's table (yet). Both retry naturally on the next frame.
        if (++stream.postFailures % kLogEveryFrames == 1) {
            ALOGW("task %d: post rejected (%d)", taskId, static_cast<int32_t>(error));
        }
    }
}

void WaydroidTaskStreams::dumpBuffer(int32_t taskId, const sp<GraphicBuffer>& buffer) {
    if (!buffer) return;

    void* data = nullptr;
    if (buffer->lock(GRALLOC_USAGE_SW_READ_OFTEN, &data) != OK || data == nullptr) {
        ALOGW("task %d: dump lock failed", taskId);
        return;
    }

    mkdir(kDumpDir, 0777);
    char path[128];
    // Stride in pixels; raw RGBA rows are stride wide.
    snprintf(path, sizeof(path), "%s/task-%d-%ux%u-s%u.raw", kDumpDir, taskId, buffer->getWidth(),
             buffer->getHeight(), buffer->getStride());
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        const size_t size = size_t{buffer->getStride()} * buffer->getHeight() * 4;
        const ssize_t written = write(fd, data, size);
        close(fd);
        ALOGI("task %d: dumped %zd bytes to %s", taskId, written, path);
    } else {
        ALOGW("task %d: cannot open %s (%s)", taskId, path, strerror(errno));
    }
    buffer->unlock();
}

} // namespace android
