#include "platform/linux/YuyvDmaBufGl.h"

#include <QOpenGLContext>
#include <QRect>

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

#ifndef DRM_FORMAT_YUYV
#define DRM_FORMAT_YUYV static_cast<EGLint>(0x56595559)
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

struct ShaderSources {
    const char *vertex = nullptr;
    const char *fragment = nullptr;
    bool bindAttribLocations = false;
};

ShaderSources pickShaderSources(const QOpenGLContext *context)
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

    // BT.601 full-range; macropixel (Y0,U,Y1,V) in (r,g,b,a) per Mesa-style YUYV import.
    static constexpr char esFragmentShader[] = R"(
        precision mediump float;
        varying vec2 vTexCoord;
        uniform sampler2D uFrame;
        uniform float uWidth;
        void main() {
            vec2 tc = vTexCoord;
            vec4 c = texture2D(uFrame, tc);
            float x = floor(tc.x * uWidth);
            float y = (mod(x, 2.0) < 1.0) ? c.r : c.b;
            float u = c.g - 0.5;
            float v = c.a - 0.5;
            float r = y + 1.402 * v;
            float g = y - 0.344 * u - 0.714 * v;
            float b = y + 1.772 * u;
            gl_FragColor = vec4(r, g, b, 1.0);
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
        out vec4 fragColor;
        void main() {
            vec2 tc = vTexCoord;
            vec4 c = texture(uFrame, tc);
            float x = floor(tc.x * uWidth);
            float y = (mod(x, 2.0) < 1.0) ? c.r : c.b;
            float u = c.g - 0.5;
            float v = c.a - 0.5;
            float r = y + 1.402 * v;
            float g = y - 0.344 * u - 0.714 * v;
            float b = y + 1.772 * u;
            fragColor = vec4(r, g, b, 1.0);
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
        void main() {
            vec2 tc = vTexCoord;
            vec4 c = texture2D(uFrame, tc);
            float x = floor(tc.x * uWidth);
            float y = (mod(x, 2.0) < 1.0) ? c.r : c.b;
            float u = c.g - 0.5;
            float v = c.a - 0.5;
            float r = y + 1.402 * v;
            float g = y - 0.344 * u - 0.714 * v;
            float b = y + 1.772 * u;
            gl_FragColor = vec4(r, g, b, 1.0);
        }
    )";

    if (context != nullptr && context->isOpenGLES()) {
        return {esVertexShader, esFragmentShader, true};
    }

    if (context != nullptr && context->format().majorVersion() >= 3) {
        return {gl330VertexShader, gl330FragmentShader, false};
    }

    return {gl120VertexShader, gl120FragmentShader, true};
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
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
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
    const auto sources = pickShaderSources(context);

    unsigned int vertex = 0;
    unsigned int fragment = 0;
    if (!compileShader(*this, GL_VERTEX_SHADER, sources.vertex, vertex, lastInitFailure_)) {
        return false;
    }
    if (!compileShader(*this, GL_FRAGMENT_SHADER, sources.fragment, fragment, lastInitFailure_)) {
        glDeleteShader(vertex);
        return false;
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

    if (linkStatus != GL_TRUE) {
        lastInitFailure_ =
            QStringLiteral("YUYV shader program link failed: %1").arg(programLinkLog(*this, programId_));
        glDeleteProgram(programId_);
        programId_ = 0;
        return false;
    }

    frameUniform_ = glGetUniformLocation(programId_, "uFrame");
    widthUniform_ = glGetUniformLocation(programId_, "uWidth");

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

    if (programId_ == 0 || frameUniform_ < 0 || widthUniform_ < 0) {
        lastInitFailure_ = QStringLiteral("YUYV shader program setup failed");
        return false;
    }

    available_ = true;
    return true;
}

void YuyvDmaBufGl::shutdown()
{
    releaseAllSlots();
    releaseFrame();
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

    const auto display = static_cast<EGLDisplay>(eglDisplay_);
    slot.eglImage =
        createPlaneImage(display, frame->dmaFd, frame->width, frame->height, frame->stride, DRM_FORMAT_YUYV);
    if (slot.eglImage == EGL_NO_IMAGE_KHR) {
        return false;
    }

    glGenTexturesFn(1, &slot.textureId);
    if (!bindEglImageToTexture(slot.textureId, slot.eglImage)) {
        releaseSlot(bufferIndex);
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
    boundFrame_ = frame;
    return true;
}

void YuyvDmaBufGl::releaseFrame()
{
    boundFrame_ = nullptr;
    activeSlot_ = -1;
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

    glViewport(0, 0, deviceWidth, deviceHeight);
    glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(viewportX, viewportY, viewportW, viewportH);

    glUseProgram(programId_);
    glUniform1i(frameUniform_, 0);
    glUniform1f(widthUniform_, boundWidth_);

    glActiveTexture(GL_TEXTURE0);
    glBindTextureFn(GL_TEXTURE_2D, slot.textureId);

    glBindBuffer(GL_ARRAY_BUFFER, vboId_);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, static_cast<int>(4 * sizeof(float)), reinterpret_cast<void *>(0));
    glVertexAttribPointer(
        1,
        2,
        GL_FLOAT,
        GL_FALSE,
        static_cast<int>(4 * sizeof(float)),
        reinterpret_cast<void *>(static_cast<int>(2 * sizeof(float))));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);

    glBindTextureFn(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}

} // namespace consolation::platform::linux
