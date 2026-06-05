#include "platform/linux/I420DmaBufGl.h"

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

#ifndef DRM_FORMAT_R8
#define DRM_FORMAT_R8 static_cast<EGLint>(0x20203852)
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

    static constexpr char esFragmentShader[] = R"(
        precision highp float;
        varying vec2 vTexCoord;
        uniform sampler2D yTex;
        uniform sampler2D uTex;
        uniform sampler2D vTex;
        void main() {
            float y = max(0.0, texture2D(yTex, vTexCoord).r - 0.0627451) * 1.1643836;
            float u = texture2D(uTex, vTexCoord).r - 0.5;
            float v = texture2D(vTex, vTexCoord).r - 0.5;
            gl_FragColor = vec4(
                y + 1.5960268 * v,
                y - 0.3917623 * u - 0.8129680 * v,
                y + 2.0172321 * u,
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
        uniform sampler2D yTex;
        uniform sampler2D uTex;
        uniform sampler2D vTex;
        out vec4 fragColor;
        void main() {
            float y = max(0.0, texture(yTex, vTexCoord).r - 0.0627451) * 1.1643836;
            float u = texture(uTex, vTexCoord).r - 0.5;
            float v = texture(vTex, vTexCoord).r - 0.5;
            fragColor = vec4(
                y + 1.5960268 * v,
                y - 0.3917623 * u - 0.8129680 * v,
                y + 2.0172321 * u,
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
        uniform sampler2D yTex;
        uniform sampler2D uTex;
        uniform sampler2D vTex;
        void main() {
            float y = max(0.0, texture2D(yTex, vTexCoord).r - 0.0627451) * 1.1643836;
            float u = texture2D(uTex, vTexCoord).r - 0.5;
            float v = texture2D(vTex, vTexCoord).r - 0.5;
            gl_FragColor = vec4(
                y + 1.5960268 * v,
                y - 0.3917623 * u - 0.8129680 * v,
                y + 2.0172321 * u,
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
    const int offset)
{
    const EGLint attribs[] = {
        EGL_WIDTH,
        width,
        EGL_HEIGHT,
        height,
        EGL_LINUX_DRM_FOURCC_EXT,
        DRM_FORMAT_R8,
        EGL_DMA_BUF_PLANE0_FD_EXT,
        dmaFd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,
        offset,
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
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glEGLImageTargetTexture2DOESFn(GL_TEXTURE_2D, static_cast<EGLImageKHR>(eglImage));
    glBindTextureFn(GL_TEXTURE_2D, 0);
    return true;
}

struct PlaneLayout {
    int yOffset = 0;
    int uOffset = 0;
    int vOffset = 0;
    int yStride = 0;
    int chromaStride = 0;
    int chromaWidth = 0;
    int chromaHeight = 0;
};

PlaneLayout computePlaneLayout(const capture::DmaBufFrame &frame)
{
    PlaneLayout layout {};
    layout.yStride = std::max(frame.stride, frame.width);
    layout.chromaStride = (layout.yStride + 1) / 2;
    layout.chromaWidth = std::max(1, (frame.width + 1) / 2);
    layout.chromaHeight = std::max(1, (frame.height + 1) / 2);

    const auto yPlaneBytes = layout.yStride * frame.height;
    const auto chromaPlaneBytes = layout.chromaStride * layout.chromaHeight;

    layout.yOffset = 0;
    if (frame.layout == capture::DmaBufLayout::Yv12) {
        layout.vOffset = yPlaneBytes;
        layout.uOffset = yPlaneBytes + chromaPlaneBytes;
    } else {
        layout.uOffset = yPlaneBytes;
        layout.vOffset = yPlaneBytes + chromaPlaneBytes;
    }

    return layout;
}

} // namespace

bool I420DmaBufGl::resolveEglDisplay()
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

bool I420DmaBufGl::resolveExtensions()
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

bool I420DmaBufGl::initialize()
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
            "no EGL display (Qt OpenGL may be using GLX; I420 dma-buf needs an EGL backend)");
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
        lastInitFailure_ = QStringLiteral("I420 shader init failed (%1)").arg(shaderFailures.join(QStringLiteral("; ")));
        return false;
    }

    yUniform_ = glGetUniformLocation(programId_, "yTex");
    uUniform_ = glGetUniformLocation(programId_, "uTex");
    vUniform_ = glGetUniformLocation(programId_, "vTex");

    if (programId_ == 0 || yUniform_ < 0 || uUniform_ < 0 || vUniform_ < 0) {
        lastInitFailure_ = QStringLiteral("I420 shader program setup failed");
        return false;
    }

    // Texture-unit sampler assignments never change — set them once here so draw() skips them.
    glUseProgram(programId_);
    glUniform1i(yUniform_, 0);
    glUniform1i(uUniform_, 1);
    glUniform1i(vUniform_, 2);
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

void I420DmaBufGl::shutdown()
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

void I420DmaBufGl::releaseSlot(const int bufferIndex)
{
    if (bufferIndex < 0 || bufferIndex >= maxBufferSlots) {
        return;
    }

    auto &slot = slots_[static_cast<size_t>(bufferIndex)];
    const auto display = static_cast<EGLDisplay>(eglDisplay_);
    if (slot.eglImageY != nullptr && eglDestroyImageKHRFn != nullptr && display != EGL_NO_DISPLAY) {
        eglDestroyImageKHRFn(display, static_cast<EGLImageKHR>(slot.eglImageY));
    }
    if (slot.eglImageU != nullptr && eglDestroyImageKHRFn != nullptr && display != EGL_NO_DISPLAY) {
        eglDestroyImageKHRFn(display, static_cast<EGLImageKHR>(slot.eglImageU));
    }
    if (slot.eglImageV != nullptr && eglDestroyImageKHRFn != nullptr && display != EGL_NO_DISPLAY) {
        eglDestroyImageKHRFn(display, static_cast<EGLImageKHR>(slot.eglImageV));
    }
    if (slot.yTextureId != 0 && glDeleteTexturesFn != nullptr) {
        glDeleteTexturesFn(1, &slot.yTextureId);
    }
    if (slot.uTextureId != 0 && glDeleteTexturesFn != nullptr) {
        glDeleteTexturesFn(1, &slot.uTextureId);
    }
    if (slot.vTextureId != 0 && glDeleteTexturesFn != nullptr) {
        glDeleteTexturesFn(1, &slot.vTextureId);
    }
    slot = {};
    if (activeSlot_ == bufferIndex) {
        activeSlot_ = -1;
    }
}

void I420DmaBufGl::releaseAllSlots()
{
    for (int index = 0; index < maxBufferSlots; ++index) {
        releaseSlot(index);
    }
}

bool I420DmaBufGl::ensureSlotBound(const capture::DmaBufFrameHandle &frame, const int bufferIndex)
{
    auto &slot = slots_[static_cast<size_t>(bufferIndex)];
    if (slot.dmaFd == frame->dmaFd && slot.layout == frame->layout && slot.yTextureId != 0 && slot.uTextureId != 0 &&
        slot.vTextureId != 0) {
        return true;
    }

    releaseSlot(bufferIndex);
    lastBindFailure_.clear();

    const auto planes = computePlaneLayout(*frame);
    const auto display = static_cast<EGLDisplay>(eglDisplay_);

    slot.eglImageY = createPlaneImage(
        display, frame->dmaFd, frame->width, frame->height, planes.yStride, planes.yOffset);
    slot.eglImageU = createPlaneImage(
        display,
        frame->dmaFd,
        planes.chromaWidth,
        planes.chromaHeight,
        planes.chromaStride,
        planes.uOffset);
    slot.eglImageV = createPlaneImage(
        display,
        frame->dmaFd,
        planes.chromaWidth,
        planes.chromaHeight,
        planes.chromaStride,
        planes.vOffset);

    if (slot.eglImageY == EGL_NO_IMAGE_KHR || slot.eglImageU == EGL_NO_IMAGE_KHR ||
        slot.eglImageV == EGL_NO_IMAGE_KHR) {
        releaseSlot(bufferIndex);
        lastBindFailure_ = QStringLiteral("EGL I420/YV12 raw R8 dma-buf import failed");
        return false;
    }

    glGenTexturesFn(1, &slot.yTextureId);
    glGenTexturesFn(1, &slot.uTextureId);
    glGenTexturesFn(1, &slot.vTextureId);
    if (!bindEglImageToTexture(slot.yTextureId, slot.eglImageY) ||
        !bindEglImageToTexture(slot.uTextureId, slot.eglImageU) ||
        !bindEglImageToTexture(slot.vTextureId, slot.eglImageV)) {
        releaseSlot(bufferIndex);
        lastBindFailure_ = QStringLiteral("EGLImage to GL texture bind failed");
        return false;
    }

    slot.dmaFd = frame->dmaFd;
    slot.layout = frame->layout;
    return true;
}

bool I420DmaBufGl::bindFrame(const capture::DmaBufFrameHandle &frame)
{
    if (!available_ || !frame || frame->dmaFd < 0 || frame->width <= 0 || frame->height <= 0 || frame->stride <= 0) {
        return false;
    }

    if (frame->layout != capture::DmaBufLayout::I420 && frame->layout != capture::DmaBufLayout::Yv12) {
        return false;
    }

    if (frame->bufferIndex < 0 || frame->bufferIndex >= maxBufferSlots) {
        return false;
    }

    if (!ensureSlotBound(frame, frame->bufferIndex)) {
        return false;
    }

    activeSlot_ = frame->bufferIndex;
    boundFrame_ = frame;
    return true;
}

void I420DmaBufGl::releaseFrame()
{
    boundFrame_ = nullptr;
    activeSlot_ = -1;
}

void I420DmaBufGl::invalidateSlot(const int bufferIndex)
{
    releaseSlot(bufferIndex);
}

void I420DmaBufGl::draw(const QSize &widgetSize, const QRect &targetRect, const float devicePixelRatio)
{
    if (!available_ || activeSlot_ < 0 || programId_ == 0 || targetRect.isEmpty()) {
        return;
    }

    const auto &slot = slots_[static_cast<size_t>(activeSlot_)];
    if (slot.yTextureId == 0 || slot.uTextureId == 0 || slot.vTextureId == 0) {
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

    // Sampler uniforms (yTex=0, uTex=1, vTex=2) were set once at initialize() and never change.
    glUseProgram(programId_);

    glActiveTexture(GL_TEXTURE0);
    glBindTextureFn(GL_TEXTURE_2D, slot.yTextureId);
    glActiveTexture(GL_TEXTURE1);
    glBindTextureFn(GL_TEXTURE_2D, slot.uTextureId);
    glActiveTexture(GL_TEXTURE2);
    glBindTextureFn(GL_TEXTURE_2D, slot.vTextureId);
    glActiveTexture(GL_TEXTURE0);

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
    glActiveTexture(GL_TEXTURE2);
    glBindTextureFn(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTextureFn(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(0);
}

} // namespace consolation::platform::linux
