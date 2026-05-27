#include "platform/linux/YuyvDmaBufGl.h"

#include <QOpenGLContext>
#include <QRect>

#include <array>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#ifndef EGL_LINUX_DMA_BUF_EXT
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#endif
#ifndef EGL_LINUX_DRM_FOURCC_EXT
#define EGL_LINUX_DRM_FOURCC_EXT 0x3271
#endif
#ifndef EGL_DMA_BUF_PLANE0_FD_EXT
#define EGL_DMA_BUF_PLANE0_FD_EXT 0x3272
#endif
#ifndef EGL_DMA_BUF_PLANE0_OFFSET_EXT
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#endif
#ifndef EGL_DMA_BUF_PLANE0_PITCH_EXT
#define EGL_DMA_BUF_PLANE0_PITCH_EXT 0x3274
#endif

#ifndef DRM_FORMAT_RG88
#define DRM_FORMAT_RG88 static_cast<EGLint>(0x38385247)
#endif
#ifndef DRM_FORMAT_GR88
#define DRM_FORMAT_GR88 static_cast<EGLint>(0x38384752)
#endif

namespace consolation::platform::linux {

namespace {

using PFNEGLCREATEIMAGEKHRPROC = EGLImageKHR (*)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
using PFNEGLDESTROYIMAGEKHRPROC = EGLBoolean (*)(EGLDisplay, EGLImageKHR);
using PFNGLBINDTEXTUREPROC = void (*)(unsigned int, unsigned int);
using PFNGLGEN_TEXTURESPROC = void (*)(int, unsigned int *);
using PFNGLDELETE_TEXTURESPROC = void (*)(int, const unsigned int *);
using PFNGLEGLIMAGETARGETTEXTURE2DOESPROC = void (*)(unsigned int, void *);

PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHRFn = nullptr;
PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHRFn = nullptr;
PFNGLBINDTEXTUREPROC glBindTextureFn = nullptr;
PFNGLGEN_TEXTURESPROC glGenTexturesFn = nullptr;
PFNGLDELETE_TEXTURESPROC glDeleteTexturesFn = nullptr;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOESFn = nullptr;

using PFNGLGENVERTEXARRAYSPROC = void (*)(int, unsigned int *);
using PFNGLBINDVERTEXARRAYPROC = void (*)(unsigned int);
using PFNGLDELETEVERTEXARRAYSPROC = void (*)(int, const unsigned int *);

PFNGLGENVERTEXARRAYSPROC glGenVertexArraysFn = nullptr;
PFNGLBINDVERTEXARRAYPROC glBindVertexArrayFn = nullptr;
PFNGLDELETEVERTEXARRAYSPROC glDeleteVertexArraysFn = nullptr;

struct ShaderSources {
    const char *vertex = nullptr;
    const char *fragment = nullptr;
    bool bindAttribLocations = false;
    const char *label = nullptr;
};

std::array<ShaderSources, 3> shaderCandidates(const QOpenGLContext *context)
{
    static constexpr char esVertexShader[] = R"(
        attribute vec2 aPos;
        attribute vec2 aTexCoord;
        varying vec2 vTexCoord;
        void main() {
            gl_Position = vec4(aPos, 0.0, 1.0);
            vTexCoord = aTexCoord;
        }
    )";

    // Import YUYV bytes as RG88/GR88 so shader channel mapping is deterministic.
    static constexpr char esFragmentShader[] = R"(
        precision mediump float;
        varying vec2 vTexCoord;
        uniform sampler2D uFrame;
        uniform float uWidth;
        uniform int uPairOrder;
        vec2 bytePair(vec4 sampleValue) {
            return (uPairOrder == 0) ? sampleValue.rg : sampleValue.gr;
        }
        void main() {
            float x = floor(vTexCoord.x * uWidth);
            float isOdd = mod(x, 2.0);
            float pairedX = (isOdd < 1.0) ? min(x + 1.0, uWidth - 1.0) : max(x - 1.0, 0.0);
            vec2 current = bytePair(texture2D(uFrame, vTexCoord));
            vec2 paired = bytePair(texture2D(uFrame, vec2((pairedX + 0.5) / uWidth, vTexCoord.y)));
            float y = current.x;
            float u = ((isOdd < 1.0) ? current.y : paired.y) - 0.5;
            float v = ((isOdd < 1.0) ? paired.y : current.y) - 0.5;
            gl_FragColor = vec4(
                y + 1.402 * v,
                y - 0.344 * u - 0.714 * v,
                y + 1.772 * u,
                1.0);
        }
    )";

    static constexpr char gl330VertexShader[] = R"(
        #version 330 core
        layout(location = 0) in vec2 aPos;
        layout(location = 1) in vec2 aTexCoord;
        out vec2 vTexCoord;
        void main() {
            gl_Position = vec4(aPos, 0.0, 1.0);
            vTexCoord = aTexCoord;
        }
    )";

    static constexpr char gl330FragmentShader[] = R"(
        #version 330 core
        in vec2 vTexCoord;
        uniform sampler2D uFrame;
        uniform float uWidth;
        uniform int uPairOrder;
        out vec4 fragColor;
        vec2 bytePair(vec4 sampleValue) {
            return (uPairOrder == 0) ? sampleValue.rg : sampleValue.gr;
        }
        void main() {
            float x = floor(vTexCoord.x * uWidth);
            float isOdd = mod(x, 2.0);
            float pairedX = (isOdd < 1.0) ? min(x + 1.0, uWidth - 1.0) : max(x - 1.0, 0.0);
            vec2 current = bytePair(texture(uFrame, vTexCoord));
            vec2 paired = bytePair(texture(uFrame, vec2((pairedX + 0.5) / uWidth, vTexCoord.y)));
            float y = current.x;
            float u = ((isOdd < 1.0) ? current.y : paired.y) - 0.5;
            float v = ((isOdd < 1.0) ? paired.y : current.y) - 0.5;
            fragColor = vec4(
                y + 1.402 * v,
                y - 0.344 * u - 0.714 * v,
                y + 1.772 * u,
                1.0);
        }
    )";

    static constexpr char gl120VertexShader[] = R"(
        #version 120
        attribute vec2 aPos;
        attribute vec2 aTexCoord;
        varying vec2 vTexCoord;
        void main() {
            gl_Position = vec4(aPos, 0.0, 1.0);
            vTexCoord = aTexCoord;
        }
    )";

    static constexpr char gl120FragmentShader[] = R"(
        #version 120
        varying vec2 vTexCoord;
        uniform sampler2D uFrame;
        uniform float uWidth;
        uniform int uPairOrder;
        vec2 bytePair(vec4 sampleValue) {
            return (uPairOrder == 0) ? sampleValue.rg : sampleValue.gr;
        }
        void main() {
            float x = floor(vTexCoord.x * uWidth);
            float isOdd = mod(x, 2.0);
            float pairedX = (isOdd < 1.0) ? min(x + 1.0, uWidth - 1.0) : max(x - 1.0, 0.0);
            vec2 current = bytePair(texture2D(uFrame, vTexCoord));
            vec2 paired = bytePair(texture2D(uFrame, vec2((pairedX + 0.5) / uWidth, vTexCoord.y)));
            float y = current.x;
            float u = ((isOdd < 1.0) ? current.y : paired.y) - 0.5;
            float v = ((isOdd < 1.0) ? paired.y : current.y) - 0.5;
            gl_FragColor = vec4(
                y + 1.402 * v,
                y - 0.344 * u - 0.714 * v,
                y + 1.772 * u,
                1.0);
        }
    )";

    const ShaderSources esProfile {esVertexShader, esFragmentShader, true, "es"};
    const ShaderSources gl330Profile {gl330VertexShader, gl330FragmentShader, false, "gl330"};
    const ShaderSources gl120Profile {gl120VertexShader, gl120FragmentShader, true, "gl120"};

    if (context != nullptr && context->isOpenGLES()) {
        return {esProfile, gl120Profile, gl330Profile};
    }

    if (context != nullptr && context->format().majorVersion() >= 3) {
        return {gl330Profile, gl120Profile, esProfile};
    }

    return {gl120Profile, esProfile, gl330Profile};
}

bool compileShader(
    QOpenGLFunctions &gl,
    const unsigned int type,
    const char *source,
    unsigned int &outShader,
    QString &failure)
{
    outShader = gl.glCreateShader(static_cast<GLenum>(type));
    gl.glShaderSource(outShader, 1, &source, nullptr);
    gl.glCompileShader(outShader);

    GLint compileStatus = GL_FALSE;
    gl.glGetShaderiv(outShader, GL_COMPILE_STATUS, &compileStatus);
    if (compileStatus == GL_TRUE) {
        return true;
    }

    char log[1024] {};
    gl.glGetShaderInfoLog(outShader, static_cast<GLsizei>(sizeof(log) - 1), nullptr, log);
    failure = QStringLiteral("shader compile failed: %1").arg(QString::fromLocal8Bit(log).trimmed());
    gl.glDeleteShader(outShader);
    outShader = 0;
    return false;
}

QString programLinkLog(QOpenGLFunctions &gl, const unsigned int program)
{
    char log[1024] {};
    gl.glGetProgramInfoLog(program, static_cast<GLsizei>(sizeof(log) - 1), nullptr, log);
    return QString::fromLocal8Bit(log).trimmed();
}

void *createPlaneImage(
    const EGLDisplay display,
    const int dmaFd,
    const int width,
    const int height,
    const int stride,
    const EGLint fourcc)
{
    const EGLint attribs[] = {
        EGL_WIDTH,
        width,
        EGL_HEIGHT,
        height,
        EGL_LINUX_DRM_FOURCC_EXT,
        fourcc,
        EGL_DMA_BUF_PLANE0_FD_EXT,
        dmaFd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,
        0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT,
        stride,
        EGL_NONE,
    };

    return eglCreateImageKHRFn(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);
}

bool bindEglImageToTexture(const unsigned int textureId, void *const eglImage)
{
    if (eglImage == EGL_NO_IMAGE_KHR || textureId == 0) {
        return false;
    }

    glBindTextureFn(GL_TEXTURE_2D, textureId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glEGLImageTargetTexture2DOESFn(GL_TEXTURE_2D, static_cast<EGLImageKHR>(eglImage));
    glBindTextureFn(GL_TEXTURE_2D, 0);
    return true;
}

} // namespace

bool YuyvDmaBufGl::resolveEglDisplay()
{
    if (const auto *context = QOpenGLContext::currentContext()) {
        if (const auto *egl = context->nativeInterface<QNativeInterface::QEGLContext>()) {
            eglDisplay_ = egl->display();
            if (eglDisplay_ != EGL_NO_DISPLAY) {
                return true;
            }
        }
    }

    eglDisplay_ = eglGetCurrentDisplay();
    return eglDisplay_ != EGL_NO_DISPLAY;
}

bool YuyvDmaBufGl::resolveExtensions()
{
    eglCreateImageKHRFn = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    eglDestroyImageKHRFn = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    glEGLImageTargetTexture2DOESFn =
        reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    glBindTextureFn = reinterpret_cast<PFNGLBINDTEXTUREPROC>(eglGetProcAddress("glBindTexture"));
    glGenTexturesFn = reinterpret_cast<PFNGLGEN_TEXTURESPROC>(eglGetProcAddress("glGenTextures"));
    glDeleteTexturesFn = reinterpret_cast<PFNGLDELETE_TEXTURESPROC>(eglGetProcAddress("glDeleteTextures"));

    // VAO support is optional — resolve without failing if unavailable (ES2, older GL).
    glGenVertexArraysFn = reinterpret_cast<PFNGLGENVERTEXARRAYSPROC>(eglGetProcAddress("glGenVertexArrays"));
    if (!glGenVertexArraysFn) {
        glGenVertexArraysFn =
            reinterpret_cast<PFNGLGENVERTEXARRAYSPROC>(eglGetProcAddress("glGenVertexArraysOES"));
    }
    glBindVertexArrayFn = reinterpret_cast<PFNGLBINDVERTEXARRAYPROC>(eglGetProcAddress("glBindVertexArray"));
    if (!glBindVertexArrayFn) {
        glBindVertexArrayFn =
            reinterpret_cast<PFNGLBINDVERTEXARRAYPROC>(eglGetProcAddress("glBindVertexArrayOES"));
    }
    glDeleteVertexArraysFn =
        reinterpret_cast<PFNGLDELETEVERTEXARRAYSPROC>(eglGetProcAddress("glDeleteVertexArrays"));
    if (!glDeleteVertexArraysFn) {
        glDeleteVertexArraysFn =
            reinterpret_cast<PFNGLDELETEVERTEXARRAYSPROC>(eglGetProcAddress("glDeleteVertexArraysOES"));
    }

    return eglCreateImageKHRFn != nullptr && eglDestroyImageKHRFn != nullptr &&
        glEGLImageTargetTexture2DOESFn != nullptr && glBindTextureFn != nullptr && glGenTexturesFn != nullptr &&
        glDeleteTexturesFn != nullptr;
}

bool YuyvDmaBufGl::initialize()
{
    lastInitFailure_.clear();
    available_ = false;

    initializeOpenGLFunctions();
    if (!resolveExtensions()) {
        lastInitFailure_ = QStringLiteral("missing EGL/GL extensions for dma-buf import");
        return false;
    }

    if (!resolveEglDisplay()) {
        lastInitFailure_ = QStringLiteral(
            "no EGL display (Qt OpenGL may be using GLX; YUYV dma-buf needs an EGL backend)");
        return false;
    }

    const auto *context = QOpenGLContext::currentContext();
    QStringList shaderFailures;
    for (const auto &sources : shaderCandidates(context)) {
        unsigned int vertex = 0;
        unsigned int fragment = 0;
        QString failure;
        if (!compileShader(*this, GL_VERTEX_SHADER, sources.vertex, vertex, failure)) {
            shaderFailures << QStringLiteral("%1 vertex: %2").arg(QString::fromLatin1(sources.label), failure);
            continue;
        }
        if (!compileShader(*this, GL_FRAGMENT_SHADER, sources.fragment, fragment, failure)) {
            glDeleteShader(vertex);
            shaderFailures << QStringLiteral("%1 fragment: %2").arg(QString::fromLatin1(sources.label), failure);
            continue;
        }

        programId_ = glCreateProgram();
        glAttachShader(programId_, vertex);
        glAttachShader(programId_, fragment);
        if (sources.bindAttribLocations) {
            glBindAttribLocation(programId_, 0, "aPos");
            glBindAttribLocation(programId_, 1, "aTexCoord");
        }
        glLinkProgram(programId_);
        GLint linkStatus = GL_FALSE;
        glGetProgramiv(programId_, GL_LINK_STATUS, &linkStatus);
        glDeleteShader(vertex);
        glDeleteShader(fragment);

        if (linkStatus == GL_TRUE) {
            break;
        }

        shaderFailures << QStringLiteral("%1 link: %2")
                              .arg(QString::fromLatin1(sources.label), programLinkLog(*this, programId_));
        glDeleteProgram(programId_);
        programId_ = 0;
    }

    if (programId_ == 0) {
        lastInitFailure_ = QStringLiteral("YUYV shader init failed (%1)").arg(shaderFailures.join(QStringLiteral("; ")));
        return false;
    }

    frameUniform_ = glGetUniformLocation(programId_, "uFrame");
    widthUniform_ = glGetUniformLocation(programId_, "uWidth");
    pairOrderUniform_ = glGetUniformLocation(programId_, "uPairOrder");

    if (programId_ == 0 || frameUniform_ < 0 || widthUniform_ < 0 || pairOrderUniform_ < 0) {
        lastInitFailure_ = QStringLiteral("YUYV shader program setup failed");
        return false;
    }

    // Texture-unit sampler assignment never changes — set it once here so draw() skips it.
    glUseProgram(programId_);
    glUniform1i(frameUniform_, 0);
    glUseProgram(0);

    static constexpr float quadVertices[] = {
        -1.0F, -1.0F, 0.0F, 1.0F,
        1.0F, -1.0F, 1.0F, 1.0F,
        -1.0F, 1.0F, 0.0F, 0.0F,
        1.0F, 1.0F, 1.0F, 0.0F,
    };
    glGenBuffers(1, &vboId_);
    glBindBuffer(GL_ARRAY_BUFFER, vboId_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // Create a VAO to record the VBO binding and attrib layout once, eliminating per-frame redundancy.
    if (glGenVertexArraysFn && glBindVertexArrayFn && glDeleteVertexArraysFn) {
        glGenVertexArraysFn(1, &vaoId_);
        glBindVertexArrayFn(vaoId_);
        glBindBuffer(GL_ARRAY_BUFFER, vboId_);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(
            0, 2, GL_FLOAT, GL_FALSE, static_cast<int>(4 * sizeof(float)), reinterpret_cast<void *>(0));
        glVertexAttribPointer(
            1, 2, GL_FLOAT, GL_FALSE, static_cast<int>(4 * sizeof(float)),
            reinterpret_cast<void *>(static_cast<int>(2 * sizeof(float))));
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindVertexArrayFn(0);
    }

    available_ = true;
    return true;
}

void YuyvDmaBufGl::shutdown()
{
    releaseAllSlots();
    releaseFrame();
    if (vaoId_ != 0 && glDeleteVertexArraysFn) {
        glDeleteVertexArraysFn(1, &vaoId_);
        vaoId_ = 0;
    }
    if (vboId_ != 0) {
        glDeleteBuffers(1, &vboId_);
        vboId_ = 0;
    }
    if (programId_ != 0) {
        glDeleteProgram(programId_);
        programId_ = 0;
    }
    available_ = false;
}

void YuyvDmaBufGl::releaseSlot(const int bufferIndex)
{
    if (bufferIndex < 0 || bufferIndex >= maxBufferSlots) {
        return;
    }

    auto &slot = slots_[static_cast<size_t>(bufferIndex)];
    const auto display = static_cast<EGLDisplay>(eglDisplay_);
    if (slot.eglImage != nullptr && eglDestroyImageKHRFn != nullptr && display != EGL_NO_DISPLAY) {
        eglDestroyImageKHRFn(display, static_cast<EGLImageKHR>(slot.eglImage));
    }
    if (slot.textureId != 0 && glDeleteTexturesFn != nullptr) {
        glDeleteTexturesFn(1, &slot.textureId);
    }
    slot = {};
    if (activeSlot_ == bufferIndex) {
        activeSlot_ = -1;
    }
}

void YuyvDmaBufGl::releaseAllSlots()
{
    for (int index = 0; index < maxBufferSlots; ++index) {
        releaseSlot(index);
    }
}

bool YuyvDmaBufGl::ensureSlotBound(const capture::DmaBufFrameHandle &frame, const int bufferIndex)
{
    auto &slot = slots_[static_cast<size_t>(bufferIndex)];
    if (slot.dmaFd == frame->dmaFd && slot.textureId != 0) {
        return true;
    }

    releaseSlot(bufferIndex);
    lastBindFailure_.clear();

    const auto display = static_cast<EGLDisplay>(eglDisplay_);
    slot.pairOrder = 0;
    slot.eglImage =
        createPlaneImage(display, frame->dmaFd, frame->width, frame->height, frame->stride, DRM_FORMAT_RG88);
    if (slot.eglImage == EGL_NO_IMAGE_KHR) {
        slot.pairOrder = 1;
        slot.eglImage =
            createPlaneImage(display, frame->dmaFd, frame->width, frame->height, frame->stride, DRM_FORMAT_GR88);
    }
    if (slot.eglImage == EGL_NO_IMAGE_KHR) {
        lastBindFailure_ = QStringLiteral("EGL YUYV raw RG88/GR88 dma-buf import failed");
        return false;
    }

    glGenTexturesFn(1, &slot.textureId);
    if (!bindEglImageToTexture(slot.textureId, slot.eglImage)) {
        releaseSlot(bufferIndex);
        lastBindFailure_ = QStringLiteral("EGLImage to GL texture bind failed");
        return false;
    }

    slot.dmaFd = frame->dmaFd;
    return true;
}

bool YuyvDmaBufGl::bindFrame(const capture::DmaBufFrameHandle &frame)
{
    if (!available_ || !frame || frame->dmaFd < 0 || frame->width <= 0 || frame->height <= 0 || frame->stride <= 0) {
        return false;
    }

    if (frame->layout != capture::DmaBufLayout::Yuyv422) {
        return false;
    }

    if (frame->bufferIndex < 0 || frame->bufferIndex >= maxBufferSlots) {
        return false;
    }

    if (!ensureSlotBound(frame, frame->bufferIndex)) {
        return false;
    }

    activeSlot_ = frame->bufferIndex;
    boundWidth_ = static_cast<float>(frame->width);
    boundPairOrder_ = slots_[static_cast<size_t>(frame->bufferIndex)].pairOrder;
    boundFrame_ = frame;
    return true;
}

void YuyvDmaBufGl::releaseFrame()
{
    boundFrame_ = nullptr;
    activeSlot_ = -1;
}

void YuyvDmaBufGl::invalidateSlot(const int bufferIndex)
{
    releaseSlot(bufferIndex);
}

void YuyvDmaBufGl::draw(const QSize &widgetSize, const QRect &targetRect, const float devicePixelRatio)
{
    if (!available_ || activeSlot_ < 0 || programId_ == 0 || targetRect.isEmpty()) {
        return;
    }

    const auto &slot = slots_[static_cast<size_t>(activeSlot_)];
    if (slot.textureId == 0) {
        return;
    }

    const auto dpr = devicePixelRatio > 0.0F ? devicePixelRatio : 1.0F;
    const auto deviceWidth = std::max(1, static_cast<int>(widgetSize.width() * dpr));
    const auto deviceHeight = std::max(1, static_cast<int>(widgetSize.height() * dpr));
    const auto viewportX = static_cast<int>(targetRect.x() * dpr);
    const auto viewportY = static_cast<int>((widgetSize.height() - targetRect.y() - targetRect.height()) * dpr);
    const auto viewportW = std::max(1, static_cast<int>(targetRect.width() * dpr));
    const auto viewportH = std::max(1, static_cast<int>(targetRect.height() * dpr));

    if (viewportX > 0 || viewportY > 0) {
        glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
        glEnable(GL_SCISSOR_TEST);
        if (viewportX > 0) {
            glScissor(0, 0, viewportX, deviceHeight);
            glClear(GL_COLOR_BUFFER_BIT);
            glScissor(viewportX + viewportW, 0, deviceWidth - viewportX - viewportW, deviceHeight);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        if (viewportY > 0) {
            glScissor(0, 0, deviceWidth, viewportY);
            glClear(GL_COLOR_BUFFER_BIT);
            glScissor(0, viewportY + viewportH, deviceWidth, deviceHeight - viewportY - viewportH);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        glDisable(GL_SCISSOR_TEST);
    }
    glViewport(viewportX, viewportY, viewportW, viewportH);

    // Sampler uniform (uFrame=0) was set once at initialize() and never changes.
    // Per-slot uniforms are guarded by dirty flags to skip redundant glUniform calls in steady state.
    glUseProgram(programId_);
    if (boundWidth_ != lastSentWidth_) {
        glUniform1f(widthUniform_, boundWidth_);
        lastSentWidth_ = boundWidth_;
    }
    if (boundPairOrder_ != lastSentPairOrder_) {
        glUniform1i(pairOrderUniform_, boundPairOrder_);
        lastSentPairOrder_ = boundPairOrder_;
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTextureFn(GL_TEXTURE_2D, slot.textureId);

    if (vaoId_ != 0 && glBindVertexArrayFn) {
        // VAO records VBO binding and attrib layout — no per-frame redundant setup.
        glBindVertexArrayFn(vaoId_);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArrayFn(0);
    } else {
        glBindBuffer(GL_ARRAY_BUFFER, vboId_);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(
            0, 2, GL_FLOAT, GL_FALSE, static_cast<int>(4 * sizeof(float)), reinterpret_cast<void *>(0));
        glVertexAttribPointer(
            1, 2, GL_FLOAT, GL_FALSE, static_cast<int>(4 * sizeof(float)),
            reinterpret_cast<void *>(static_cast<int>(2 * sizeof(float))));
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glDisableVertexAttribArray(0);
        glDisableVertexAttribArray(1);
    }

    glBindTextureFn(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}

} // namespace consolation::platform::linux
