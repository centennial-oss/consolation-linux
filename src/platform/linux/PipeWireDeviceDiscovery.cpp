#include "platform/linux/PipeWireDeviceDiscovery.h"

#if defined(CONSOLATION_HAS_PIPEWIRE)
#include <pipewire/pipewire.h>
#include <spa/utils/dict.h>
#endif

#include <QCoreApplication>

#include <cstring>

namespace consolation::platform::linux {

#if defined(CONSOLATION_HAS_PIPEWIRE)
namespace {

struct RegistryScan {
    pw_main_loop *loop = nullptr;
    pw_context *context = nullptr;
    pw_core *core = nullptr;
    pw_registry *registry = nullptr;
    spa_hook registryListener {};
    spa_hook coreListener {};
    std::vector<capture::CaptureDevice> devices;
    int pendingSync = 0;
};

QString property(const spa_dict *properties, const char *key)
{
    const auto *value = spa_dict_lookup(properties, key);
    return value == nullptr ? QString() : QString::fromUtf8(value);
}

bool isVideoSourceNode(const spa_dict *properties)
{
    const auto mediaClass = property(properties, PW_KEY_MEDIA_CLASS);
    if (mediaClass != QStringLiteral("Video/Source")) {
        return false;
    }

    const auto objectPath = property(properties, PW_KEY_OBJECT_PATH);
    const auto api = property(properties, "api");
    const auto deviceApi = property(properties, "device.api");
    return objectPath.contains(QStringLiteral("v4l2"), Qt::CaseInsensitive) ||
        api.contains(QStringLiteral("v4l2"), Qt::CaseInsensitive) ||
        deviceApi.contains(QStringLiteral("v4l2"), Qt::CaseInsensitive);
}

bool isLikelyUsbCaptureDevice(const spa_dict *properties)
{
    const auto name = property(properties, PW_KEY_NODE_DESCRIPTION) + QStringLiteral(" ") +
        property(properties, PW_KEY_NODE_NAME) + QStringLiteral(" ") +
        property(properties, PW_KEY_OBJECT_PATH);
    if (name.contains(QStringLiteral("facetime"), Qt::CaseInsensitive) ||
        name.contains(QStringLiteral("apple-isp"), Qt::CaseInsensitive)) {
        return false;
    }

    return name.contains(QStringLiteral("usb"), Qt::CaseInsensitive) ||
        name.contains(QStringLiteral("uvc"), Qt::CaseInsensitive) ||
        name.contains(QStringLiteral("capture"), Qt::CaseInsensitive) ||
        name.contains(QStringLiteral("video"), Qt::CaseInsensitive);
}

void registryGlobal(
    void *data,
    const uint32_t id,
    const uint32_t permissions,
    const char *type,
    const uint32_t version,
    const spa_dict *properties)
{
    Q_UNUSED(permissions);
    Q_UNUSED(version);

    auto *scan = static_cast<RegistryScan *>(data);
    if (properties == nullptr ||
        std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0 ||
        !isVideoSourceNode(properties) ||
        !isLikelyUsbCaptureDevice(properties)) {
        return;
    }

    capture::CaptureDevice device;
    device.backend = capture::CaptureBackend::PipeWire;
    device.backendNodeId = id;
    device.devicePath = QStringLiteral("pipewire://node/%1").arg(id);
    device.stableId = property(properties, PW_KEY_OBJECT_SERIAL);
    device.nodeName = property(properties, PW_KEY_NODE_NAME);
    device.v4l2DevicePath = property(properties, "api.v4l2.path");
    const auto objectPath = property(properties, PW_KEY_OBJECT_PATH);
    if (device.v4l2DevicePath.isEmpty() && objectPath.startsWith(QStringLiteral("v4l2:"))) {
        device.v4l2DevicePath = objectPath.mid(5);
    }
    if (device.stableId.isEmpty()) {
        device.stableId = device.nodeName;
    }
    device.displayName = property(properties, PW_KEY_NODE_DESCRIPTION);
    if (device.displayName.isEmpty()) {
        device.displayName = property(properties, PW_KEY_NODE_NAME);
    }
    if (device.displayName.isEmpty()) {
        device.displayName = QStringLiteral("PipeWire Camera %1").arg(id);
    }

    // PipeWire format enumeration is implemented with stream negotiation next. Keep one
    // selectable placeholder so the UI can route PipeWire-owned cameras to this backend.
    device.formats.push_back(capture::CaptureFormat{
        .width = 0,
        .height = 0,
        .framesPerSecond = 0.0,
        .pixelFormat = QStringLiteral("PipeWire"),
        .label = QStringLiteral("PipeWire negotiated format"),
    });
    scan->devices.push_back(std::move(device));
}

void registryGlobalRemove(void *data, const uint32_t id)
{
    Q_UNUSED(data);
    Q_UNUSED(id);
}

const pw_registry_events registryEvents = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = registryGlobal,
    .global_remove = registryGlobalRemove,
};

void coreDone(void *data, const uint32_t id, const int seq)
{
    auto *scan = static_cast<RegistryScan *>(data);
    if (id == PW_ID_CORE && seq == scan->pendingSync) {
        pw_main_loop_quit(scan->loop);
    }
}

void coreError(void *data, const uint32_t id, const int seq, const int res, const char *message)
{
    Q_UNUSED(id);
    Q_UNUSED(seq);
    Q_UNUSED(res);
    Q_UNUSED(message);

    auto *scan = static_cast<RegistryScan *>(data);
    pw_main_loop_quit(scan->loop);
}

const pw_core_events coreEvents = {
    .version = PW_VERSION_CORE_EVENTS,
    .done = coreDone,
    .error = coreError,
};

} // namespace
#endif

bool deviceMatchesTarget(
    const spa_dict *properties,
    const uint32_t id,
    const capture::CaptureDevice &device)
{
    if (!isVideoSourceNode(properties)) {
        return false;
    }

    const auto v4l2Path = property(properties, "api.v4l2.path");
    if (!device.v4l2DevicePath.isEmpty() && v4l2Path == device.v4l2DevicePath) {
        return true;
    }

    const auto nodeName = property(properties, PW_KEY_NODE_NAME);
    if (!device.nodeName.isEmpty() && nodeName == device.nodeName) {
        return true;
    }

    if (device.backendNodeId != 0 && id == device.backendNodeId) {
        return true;
    }

    const auto serial = property(properties, PW_KEY_OBJECT_SERIAL);
    return !device.stableId.isEmpty() && serial == device.stableId;
}

QString registryResolveTargetObject(RegistryScan &scan, const capture::CaptureDevice &device)
{
    struct ResolveContext {
        RegistryScan *scan = nullptr;
        const capture::CaptureDevice *device = nullptr;
        QString targetObject;
    } context{&scan, &device, {}};

    const pw_registry_events resolveEvents = {
        .version = PW_VERSION_REGISTRY_EVENTS,
        .global =
            [](void *data, const uint32_t id, const uint32_t permissions, const char *type, const uint32_t version, const spa_dict *properties) {
                Q_UNUSED(permissions);
                Q_UNUSED(version);
                auto *context = static_cast<ResolveContext *>(data);
                if (properties == nullptr ||
                    std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0 ||
                    !deviceMatchesTarget(properties, id, *context->device)) {
                    return;
                }

                const auto serial = property(properties, PW_KEY_OBJECT_SERIAL);
                if (!serial.isEmpty()) {
                    context->targetObject = serial;
                    pw_main_loop_quit(context->scan->loop);
                    return;
                }

                const auto nodeName = property(properties, PW_KEY_NODE_NAME);
                if (!nodeName.isEmpty()) {
                    context->targetObject = nodeName;
                    pw_main_loop_quit(context->scan->loop);
                }
            },
    };

    spa_hook resolveListener {};
    pw_registry_add_listener(scan.registry, &resolveListener, &resolveEvents, &context);
    scan.pendingSync = pw_core_sync(scan.core, PW_ID_CORE, 0);
    pw_main_loop_run(scan.loop);
    spa_hook_remove(&resolveListener);
    return context.targetObject;
}

std::vector<capture::CaptureDevice> PipeWireDeviceDiscovery::enumerateDevices() const
{
#if defined(CONSOLATION_HAS_PIPEWIRE)
    static bool initialized = false;
    if (!initialized) {
        pw_init(nullptr, nullptr);
        initialized = true;
    }

    RegistryScan scan;
    scan.loop = pw_main_loop_new(nullptr);
    if (scan.loop == nullptr) {
        return {};
    }

    scan.context = pw_context_new(pw_main_loop_get_loop(scan.loop), nullptr, 0);
    if (scan.context == nullptr) {
        pw_main_loop_destroy(scan.loop);
        return {};
    }

    scan.core = pw_context_connect(scan.context, nullptr, 0);
    if (scan.core == nullptr) {
        pw_context_destroy(scan.context);
        pw_main_loop_destroy(scan.loop);
        return {};
    }

    pw_core_add_listener(scan.core, &scan.coreListener, &coreEvents, &scan);
    scan.registry = pw_core_get_registry(scan.core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(scan.registry, &scan.registryListener, &registryEvents, &scan);
    scan.pendingSync = pw_core_sync(scan.core, PW_ID_CORE, 0);

    pw_main_loop_run(scan.loop);

    spa_hook_remove(&scan.registryListener);
    spa_hook_remove(&scan.coreListener);
    if (scan.registry != nullptr) {
        pw_proxy_destroy(reinterpret_cast<pw_proxy *>(scan.registry));
    }
    if (scan.core != nullptr) {
        pw_core_disconnect(scan.core);
    }
    if (scan.context != nullptr) {
        pw_context_destroy(scan.context);
    }
    if (scan.loop != nullptr) {
        pw_main_loop_destroy(scan.loop);
    }

    return scan.devices;
#else
    return {};
#endif
}

QString PipeWireDeviceDiscovery::resolveTargetObject(const capture::CaptureDevice &device) const
{
#if defined(CONSOLATION_HAS_PIPEWIRE)
    if (device.v4l2DevicePath.isEmpty() && device.nodeName.isEmpty() && device.backendNodeId == 0 &&
        device.stableId.isEmpty()) {
        return {};
    }

    static bool initialized = false;
    if (!initialized) {
        pw_init(nullptr, nullptr);
        initialized = true;
    }

    RegistryScan scan;
    scan.loop = pw_main_loop_new(nullptr);
    if (scan.loop == nullptr) {
        return device.stableId;
    }

    scan.context = pw_context_new(pw_main_loop_get_loop(scan.loop), nullptr, 0);
    if (scan.context == nullptr) {
        pw_main_loop_destroy(scan.loop);
        return device.stableId;
    }

    scan.core = pw_context_connect(scan.context, nullptr, 0);
    if (scan.core == nullptr) {
        pw_context_destroy(scan.context);
        pw_main_loop_destroy(scan.loop);
        return device.stableId;
    }

    pw_core_add_listener(scan.core, &scan.coreListener, &coreEvents, &scan);
    scan.registry = pw_core_get_registry(scan.core, PW_VERSION_REGISTRY, 0);

    const auto targetObject = registryResolveTargetObject(scan, device);

    spa_hook_remove(&scan.coreListener);
    if (scan.registry != nullptr) {
        pw_proxy_destroy(reinterpret_cast<pw_proxy *>(scan.registry));
    }
    if (scan.core != nullptr) {
        pw_core_disconnect(scan.core);
    }
    if (scan.context != nullptr) {
        pw_context_destroy(scan.context);
    }
    if (scan.loop != nullptr) {
        pw_main_loop_destroy(scan.loop);
    }

    return targetObject.isEmpty() ? device.stableId : targetObject;
#else
    Q_UNUSED(device);
    return {};
#endif
}

} // namespace consolation::platform::linux
