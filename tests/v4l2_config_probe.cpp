#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTextStream>

#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <numeric>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

int xioctl(const int fd, const unsigned long request, void *arg)
{
    int result = 0;
    do {
        result = ::ioctl(fd, request, arg);
    } while (result == -1 && errno == EINTR);
    return result;
}

quint32 fourCc(const QString &value)
{
    const auto latin = value.toLatin1();
    if (latin.size() < 4) {
        return 0;
    }
    return v4l2_fourcc(latin[0], latin[1], latin[2], latin[3]);
}

QString fourCcToString(const quint32 value)
{
    char chars[4] = {
        static_cast<char>(value & 0xff),
        static_cast<char>((value >> 8) & 0xff),
        static_cast<char>((value >> 16) & 0xff),
        static_cast<char>((value >> 24) & 0xff),
    };
    return QString::fromLatin1(chars, 4).trimmed();
}

v4l2_fract fpsToTimePerFrame(const double fps)
{
    auto numerator = 1000U;
    auto denominator = static_cast<unsigned int>(std::round(fps * 1000.0));
    const auto divisor = std::gcd(numerator, denominator);
    numerator /= divisor;
    denominator /= divisor;
    return v4l2_fract{numerator, denominator};
}

double fpsFromTimePerFrame(const v4l2_fract &value)
{
    if (value.numerator == 0) {
        return 0.0;
    }
    return static_cast<double>(value.denominator) / static_cast<double>(value.numerator);
}

QString errnoText()
{
    return QString::fromLocal8Bit(std::strerror(errno));
}

void logResult(QTextStream &out, const QString &name, const int result)
{
    if (result == 0) {
        out << name << ": ok\n";
        return;
    }
    out << name << ": failed errno=" << errno << " " << errnoText() << "\n";
}

v4l2_format makeFormat(const int width, const int height, const quint32 pixelFormat)
{
    v4l2_format format {};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = static_cast<__u32>(width);
    format.fmt.pix.height = static_cast<__u32>(height);
    format.fmt.pix.pixelformat = pixelFormat;
    format.fmt.pix.field = V4L2_FIELD_NONE;
    return format;
}

void setRequestedFormat(v4l2_format &format, const int width, const int height, const quint32 pixelFormat)
{
    format.fmt.pix.width = static_cast<__u32>(width);
    format.fmt.pix.height = static_cast<__u32>(height);
    format.fmt.pix.pixelformat = pixelFormat;
}

void logFormat(QTextStream &out, const QString &label, const v4l2_format &format)
{
    out << label << ": "
        << format.fmt.pix.width << "x" << format.fmt.pix.height
        << " " << fourCcToString(format.fmt.pix.pixelformat)
        << " field=" << format.fmt.pix.field
        << " bytesperline=" << format.fmt.pix.bytesperline
        << " sizeimage=" << format.fmt.pix.sizeimage << "\n";
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption deviceOption(QStringLiteral("device"), QStringLiteral("V4L2 device path."), QStringLiteral("path"), QStringLiteral("/dev/video2"));
    const QCommandLineOption widthOption(QStringLiteral("width"), QStringLiteral("Requested width."), QStringLiteral("px"), QStringLiteral("1920"));
    const QCommandLineOption heightOption(QStringLiteral("height"), QStringLiteral("Requested height."), QStringLiteral("px"), QStringLiteral("1080"));
    const QCommandLineOption formatOption(QStringLiteral("format"), QStringLiteral("Requested FourCC."), QStringLiteral("fourcc"), QStringLiteral("NV12"));
    const QCommandLineOption fpsOption(QStringLiteral("fps"), QStringLiteral("Requested frame rate."), QStringLiteral("fps"), QStringLiteral("120"));
    parser.addOption(deviceOption);
    parser.addOption(widthOption);
    parser.addOption(heightOption);
    parser.addOption(formatOption);
    parser.addOption(fpsOption);
    parser.process(app);

    QTextStream out(stdout);
    const auto device = parser.value(deviceOption);
    const auto width = parser.value(widthOption).toInt();
    const auto height = parser.value(heightOption).toInt();
    const auto pixelFormat = fourCc(parser.value(formatOption));
    const auto fps = parser.value(fpsOption).toDouble();

    out << "open " << device << "\n";
    const int fd = ::open(device.toLocal8Bit().constData(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        out << "open: failed errno=" << errno << " " << errnoText() << "\n";
        return 2;
    }

    v4l2_capability cap {};
    auto result = xioctl(fd, VIDIOC_QUERYCAP, &cap);
    logResult(out, QStringLiteral("VIDIOC_QUERYCAP"), result);
    if (result == 0) {
        out << "  driver=" << reinterpret_cast<const char *>(cap.driver)
            << " card=" << reinterpret_cast<const char *>(cap.card)
            << " bus=" << reinterpret_cast<const char *>(cap.bus_info)
            << " capabilities=0x" << Qt::hex << cap.capabilities
            << " device_caps=0x" << cap.device_caps << Qt::dec << "\n";
    }

    int input = 0;
    result = xioctl(fd, VIDIOC_G_INPUT, &input);
    logResult(out, QStringLiteral("VIDIOC_G_INPUT"), result);
    if (result == 0) {
        out << "  input=" << input << "\n";
    }

    v4l2_format current {};
    current.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    result = xioctl(fd, VIDIOC_G_FMT, &current);
    logResult(out, QStringLiteral("VIDIOC_G_FMT"), result);
    if (result == 0) {
        logFormat(out, QStringLiteral("  current"), current);
    }

    auto tryFormat = current;
    setRequestedFormat(tryFormat, width, height, pixelFormat);
    result = xioctl(fd, VIDIOC_TRY_FMT, &tryFormat);
    logResult(out, QStringLiteral("VIDIOC_TRY_FMT"), result);
    if (result == 0) {
        logFormat(out, QStringLiteral("  try"), tryFormat);
    }

    v4l2_streamparm streamParm {};
    streamParm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    result = xioctl(fd, VIDIOC_G_PARM, &streamParm);
    logResult(out, QStringLiteral("VIDIOC_G_PARM"), result);
    if (result == 0) {
        out << "  current fps=" << fpsFromTimePerFrame(streamParm.parm.capture.timeperframe) << "\n";
    }

    streamParm = {};
    streamParm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    streamParm.parm.capture.timeperframe = fpsToTimePerFrame(fps);
    result = xioctl(fd, VIDIOC_S_PARM, &streamParm);
    logResult(out, QStringLiteral("VIDIOC_S_PARM before S_FMT"), result);
    if (result == 0) {
        out << "  accepted fps=" << fpsFromTimePerFrame(streamParm.parm.capture.timeperframe) << "\n";
    }

    auto requested = current;
    setRequestedFormat(requested, width, height, pixelFormat);
    result = xioctl(fd, VIDIOC_S_FMT, &requested);
    logResult(out, QStringLiteral("VIDIOC_S_FMT"), result);
    if (result == 0) {
        logFormat(out, QStringLiteral("  accepted"), requested);
    }

    streamParm = {};
    streamParm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    streamParm.parm.capture.timeperframe = fpsToTimePerFrame(fps);
    result = xioctl(fd, VIDIOC_S_PARM, &streamParm);
    logResult(out, QStringLiteral("VIDIOC_S_PARM after S_FMT"), result);
    if (result == 0) {
        out << "  accepted fps=" << fpsFromTimePerFrame(streamParm.parm.capture.timeperframe) << "\n";
    }

    ::close(fd);
    return 0;
}
