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
#include <inttypes.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "FrontEnd/LayerHierarchy.h"
#include "FrontEnd/LayerSnapshot.h"
#include "Layer.h"
#include "LayerFE.h"
#include "SurfaceFlinger.h"

namespace android {

namespace {
constexpr int kLogEveryFrames = 300;
constexpr char kDumpProp[] = "waydroid.task_streams.dump";
constexpr char kDumpDir[] = "/data/task_streams";
// SF cannot tell a backgrounded task from a removed one (both just leave the
// visible hierarchy), so streams are kept and only evicted beyond this cap.
constexpr size_t kMaxStreams = 12;
// All slots stuck busy this long means the HAL lost its slot state
// (reconnect); reset our flags and let posts re-settle.
constexpr int kStarvedResetFrames = 120;
// Consecutive refused posts before we stop rendering a task, and for how long.
constexpr int kRefusedThreshold = 3;
constexpr uint64_t kRefusedBackoffFrames = 300;
// A backgrounded task's card keeps the LAST posted frame, so never post
// mid-animation frames: wait until the task's layer geometry is unchanged
// between frames. The cap is a last resort for an app animating a window
// transform forever — it must be far longer than any transition, because a
// cap-forced post during a task switch is how zoomed mid-transition frames
// ended up frozen into cards (the idle flush handles liveness otherwise).
constexpr int kMaxUnstableFrames = 1200;
// Idle flush passes before giving up on a task whose frames stay withheld.
constexpr int kMaxFlushRetries = 40;
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
            const bool withheld = renderTasks(tasks);
            lock.lock();
            mLastTasks = std::move(tasks);
            mFlushPending = withheld;
            mFlushRetries = 0;
        }
        if (mFlushPending) {
            // A task's latest state did not reach its card (stability gate,
            // or its snapshots came back empty mid-transition). If SF goes
            // idle now (launch animation ended on a static screen), no
            // present will ever deliver the settled frame — re-render until
            // it posts, bounded so tasks that stay empty (backgrounded)
            // don't spin forever.
            if (!mCondition.wait_for(lock, std::chrono::milliseconds(150), [this]() REQUIRES(
                                             mMutex) { return mFramePending || !mRunning; })) {
                mFlushPending = false;
                std::vector<TaskCapture> tasks = mLastTasks;
                lock.unlock();
                const bool withheld = renderTasks(tasks);
                lock.lock();
                mFlushPending = withheld && ++mFlushRetries < kMaxFlushRetries;
            }
            continue;
        }
        mCondition.wait(lock,
                        [this]() REQUIRES(mMutex) { return mFramePending || !mRunning; });
    }
}

bool WaydroidTaskStreams::renderTasks(const std::vector<TaskCapture>& tasks) {
    const bool dump = base::GetBoolProperty(std::string(kDumpProp), false);

    mFrame++;

    // A task absent from this frame is usually backgrounded, not gone. Its
    // stream must survive: the compositor still displays one slot's buffer,
    // and freeing that GraphicBuffer lets gralloc recycle the dmabuf under
    // the card (wrong-app content). Free only slots the compositor released.
    for (auto& [taskId, stream] : mStreams) {
        const bool visible = std::any_of(tasks.begin(), tasks.end(),
                                         [taskId = taskId](const auto& t) {
                                             return t.taskId == taskId;
                                         });
        if (visible) {
            stream.lastSeenFrame = mFrame;
            continue;
        }
        for (auto& slot : stream.slots) {
            if (!slot.busy && slot.texture) {
                slot.texture = nullptr;
            }
        }
    }

    // Bound the map: evict the longest-absent stream (its card is most
    // likely gone; if not, the card may show recycled content until refocus).
    while (mStreams.size() > kMaxStreams) {
        auto oldest = mStreams.end();
        for (auto it = mStreams.begin(); it != mStreams.end(); ++it) {
            if (it->second.lastSeenFrame == mFrame) continue;
            if (oldest == mStreams.end() ||
                it->second.lastSeenFrame < oldest->second.lastSeenFrame) {
                oldest = it;
            }
        }
        if (oldest == mStreams.end()) break;
        ALOGI("task %d: evicting stream (absent %" PRIu64 " frames)", oldest->first,
              mFrame - oldest->second.lastSeenFrame);
        mStreams.erase(oldest);
    }

    bool withheld = false;
    for (const auto& task : tasks) {
        withheld |= renderTask(task, mStreams[task.taskId], dump);
    }

    if (dump) {
        property_set(kDumpProp, "0");
    }
    return withheld;
}

bool WaydroidTaskStreams::renderTask(const TaskCapture& task, TaskStream& stream, bool dump) {
    // Backing off after the HAL refused this task (launcher, system tasks).
    if (mFrame < stream.skipUntilFrame) {
        return false;
    }

    // Pick a slot the host compositor is not holding. Round-robin, so a slot
    // the HAL keeps rejecting does not get hammered forever.
    uint32_t slot = kSlotsPerTask;
    for (uint32_t i = 0; i < kSlotsPerTask; i++) {
        const uint32_t cand = (stream.nextSlot + i) % kSlotsPerTask;
        if (!stream.slots[cand].busy) {
            slot = cand;
            break;
        }
    }
    if (slot == kSlotsPerTask) {
        stream.starvedFrames++;
        if (++stream.consecutiveStarved >= kStarvedResetFrames) {
            ALOGW("task %d: all slots busy for %d frames, resetting slot state", task.taskId,
                  stream.consecutiveStarved);
            for (auto& s : stream.slots) s.busy = false;
            stream.consecutiveStarved = 0;
        }
        return false;
    }
    stream.consecutiveStarved = 0;
    stream.nextSlot = (slot + 1) % kSlotsPerTask;

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
        return false;
    }
    if (args.layers.empty()) {
        stream.emptyFrames++;
        // A stream that has posted before may be empty only transiently
        // (starting-window swap, transition churn) — worth a flush retry so
        // a launch that ends on empty frames doesn't freeze mid-animation.
        return stream.renderedFrames > 0;
    }

    // Geometry signature: layer set + global transforms + alphas. During
    // window transitions (launch, task switch, spread) these change every
    // frame; freezing such a frame into the card is what produced gray and
    // half-drawn cards. Post only frames whose geometry matches the previous
    // frame's.
    uint64_t sig = 14695981039346656037ull;
    const auto mix = [&sig](uint64_t v) {
        sig ^= v;
        sig *= 1099511628211ull;
    };
    for (const auto& [layer, layerFE] : args.layers) {
        const auto* snapshot = layerFE->mSnapshot.get();
        if (!snapshot) continue;
        mix(snapshot->path.id);
        const ui::Transform& t = snapshot->geomLayerTransform;
        for (const float f : {t.dsdx(), t.dtdx(), t.dtdy(), t.dsdy(), t.tx(), t.ty(),
                              snapshot->alpha}) {
            uint32_t bits;
            memcpy(&bits, &f, sizeof(bits));
            mix(bits);
        }
    }
    const bool stable = stream.haveGeometrySig && sig == stream.geometrySig;
    stream.geometrySig = sig;
    stream.haveGeometrySig = true;
    if (!stable && stream.consecutiveUnstable < kMaxUnstableFrames) {
        stream.unstableFrames++;
        stream.consecutiveUnstable++;
        return true;
    }
    stream.consecutiveUnstable = 0;

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
            return false;
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
        return false;
    }

    postBuffer(task.taskId, slot, stream, fenceResult.value());

    stream.renderTotalNs += systemTime() - start;
    if (++stream.renderedFrames % kLogEveryFrames == 0) {
        ALOGI("task %d: %d frames rendered, avg %.2f ms, %d empty, %d starved, %d unstable,"
              " %d post failures, %zu layers last frame",
              task.taskId, stream.renderedFrames,
              stream.renderTotalNs / 1e6 / stream.renderedFrames, stream.emptyFrames,
              stream.starvedFrames, stream.unstableFrames, stream.postFailures,
              args.layers.size());
    }

    if (dump) {
        fenceResult.value()->waitForever(LOG_TAG);
        dumpBuffer(task.taskId, texture->getBuffer());
    }
    return false;
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
        stream.consecutiveRefused = 0;
    } else {
        if (++stream.postFailures % kLogEveryFrames == 1) {
            ALOGW("task %d: post rejected (%d)", taskId, static_cast<int32_t>(error));
        }
        // BAD_DISPLAY: task streams inactive HAL-side; BAD_LAYER: task refused
        // (blacklist/no identity) or not in the HAL's table yet. A task that
        // keeps getting refused is not worth rendering every frame.
        if (error == Error::BAD_LAYER || error == Error::BAD_DISPLAY) {
            if (++stream.consecutiveRefused >= kRefusedThreshold) {
                stream.skipUntilFrame = mFrame + kRefusedBackoffFrames;
                stream.consecutiveRefused = 0;
            }
        } else {
            stream.consecutiveRefused = 0;
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
