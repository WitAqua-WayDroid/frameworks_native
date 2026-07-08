/*
 * Copyright (C) 2024 The Android Open Source Project
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
#define LOG_TAG "libbinder.BackendUnifiedServiceManager"

#include "BackendUnifiedServiceManager.h"

#include <android-base/strings.h>
#include <android/os/IAccessor.h>
#include <android/os/IServiceManager.h>
#include <binder/ProcessState.h>
#include <binder/RpcSession.h>
#include <binder/Stability.h>
#include <cutils/sockets.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <fstream>
#include <mutex>
#include <string>
#include <unordered_set>

#if defined(__BIONIC__) && !defined(__ANDROID_VNDK__)
#include <android-base/properties.h>
#endif

namespace android {

// This is similar to the kernel binder servicemanager's context 0. It's the
// known socket that we expect the Unix Domain Socket servicemanager to be listening on.
const char kUdsServiceManagerName[] = ANDROID_SOCKET_DIR "/rpc_servicemanager";

#ifdef LIBBINDER_CLIENT_CACHE
constexpr bool kUseCache = true;
#else
constexpr bool kUseCache = false;
#endif

#ifdef LIBBINDER_ADDSERVICE_CACHE
constexpr bool kUseCacheInAddService = true;
#else
constexpr bool kUseCacheInAddService = false;
#endif

#ifdef LIBBINDER_REMOVE_CACHE_STATIC_LIST
constexpr bool kRemoveStaticList = true;
#else
constexpr bool kRemoveStaticList = false;
#endif

using AidlServiceManager = android::os::IServiceManager;
using android::os::IAccessor;
using binder::Status;

// Waydroid dual-driver: whitelist of AIDL service names to route to the host
// binder servicemanager. Loaded lazily from /system/etc/hostaidls.conf on
// first access; file format is one service name per line, '#' starts a
// comment, blank lines are ignored. Intentionally dep-free (no XML parser).
namespace {
[[clang::no_destroy]] static std::once_flag gHostAidlsOnce;
[[clang::no_destroy]] static std::unordered_set<std::string> gHostAidls;

static void loadHostAidlsLocked() {
    std::ifstream f("/system/etc/hostaidls.conf");
    if (!f.good()) {
        ALOGI("Waydroid: /system/etc/hostaidls.conf not found; "
              "host-AIDL passthrough disabled");
        return;
    }
    std::string line;
    while (std::getline(f, line)) {
        // strip comments
        size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        // trim
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        size_t end = line.find_last_not_of(" \t\r\n");
        std::string name = line.substr(start, end - start + 1);
        if (name.empty()) continue;
        gHostAidls.insert(name);
        ALOGI("Waydroid: host-AIDL whitelisted: %s", name.c_str());
    }
}
} // anonymous namespace (still inside ::android below)

// NOTE: This file is already inside `namespace android { ... }` which opens
// near the top of the translation unit and closes at EOF, so the following
// definition is emitted as `android::isHostAidlService` -- matching the
// declaration in BackendUnifiedServiceManager.h and the call sites in
// IServiceManager.cpp.
bool isHostAidlService(const std::string& name) {
    std::call_once(gHostAidlsOnce, loadHostAidlsLocked);
    if (gHostAidls.count(name) > 0) return true;
    // Also match if any whitelisted entry is a prefix of the service name
    // ending with /
    for (const auto& entry : gHostAidls) {
        if (name.size() > entry.size() && name[entry.size()] == '/' &&
            name.compare(0, entry.size(), entry) == 0) {
            return true;
        }
    }
    return false;
}

// Waydroid dual-driver: host servicemanager cross-version bridge. A16 inserted
// getService2/checkService2 into IServiceManager, shifting checkService from
// code 2 (A14/A15) to code 3 (A16) and every later code by two. So our A16 AIDL
// Bp talks code 3, which a pre-A16 host decodes as addService.

// checkService transaction code on a pre-A16 (A14/A15) servicemanager.
static constexpr uint32_t kLegacyCheckServiceCode = ::android::IBinder::FIRST_CALL_TRANSACTION + 1;

// Whether the host servicemanager predates A16 (cached). The AIDL probe sends
// code 3: on A16 it resolves to checkService (ok); on a pre-A16 host it hits
// addService with a truncated parcel and fails, so a failed probe means pre-A16.
static bool hostServicemanagerIsPreA16() {
    static std::once_flag once;
    static bool preA16 = true; // safe default: the host we bridge to is A14
    std::call_once(once, [] {
        sp<ProcessState> host = ProcessState::self(/*isHost=*/true);
        if (host == nullptr) return;
        sp<IBinder> ctx = host->getContextObject(nullptr);
        if (ctx == nullptr) return;
        sp<AidlServiceManager> hostSm = interface_cast<AidlServiceManager>(ctx);
        if (hostSm == nullptr) return;

        sp<IBinder> dummy;
        Status s = hostSm->checkService("__waydroid_version_probe__", &dummy);
        preA16 = !s.isOk();
        ALOGI("Waydroid: host servicemanager is %s (probe: %s)", preA16 ? "pre-A16" : "A16+",
              s.toString8().c_str());
    });
    return preA16;
}

// Pre-A16 host: raw legacy checkService (code 2). Request/reply envelope is
// wire-compatible with the old servicemanager; the reply binder is resolved
// against the host ProcessState via BpBinder::transact's SetIsHost() stamp.
static Status hostCheckServiceLegacy(const sp<IBinder>& ctx, const std::string& name,
                                     sp<IBinder>* out) {
    Parcel data;
    data.writeInterfaceToken(String16("android.os.IServiceManager"));
    data.writeUtf8AsUtf16(name);

    Parcel reply;
    if (status_t err = ctx->transact(kLegacyCheckServiceCode, data, &reply, 0); err != NO_ERROR) {
        ALOGW("Waydroid: host legacy checkService(%s) transact failed: %s (%d)", name.c_str(),
              strerror(-err), err);
        return Status::fromStatusT(err);
    }

    Status status;
    if (status_t ret = status.readFromParcel(reply); ret != OK) {
        return Status::fromStatusT(ret);
    }
    if (!status.isOk()) return status;

    if (status_t ret = reply.readNullableStrongBinder(out); ret != OK) {
        return Status::fromStatusT(ret);
    }
    return Status::ok();
}

// A16+ host: the native checkService (code 3) matches our AIDL Bp directly.
static Status hostCheckServiceAidl(const sp<IBinder>& ctx, const std::string& name,
                                   sp<IBinder>* out) {
    sp<AidlServiceManager> hostSm = interface_cast<AidlServiceManager>(ctx);
    if (hostSm == nullptr) {
        ALOGW("Waydroid: host context is not an AidlServiceManager");
        return Status::ok();
    }
    return hostSm->checkService(name, out);
}

static const char* kUnsupportedOpNoServiceManager =
        "Unsupported operation without a kernel binder servicemanager process";

static const char* kStaticCachableList[] = {
        // go/keep-sorted start
        "accessibility",
        "account",
        "activity",
        "alarm",
        "android.frameworks.stats.IStats/default",
        "android.system.keystore2.IKeystoreService/default",
        "appops",
        "audio",
        "autofill",
        "batteryproperties",
        "batterystats",
        "biometic",
        "carrier_config",
        "connectivity",
        "content",
        "content_capture",
        "device_policy",
        "display",
        "dropbox",
        "econtroller",
        "graphicsstats",
        "input",
        "input_method",
        "isub",
        "jobscheduler",
        "legacy_permission",
        "location",
        "lock_settings",
        "media.extractor",
        "media.metrics",
        "media.player",
        "media.resource_manager",
        "media_resource_monitor",
        "mount",
        "netd_listener",
        "netstats",
        "network_management",
        "nfc",
        "notification",
        "package",
        "package_native",
        "performance_hint",
        "permission",
        "permission_checker",
        "permissionmgr",
        "phone",
        "platform_compat",
        "power",
        "processinfo",
        "role",
        "sensitive_content_protection_service",
        "sensorservice",
        "statscompanion",
        "telephony.registry",
        "thermalservice",
        "time_detector",
        "tracing.proxy",
        "trust",
        "uimode",
        "user",
        "vibrator",
        "virtualdevice",
        "virtualdevice_native",
        "webviewupdate",
        "window",
        // go/keep-sorted end
};

os::ServiceWithMetadata createServiceWithMetadata(const sp<IBinder>& service, bool isLazyService) {
    os::ServiceWithMetadata out = os::ServiceWithMetadata();
    out.service = service;
    out.isLazyService = isLazyService;
    return out;
}

bool BinderCacheWithInvalidation::isClientSideCachingEnabled(const std::string& serviceName) const {
    sp<ProcessState> self = ProcessState::selfOrNull();
    // Should not cache if process state could not be found, or if thread pool
    // max could is not greater than zero.
    if (!self) {
        ALOGW("Service retrieved before binder threads started. If they are to be started, "
              "consider starting binder threads earlier.");
        return false;
    } else if (self->getThreadPoolMaxTotalThreadCount() <= 0) {
        ALOGW("Thread Pool max thread count is 0. Cannot cache binder as linkToDeath cannot be "
              "implemented. serviceName: %s",
              serviceName.c_str());
        return false;
    }
    if (kRemoveStaticList) return true;
    for (const char* name : kStaticCachableList) {
        if (name == serviceName) {
            return true;
        }
    }
    return false;
}

Status BackendUnifiedServiceManager::updateCache(const std::string& serviceName,
                                                 const os::Service& service) {
    if (!kUseCache) {
        return Status::ok();
    }

    if (service.getTag() == os::Service::Tag::serviceWithMetadata) {
        auto serviceWithMetadata = service.get<os::Service::Tag::serviceWithMetadata>();
        return updateCache(serviceName, serviceWithMetadata.service,
                           serviceWithMetadata.isLazyService);
    }
    return Status::ok();
}

Status BackendUnifiedServiceManager::updateCache(const std::string& serviceName,
                                                 const sp<IBinder>& binder, bool isServiceLazy) {
    std::string traceStr;
    // Don't cache if service is lazy
    if (kRemoveStaticList && isServiceLazy) {
        return Status::ok();
    }
    if (atrace_is_tag_enabled(ATRACE_TAG_AIDL)) {
        traceStr = "BinderCacheWithInvalidation::updateCache : " + serviceName;
    }
    binder::ScopedTrace outerAidlTrace(ATRACE_TAG_AIDL, traceStr.c_str());
    if (!binder) {
        binder::ScopedTrace
                aidlTrace(ATRACE_TAG_AIDL,
                          "BinderCacheWithInvalidation::updateCache failed: binder_null");
    } else if (!binder->isBinderAlive()) {
        binder::ScopedTrace aidlTrace(ATRACE_TAG_AIDL,
                                      "BinderCacheWithInvalidation::updateCache failed: "
                                      "isBinderAlive_false");
    }
    // If we reach here with kRemoveStaticList=true then we know service isn't lazy
    else if (mCacheForGetService->isClientSideCachingEnabled(serviceName)) {
        binder::ScopedTrace aidlTrace(ATRACE_TAG_AIDL,
                                      "BinderCacheWithInvalidation::updateCache successful");
        return mCacheForGetService->setItem(serviceName, binder);
    } else {
        binder::ScopedTrace aidlTrace(ATRACE_TAG_AIDL,
                                      "BinderCacheWithInvalidation::updateCache failed: "
                                      "caching_not_enabled");
    }
    return Status::ok();
}

bool BackendUnifiedServiceManager::returnIfCached(const std::string& serviceName,
                                                  os::Service* _out) {
    if (!kUseCache) {
        return false;
    }
    // Waydroid dual-driver: never serve host-AIDL names from the normal cache
    // (they live in the host servicemanager, not the local one).
    if (isHostAidlService(serviceName)) {
        return false;
    }
    sp<IBinder> item = mCacheForGetService->getItem(serviceName);
    // TODO(b/363177618): Enable caching for binders which are always null.
    if (item != nullptr && item->isBinderAlive()) {
        *_out = createServiceWithMetadata(item, false);
        return true;
    }
    return false;
}

Status BackendUnifiedServiceManager::queryHostService(const std::string& name,
                                                      os::Service* _out) {
    *_out = os::Service::make<os::Service::Tag::serviceWithMetadata>(
            createServiceWithMetadata(nullptr, false));

    sp<ProcessState> host = ProcessState::self(/*isHost=*/true);
    if (host == nullptr || host->getDriverName().empty()) {
        ALOGW("Waydroid: host ProcessState unavailable for %s", name.c_str());
        return Status::ok();
    }
    sp<IBinder> ctx = host->getContextObject(nullptr);
    if (ctx == nullptr) {
        ALOGW("Waydroid: no host servicemanager context for %s", name.c_str());
        return Status::ok();
    }
    // Stability is already set by getContextObject()'s markCompilationUnit().
    // Do NOT forceDowngradeToLocalStability() on host binders: they are remote
    // BpBinders and that path asserts local-only (BBinder) and would FATAL.

    sp<IBinder> out;
    Status status = hostServicemanagerIsPreA16() ? hostCheckServiceLegacy(ctx, name, &out)
                                                 : hostCheckServiceAidl(ctx, name, &out);
    if (!status.isOk()) {
        ALOGW("Waydroid: host checkService(%s) failed: %s", name.c_str(),
              status.toString8().c_str());
        return status;
    }

    if (out != nullptr) {
        ALOGI("Waydroid: host resolved %s -> binder %p", name.c_str(), out.get());
        *_out = os::Service::make<os::Service::Tag::serviceWithMetadata>(
                createServiceWithMetadata(out, false));
    }
    return status;
}

BackendUnifiedServiceManager::BackendUnifiedServiceManager(const sp<AidlServiceManager>& impl)
      : mTheRealServiceManager(impl) {
    mCacheForGetService = std::make_shared<BinderCacheWithInvalidation>();
}

Status BackendUnifiedServiceManager::getService(const ::std::string& name,
                                                sp<IBinder>* _aidl_return) {
    os::Service service;
    Status status = getService2(name, &service);
    if (status.isOk()) {
        *_aidl_return = service.get<os::Service::Tag::serviceWithMetadata>().service;
    }
    return status;
}

Status BackendUnifiedServiceManager::getService2(const ::std::string& name, os::Service* _out) {
    if (returnIfCached(name, _out)) {
        return Status::ok();
    }
    os::Service service;
    Status status = Status::ok();
    if (mTheRealServiceManager) {
        status = mTheRealServiceManager->getService2(name, &service);
    }

    if (status.isOk()) {
        status = toBinderService(name, service, _out);
        if (status.isOk()) {
            return updateCache(name, service);
        }
    }
    return status;
}

Status BackendUnifiedServiceManager::checkService(const ::std::string& name,
                                                  sp<IBinder>* _aidl_return) {
    os::Service service;
    Status status = checkService2(name, &service);
    if (status.isOk()) {
        *_aidl_return = service.get<os::Service::Tag::serviceWithMetadata>().service;
    }
    return status;
}

Status BackendUnifiedServiceManager::checkService2(const ::std::string& name, os::Service* _out) {
    os::Service service;
    if (returnIfCached(name, _out)) {
        return Status::ok();
    }

    Status status = Status::ok();
    if (mTheRealServiceManager) {
        status = mTheRealServiceManager->checkService2(name, &service);
    }
    if (status.isOk()) {
        status = toBinderService(name, service, _out);
        if (status.isOk()) {
            // Waydroid dual-driver: if the local servicemanager returned null
            // for a whitelisted AIDL name, retry against the host binder.
            if (isHostAidlService(name)) {
                const auto meta = _out->get<os::Service::Tag::serviceWithMetadata>();
                ALOGI("Waydroid: checkService2(%s) local=%s, trying host",
                      name.c_str(), meta.service != nullptr ? "non-null" : "null");
                if (meta.service == nullptr) {
                    Status hostStatus = queryHostService(name, _out);
                    ALOGI("Waydroid: queryHostService(%s) returned %s",
                          name.c_str(), hostStatus.isOk() ? "ok" : hostStatus.toString8().c_str());
                    if (!hostStatus.isOk()) return hostStatus;
                    // Do not cache host services.
                    return Status::ok();
                }
            }
            return updateCache(name, service);
        }
    }
    return status;
}

Status BackendUnifiedServiceManager::toBinderService(const ::std::string& name,
                                                     const os::Service& in, os::Service* _out) {
    switch (in.getTag()) {
        case os::Service::Tag::serviceWithMetadata: {
            auto serviceWithMetadata = in.get<os::Service::Tag::serviceWithMetadata>();
            if (serviceWithMetadata.service == nullptr) {
                // failed to find a service. Check to see if we have any local
                // injected Accessors for this service.
                os::Service accessor;
                Status status = getInjectedAccessor(name, &accessor);
                if (!status.isOk()) {
                    *_out = os::Service::make<os::Service::Tag::serviceWithMetadata>(
                            createServiceWithMetadata(nullptr, false));
                    return status;
                }
                if (accessor.getTag() == os::Service::Tag::accessor &&
                    accessor.get<os::Service::Tag::accessor>() != nullptr) {
                    ALOGI("Found local injected service for %s, will attempt to create connection",
                          name.c_str());
                    // Call this again using the accessor Service to get the real
                    // service's binder into _out
                    return toBinderService(name, accessor, _out);
                }
            }

            *_out = in;
            return Status::ok();
        }
        case os::Service::Tag::accessor: {
            sp<IBinder> accessorBinder = in.get<os::Service::Tag::accessor>();
            sp<IAccessor> accessor = interface_cast<IAccessor>(accessorBinder);
            if (accessor == nullptr) {
                ALOGE("Service#accessor doesn't have accessor. VM is maybe starting...");
                *_out = os::Service::make<os::Service::Tag::serviceWithMetadata>(
                        createServiceWithMetadata(nullptr, false));
                return Status::ok();
            }
            // Pre-flight check to handle transient connection errors.
            os::ParcelFileDescriptor pfd;
            if (Status status = accessor->addConnection(&pfd); !status.isOk()) {
                if (status.serviceSpecificErrorCode() ==
                    IAccessor::ERROR_FAILED_TO_CONNECT_TO_SOCKET) {
                    ALOGW("Accessor failed with transient error, returning null to retry: %s",
                          status.toString8().c_str());
                    *_out = os::Service::make<os::Service::Tag::serviceWithMetadata>(
                            createServiceWithMetadata(nullptr, false));
                    return Status::ok();
                }
                ALOGE("Failed to add connection via accessor: %s", status.toString8().c_str());
                return status;
            }
            auto request = [=] {
                os::ParcelFileDescriptor fd;
                Status ret = accessor->addConnection(&fd);
                if (ret.isOk()) {
                    return base::unique_fd(fd.release());
                } else {
                    ALOGE("Failed to connect to RpcSession: %s", ret.toString8().c_str());
                    return base::unique_fd(-1);
                }
            };
            auto session = RpcSession::make();
            status_t status =
                    session->setupPreconnectedClient(base::unique_fd(pfd.release()), request);
            if (status != OK) {
                ALOGE("Failed to set up preconnected binder RPC client: %s",
                      statusToString(status).c_str());
                return Status::fromStatusT(status);
            }
            session->setSessionSpecificRoot(accessorBinder);
            *_out = os::Service::make<os::Service::Tag::serviceWithMetadata>(
                    createServiceWithMetadata(session->getRootObject(), false));
            return Status::ok();
        }
        default: {
            LOG_ALWAYS_FATAL("Unknown service type: %d", in.getTag());
        }
    }
}

Status BackendUnifiedServiceManager::addService(const ::std::string& name,
                                                const sp<IBinder>& service, bool allowIsolated,
                                                int32_t dumpPriority) {
    if (mTheRealServiceManager) {
        Status status =
                mTheRealServiceManager->addService(name, service, allowIsolated, dumpPriority);
        // mEnableAddServiceCache is true by default.
        if (kUseCacheInAddService && mEnableAddServiceCache && status.isOk()) {
            return updateCache(name, service,
                               dumpPriority & android::os::IServiceManager::FLAG_IS_LAZY_SERVICE);
        }
        return status;
    }
    return Status::fromExceptionCode(Status::EX_UNSUPPORTED_OPERATION,
                                     kUnsupportedOpNoServiceManager);
}
Status BackendUnifiedServiceManager::listServices(int32_t dumpPriority,
                                                  ::std::vector<::std::string>* _aidl_return) {
    Status status = Status::ok();
    if (mTheRealServiceManager) {
        status = mTheRealServiceManager->listServices(dumpPriority, _aidl_return);
    }
    if (!status.isOk()) return status;

    appendInjectedAccessorServices(_aidl_return);

    return status;
}
Status BackendUnifiedServiceManager::registerForNotifications(
        const ::std::string& name, const sp<os::IServiceCallback>& callback) {
    if (mTheRealServiceManager) {
        return mTheRealServiceManager->registerForNotifications(name, callback);
    }
    return Status::fromExceptionCode(Status::EX_UNSUPPORTED_OPERATION,
                                     kUnsupportedOpNoServiceManager);
}
Status BackendUnifiedServiceManager::unregisterForNotifications(
        const ::std::string& name, const sp<os::IServiceCallback>& callback) {
    if (mTheRealServiceManager) {
        return mTheRealServiceManager->unregisterForNotifications(name, callback);
    }
    return Status::fromExceptionCode(Status::EX_UNSUPPORTED_OPERATION,
                                     kUnsupportedOpNoServiceManager);
}
Status BackendUnifiedServiceManager::isDeclared(const ::std::string& name, bool* _aidl_return) {
    Status status = Status::ok();
    if (mTheRealServiceManager) {
        status = mTheRealServiceManager->isDeclared(name, _aidl_return);
    }
    if (!status.isOk()) return status;

    if (!*_aidl_return) {
        forEachInjectedAccessorService([&](const std::string& instance) {
            if (name == instance) {
                *_aidl_return = true;
            }
        });
    }

    // Waydroid: host-AIDL services are not in the container VINTF manifest
    // but are reachable via the dual-driver binder bridge. Which services
    // appear declared is controlled by /system/etc/hostaidls.conf on device.
    if (!*_aidl_return && isHostAidlService(name)) {
        *_aidl_return = true;
    }

    return status;
}
Status BackendUnifiedServiceManager::getDeclaredInstances(
        const ::std::string& iface, ::std::vector<::std::string>* _aidl_return) {
    Status status = Status::ok();
    if (mTheRealServiceManager) {
        status = mTheRealServiceManager->getDeclaredInstances(iface, _aidl_return);
    }
    if (!status.isOk()) return status;

    forEachInjectedAccessorService([&](const std::string& instance) {
        // Declared instances have the format
        // <interface>/instance like foo.bar.ISomething/instance
        // If it does not have that format, consider the instance to be ""
        std::string_view name(instance);
        if (base::ConsumePrefix(&name, iface + "/")) {
            _aidl_return->emplace_back(name);
        } else if (iface == instance) {
            _aidl_return->push_back("");
        }
    });

    return status;
}
Status BackendUnifiedServiceManager::updatableViaApex(
        const ::std::string& name, ::std::optional<::std::string>* _aidl_return) {
    if (mTheRealServiceManager) {
        return mTheRealServiceManager->updatableViaApex(name, _aidl_return);
    }
    return Status::fromExceptionCode(Status::EX_UNSUPPORTED_OPERATION,
                                     kUnsupportedOpNoServiceManager);
}
Status BackendUnifiedServiceManager::getUpdatableNames(const ::std::string& apexName,
                                                       ::std::vector<::std::string>* _aidl_return) {
    if (mTheRealServiceManager) {
        return mTheRealServiceManager->getUpdatableNames(apexName, _aidl_return);
    }
    return Status::fromExceptionCode(Status::EX_UNSUPPORTED_OPERATION,
                                     kUnsupportedOpNoServiceManager);
}
Status BackendUnifiedServiceManager::getConnectionInfo(
        const ::std::string& name, ::std::optional<os::ConnectionInfo>* _aidl_return) {
    if (mTheRealServiceManager) {
        return mTheRealServiceManager->getConnectionInfo(name, _aidl_return);
    }
    return Status::fromExceptionCode(Status::EX_UNSUPPORTED_OPERATION,
                                     kUnsupportedOpNoServiceManager);
}
Status BackendUnifiedServiceManager::registerClientCallback(
        const ::std::string& name, const sp<IBinder>& service,
        const sp<os::IClientCallback>& callback) {
    if (mTheRealServiceManager) {
        return mTheRealServiceManager->registerClientCallback(name, service, callback);
    }
    return Status::fromExceptionCode(Status::EX_UNSUPPORTED_OPERATION,
                                     kUnsupportedOpNoServiceManager);
}
Status BackendUnifiedServiceManager::tryUnregisterService(const ::std::string& name,
                                                          const sp<IBinder>& service) {
    if (mTheRealServiceManager) {
        return mTheRealServiceManager->tryUnregisterService(name, service);
    }
    return Status::fromExceptionCode(Status::EX_UNSUPPORTED_OPERATION,
                                     kUnsupportedOpNoServiceManager);
}
Status BackendUnifiedServiceManager::getServiceDebugInfo(
        ::std::vector<os::ServiceDebugInfo>* _aidl_return) {
    if (mTheRealServiceManager) {
        return mTheRealServiceManager->getServiceDebugInfo(_aidl_return);
    }
    return Status::fromExceptionCode(Status::EX_UNSUPPORTED_OPERATION,
                                     kUnsupportedOpNoServiceManager);
}

Status BackendUnifiedServiceManager::checkServiceAccess(
        const AidlServiceManager::CallerContext& callerCtx, const std::string& name,
        const std::string& permission, bool* _aidl_return) {
    if (mTheRealServiceManager) {
        return mTheRealServiceManager->checkServiceAccess(callerCtx, name, permission,
                                                          _aidl_return);
    }
    return Status::fromExceptionCode(Status::EX_UNSUPPORTED_OPERATION,
                                     kUnsupportedOpNoServiceManager);
}

[[clang::no_destroy]] static std::once_flag gUSmOnce;
[[clang::no_destroy]] static sp<BackendUnifiedServiceManager> gUnifiedServiceManager;

static bool hasOutOfProcessServiceManager() {
// We don't currently support kernel binder service management or UDS
// service management on host or when libbinder is compiled without any
// kernel binder suport. Please use setDefaultServiceManager for host
// processes that want to use service manager APIs.
#if !defined(BINDER_WITH_KERNEL_IPC) || !defined(__BIONIC__)
    return false;
#else
#ifdef __ANDROID_VNDK__
    return true;
#else
    return android::base::GetBoolProperty("servicemanager.installed", true);
#endif
#endif
}

static sp<AidlServiceManager> getUdsServiceManager() {
    auto session = RpcSession::make();
    session->setFileDescriptorTransportMode(RpcSession::FileDescriptorTransportMode::UNIX);
    auto status = session->setupUnixDomainClient(kUdsServiceManagerName);
    if (status == OK) {
        return interface_cast<AidlServiceManager>(session->getRootObject());
    }
    return nullptr;
}

sp<BackendUnifiedServiceManager> getBackendUnifiedServiceManager() {
    std::call_once(gUSmOnce, []() {
#if defined(__BIONIC__) && !defined(__ANDROID_VNDK__)
        /* wait for service manager */
        if (hasOutOfProcessServiceManager()) {
            using std::literals::chrono_literals::operator""s;
            using android::base::WaitForProperty;
            while (!WaitForProperty("servicemanager.ready", "true", 1s)) {
                ALOGE("Waited for servicemanager.ready for a second, waiting another...");
            }
        }
#endif

        sp<AidlServiceManager> sm = nullptr;
        while (hasOutOfProcessServiceManager() && sm == nullptr) {
            // There is either a kernel binder service manager, or an RPC binder
            // service manager
            sp<ProcessState> ps = ProcessState::selfIfKernelBinderEnabled();
            if (ps) {
                // Service management over kernel binder
                sm = interface_cast<AidlServiceManager>(ps->getContextObject(nullptr));
            } else {
                // Check for service management over Unix Domain Sockets
                sm = getUdsServiceManager();
            }

            if (sm == nullptr) {
                std::string contextObjectName = ps
                        ? ps->getDriverName() + ", " + kUdsServiceManagerName
                        : kUdsServiceManagerName;
                ALOGE("Waiting 1s on context object(s) on %s.", contextObjectName.c_str());
                sleep(1);
            }
        }

        gUnifiedServiceManager = sp<BackendUnifiedServiceManager>::make(sm);
    });

    return gUnifiedServiceManager;
}

} // namespace android
