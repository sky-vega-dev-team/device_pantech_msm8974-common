// camtest: open a camera with the given streams and count the frames of each.
// usage: camtest <camera id> <seconds> <WxH:fmt> [<WxH:fmt> ...]
//   fmt: yuv (YUV_420_888), jpeg, priv (PRIVATE, needs no CPU)
#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCaptureRequest.h>
#include <camera/NdkCameraMetadata.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaMuxer.h>
#include <fcntl.h>
#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <thread>
#include <android/hardware_buffer.h>

#include <android/binder_process.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <vector>

struct Stream {
    int w, h, fmt;
    bool video = false;
    const char *name;
    AImageReader *reader = nullptr;
    ANativeWindow *window = nullptr;
    ACaptureSessionOutput *output = nullptr;
    ACameraOutputTarget *target = nullptr;
    std::atomic<int> frames{0};
};

static std::atomic<int> g_results{0}, g_failed{0};

/* Read a camera buffer back through the GPU, as RGBA */
static void gpuDump(AHardwareBuffer *hb, int w, int h, const char *path) {
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(dpy, nullptr, nullptr);
    EGLint ca[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                   EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig cfg; EGLint n;
    eglChooseConfig(dpy, ca, &cfg, 1, &n);
    EGLint pa[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pa);
    EGLint xa[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, xa);
    eglMakeCurrent(dpy, surf, surf, ctx);
    EGLClientBuffer cb = eglGetNativeClientBufferANDROID(hb);
    EGLint ia[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    EGLImageKHR img = eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, cb, ia);
    if (img == EGL_NO_IMAGE_KHR) { printf("no egl image %#x\n", eglGetError()); return; }
    GLuint tex, out, fbo;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, img);
    glGenTextures(1, &out);
    glBindTexture(GL_TEXTURE_2D, out);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, out, 0);
    const char *vs = "attribute vec2 p; varying vec2 uv; void main() {"
                     " uv = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }";
    const char *fs = "#extension GL_OES_EGL_image_external : require\n"
                     "precision mediump float; varying vec2 uv; uniform samplerExternalOES t;"
                     " void main() { gl_FragColor = texture2D(t, uv); }";
    GLuint prog = glCreateProgram();
    for (int k = 0; k < 2; k++) {
        GLuint sh = glCreateShader(k ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
        const char *src = k ? fs : vs;
        glShaderSource(sh, 1, &src, nullptr);
        glCompileShader(sh);
        glAttachShader(prog, sh);
    }
    glBindAttribLocation(prog, 0, "p");
    glLinkProgram(prog);
    glUseProgram(prog);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
    static const float q[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, q);
    glEnableVertexAttribArray(0);
    glViewport(0, 0, w, h);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    std::vector<uint8_t> px(w * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(px.data(), 1, px.size(), f); fclose(f); }
    printf("gpu dump gl error %#x\n", glGetError());
    eglDestroyImageKHR(dpy, img);
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglDestroySurface(dpy, surf);
}

static void onImage(void *ctx, AImageReader *reader) {
    Stream *s = (Stream *)ctx;
    AImage *img = nullptr;
    if ((getenv("LUMA") ? AImageReader_acquireNextImage(reader, &img)
                        : AImageReader_acquireLatestImage(reader, &img)) == AMEDIA_OK && img) {
        int n = s->frames++;
        /* LUMA: print the mean of the Y plane of the first YUV frames */
        if (getenv("LUMA") && s->fmt == AIMAGE_FORMAT_YUV_420_888 && n >= 20 && n < 40) {
            uint8_t *y = nullptr; int len = 0, stride = 0, w = 0, h = 0;
            int64_t ts = 0;
            AImage_getPlaneData(img, 0, &y, &len);
            AImage_getPlaneRowStride(img, 0, &stride);
            AImage_getWidth(img, &w); AImage_getHeight(img, &h);
            AImage_getTimestamp(img, &ts);
            long sum = 0, cnt = 0;
            for (int r = 0; r < h; r += 16)
                for (int c = 0; c < w; c += 16) { sum += y[r * stride + c]; cnt++; }
            printf("frame %d ts %lld luma %.1f\n", n, (long long)(ts / 1000000), (double)sum / cnt);
        }
        if (n == 60 && s->fmt == AIMAGE_FORMAT_YUV_420_888 && getenv("DUMP")) {
            /* Y plane, then the interleaved chroma as NV12/NV21 */
            FILE *f = fopen(getenv("DUMP"), "wb");
            uint8_t *d; int len, rs, ps;
            for (int p = 0; f && p < 2; p++) {
                AImage_getPlaneData(img, p, &d, &len);
                AImage_getPlaneRowStride(img, p, &rs);
                AImage_getPlanePixelStride(img, p, &ps);
                int rows = p ? s->h / 2 : s->h;
                for (int y = 0; y < rows; y++) fwrite(d + y * rs, 1, s->w, f);
                if (p) printf("dumped, chroma pixel stride %d\n", ps);
            }
            if (f) fclose(f);
        } else if (n == 60 && s->fmt == AIMAGE_FORMAT_PRIVATE && !s->video && getenv("DUMP")) {
            AHardwareBuffer *hb = nullptr;
            if (AImage_getHardwareBuffer(img, &hb) == AMEDIA_OK && hb)
                gpuDump(hb, s->w, s->h, getenv("DUMP"));
        }
        AImage_delete(img);
    }
}

static void onDisconnected(void *, ACameraDevice *) { printf("device disconnected\n"); }
static void onError(void *, ACameraDevice *, int e) { printf("device error %d\n", e); }
static void onClosed(void *, ACameraCaptureSession *) {}
static void onReady(void *, ACameraCaptureSession *) {}
static void onActive(void *, ACameraCaptureSession *) {}
static int64_t g_last_ts = 0;
static std::atomic<int64_t> g_exp{0};
static std::atomic<int32_t> g_iso{0};
static void onCompleted(void *, ACameraCaptureSession *, ACaptureRequest *,
                        const ACameraMetadata *md) {
    ACameraMetadata_const_entry e;
    int n = g_results++;
    if (ACameraMetadata_getConstEntry(md, ACAMERA_SENSOR_EXPOSURE_TIME, &e) == ACAMERA_OK) g_exp = e.data.i64[0];
    if (ACameraMetadata_getConstEntry(md, ACAMERA_SENSOR_SENSITIVITY, &e) == ACAMERA_OK) g_iso = e.data.i32[0];
    if (getenv("LUMA") && n >= 18 && n < 42 &&
            ACameraMetadata_getConstEntry(md, ACAMERA_SENSOR_TIMESTAMP, &e) == ACAMERA_OK) {
        int64_t ts = e.data.i64[0], ex = 0;
        if (ACameraMetadata_getConstEntry(md, ACAMERA_SENSOR_EXPOSURE_TIME, &e) == ACAMERA_OK) ex = e.data.i64[0];
        printf("result %d ts %lld exposure %.1f ms\n", n, (long long)(ts / 1000000), ex / 1e6);
    }
    if (n < 12 && ACameraMetadata_getConstEntry(md, ACAMERA_SENSOR_TIMESTAMP, &e) == ACAMERA_OK) {
        int64_t ts = e.data.i64[0];
        int64_t fd = 0, ex = 0;
        if (ACameraMetadata_getConstEntry(md, ACAMERA_SENSOR_FRAME_DURATION, &e) == ACAMERA_OK) fd = e.data.i64[0];
        if (ACameraMetadata_getConstEntry(md, ACAMERA_SENSOR_EXPOSURE_TIME, &e) == ACAMERA_OK) ex = e.data.i64[0];
        printf("result %d ts delta %.1f ms, frameDuration %.1f ms, exposure %.1f ms\n", n,
               g_last_ts ? (ts - g_last_ts) / 1e6 : 0.0, fd / 1e6, ex / 1e6);
        g_last_ts = ts;
    }
}
static void onFailed(void *, ACameraCaptureSession *, ACaptureRequest *,
                     ACameraCaptureFailure *f) {
    if (g_failed++ < 5) printf("capture failed reason %d frame %lld\n", f->reason, (long long)f->frameNumber);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <id> <seconds> <WxH:yuv|jpeg|priv>...\n", argv[0]);
        return 1;
    }
    ABinderProcess_setThreadPoolMaxThreadCount(4);
    ABinderProcess_startThreadPool();
    const char *id = argv[1];
    int secs = atoi(argv[2]);
    std::vector<Stream *> streams;
    for (int i = 3; i < argc; i++) {
        Stream *s = new Stream();
        char f[16] = {0};
        if (sscanf(argv[i], "%dx%d:%15s", &s->w, &s->h, f) != 3) return 1;
        if (!strcmp(f, "yuv")) s->fmt = AIMAGE_FORMAT_YUV_420_888;
        else if (!strcmp(f, "jpeg")) s->fmt = AIMAGE_FORMAT_JPEG;
        else if (!strcmp(f, "video")) { s->fmt = AIMAGE_FORMAT_PRIVATE; s->video = true; }
        else s->fmt = AIMAGE_FORMAT_PRIVATE;
        s->name = argv[i];
        streams.push_back(s);
    }

    ACameraManager *mgr = ACameraManager_create();
    ACameraDevice_StateCallbacks devCb = {nullptr, onDisconnected, onError};
    ACameraDevice *dev = nullptr;
    camera_status_t rc = ACameraManager_openCamera(mgr, id, &devCb, &dev);
    if (rc != ACAMERA_OK) { printf("openCamera %d\n", rc); return 1; }

    ACaptureSessionOutputContainer *outs = nullptr;
    ACaptureSessionOutputContainer_create(&outs);
    ACaptureRequest *req = nullptr;
    ACameraDevice_createCaptureRequest(dev, TEMPLATE_PREVIEW, &req);
    if (getenv("AFOFF")) {
        uint8_t af = ACAMERA_CONTROL_AF_MODE_OFF;
        ACaptureRequest_setEntry_u8(req, ACAMERA_CONTROL_AF_MODE, 1, &af);
        printf("AF off\n");
    }
    if (getenv("ZOOM")) {
        float z = atof(getenv("ZOOM"));
        ACameraMetadata *ch = nullptr;
        ACameraMetadata_const_entry e;
        ACameraManager_getCameraCharacteristics(mgr, id, &ch);
        if (ACameraMetadata_getConstEntry(ch, ACAMERA_SENSOR_INFO_ACTIVE_ARRAY_SIZE, &e) == ACAMERA_OK) {
            int32_t w = e.data.i32[2] - e.data.i32[0], h = e.data.i32[3] - e.data.i32[1];
            int32_t cw = w / z, chh = h / z;
            int32_t crop[4] = {(w - cw) / 2, (h - chh) / 2, cw, chh};
            ACaptureRequest_setEntry_i32(req, ACAMERA_SCALER_CROP_REGION, 4, crop);
            printf("crop %d,%d %dx%d\n", crop[0], crop[1], crop[2], crop[3]);
        }
    }
    if (getenv("EXP")) {
        /* manual exposure in ms at ISO 800, 33 ms frames */
        uint8_t ae = ACAMERA_CONTROL_AE_MODE_OFF;
        int64_t exp = (int64_t)(atof(getenv("EXP")) * 1e6), fd = 33333333;
        int32_t iso = 800;
        ACaptureRequest_setEntry_u8(req, ACAMERA_CONTROL_AE_MODE, 1, &ae);
        ACaptureRequest_setEntry_i64(req, ACAMERA_SENSOR_EXPOSURE_TIME, 1, &exp);
        ACaptureRequest_setEntry_i64(req, ACAMERA_SENSOR_FRAME_DURATION, 1, &fd);
        ACaptureRequest_setEntry_i32(req, ACAMERA_SENSOR_SENSITIVITY, 1, &iso);
        printf("manual exposure %lld ns\n", (long long)exp);
    }
    if (getenv("FPS")) {
        int32_t r[2] = {0, 0};
        sscanf(getenv("FPS"), "%d,%d", &r[0], &r[1]);
        if (!r[1]) r[1] = r[0];
        ACaptureRequest_setEntry_i32(req, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2, r);
        printf("fps range %d-%d\n", r[0], r[1]);
    }
    if (getenv("ROI")) {
        /* x,y,w,h in active array coordinates, for both AE and AF */
        int32_t r[5] = {0, 0, 0, 0, 1000};
        sscanf(getenv("ROI"), "%d,%d,%d,%d", &r[0], &r[1], &r[2], &r[3]);
        r[2] += r[0];
        r[3] += r[1];
        ACaptureRequest_setEntry_i32(req, ACAMERA_CONTROL_AE_REGIONS, 5, r);
        ACaptureRequest_setEntry_i32(req, ACAMERA_CONTROL_AF_REGIONS, 5, r);
        printf("roi %d,%d-%d,%d\n", r[0], r[1], r[2], r[3]);
    }
    /* REC=<path>: record the video stream to an mp4 file instead */
    AMediaCodec *codec = nullptr;
    AMediaMuxer *muxer = nullptr;
    std::atomic<bool> recording{false};
    std::thread drain;
    for (Stream *s : streams) {
        if (!s->video || !getenv("REC")) continue;
        const char *fr = getenv("FPS") ? getenv("FPS") : "30";
        AMediaFormat *fmt = AMediaFormat_new();
        AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/avc");
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, s->w);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, s->h);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_COLOR_FORMAT, 0x7f000789);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_BIT_RATE, 24000000);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_FRAME_RATE, atoi(fr));
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
        codec = AMediaCodec_createCodecByName(getenv("CODEC") ? getenv("CODEC") : "OMX.qcom.video.encoder.avc");
        media_status_t cr = AMEDIA_OK;
        if (!codec || (cr = AMediaCodec_configure(codec, fmt, nullptr, nullptr,
                AMEDIACODEC_CONFIGURE_FLAG_ENCODE)) != AMEDIA_OK ||
                (cr = AMediaCodec_createInputSurface(codec, &s->window)) != AMEDIA_OK) {
            printf("encoder failed: codec %p status %d\n", codec, cr); return 1;
        }
        AMediaFormat_delete(fmt);
        int fd = open(getenv("REC"), O_CREAT | O_TRUNC | O_RDWR, 0644);
        muxer = AMediaMuxer_new(fd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
        AMediaCodec_start(codec);
        recording = true;
        drain = std::thread([&, s] {
            ssize_t track = -1;
            int encoded = 0;
            while (true) {
                AMediaCodecBufferInfo bi;
                ssize_t i = AMediaCodec_dequeueOutputBuffer(codec, &bi, 100000);
                if (i == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                    AMediaFormat *of = AMediaCodec_getOutputFormat(codec);
                    track = AMediaMuxer_addTrack(muxer, of);
                    AMediaFormat_delete(of);
                    AMediaMuxer_start(muxer);
                } else if (i >= 0) {
                    size_t sz;
                    uint8_t *buf = AMediaCodec_getOutputBuffer(codec, i, &sz);
                    if (track >= 0 && bi.size && !(bi.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG)) {
                        AMediaMuxer_writeSampleData(muxer, track, buf, &bi);
                        s->frames = ++encoded;
                    }
                    AMediaCodec_releaseOutputBuffer(codec, i, false);
                    if (bi.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) break;
                } else if (!recording) {
                    break;
                }
            }
        });
        ACaptureSessionOutput_create(s->window, &s->output);
        ACaptureSessionOutputContainer_add(outs, s->output);
        ACameraOutputTarget_create(s->window, &s->target);
        ACaptureRequest_addTarget(req, s->target);
    }
    for (Stream *s : streams) {
        if (s->output) continue;
        media_status_t mr = s->fmt == AIMAGE_FORMAT_PRIVATE ?
            AImageReader_newWithUsage(s->w, s->h, s->fmt,
                s->video ? AHARDWAREBUFFER_USAGE_VIDEO_ENCODE :
                AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 8, &s->reader) :
            AImageReader_new(s->w, s->h, s->fmt, 8, &s->reader);
        if (mr != AMEDIA_OK) {
            printf("reader %s failed\n", s->name); return 1;
        }
        AImageReader_ImageListener l = {s, onImage};
        AImageReader_setImageListener(s->reader, &l);
        AImageReader_getWindow(s->reader, &s->window);
        ACaptureSessionOutput_create(s->window, &s->output);
        ACaptureSessionOutputContainer_add(outs, s->output);
        ACameraOutputTarget_create(s->window, &s->target);
        /* JPEGIDLE: configure the JPEG streams but leave them out of the requests */
        if (!(s->fmt == AIMAGE_FORMAT_JPEG && getenv("JPEGIDLE")))
            ACaptureRequest_addTarget(req, s->target);
    }

    ACameraCaptureSession_stateCallbacks sCb = {nullptr, onClosed, onReady, onActive};
    ACameraCaptureSession *sess = nullptr;
    rc = ACameraDevice_createCaptureSession(dev, outs, &sCb, &sess);
    if (rc != ACAMERA_OK) { printf("createCaptureSession %d\n", rc); return 1; }

    ACameraCaptureSession_captureCallbacks cCb = {};
    cCb.onCaptureCompleted = onCompleted;
    cCb.onCaptureFailed = onFailed;
    /* EXPALT=a,b: repeat a burst of two requests with these manual exposures in ms */
    ACaptureRequest *reqs[2] = {req, nullptr};
    int nreq = 1;
    if (getenv("EXPALT")) {
        double a = 0, b = 0;
        sscanf(getenv("EXPALT"), "%lf,%lf", &a, &b);
        uint8_t ae = ACAMERA_CONTROL_AE_MODE_OFF;
        int64_t fd = 33333333;
        int32_t iso = 400;
        reqs[1] = ACaptureRequest_copy(req);
        for (int i = 0; i < 2; i++) {
            int64_t exp = (int64_t)((i ? b : a) * 1e6);
            ACaptureRequest_setEntry_u8(reqs[i], ACAMERA_CONTROL_AE_MODE, 1, &ae);
            ACaptureRequest_setEntry_i64(reqs[i], ACAMERA_SENSOR_EXPOSURE_TIME, 1, &exp);
            ACaptureRequest_setEntry_i64(reqs[i], ACAMERA_SENSOR_FRAME_DURATION, 1, &fd);
            ACaptureRequest_setEntry_i32(reqs[i], ACAMERA_SENSOR_SENSITIVITY, 1, &iso);
        }
        nreq = 2;
        printf("alternating exposure %.1f / %.1f ms\n", a, b);
    }
    rc = ACameraCaptureSession_setRepeatingRequest(sess, &cCb, nreq, reqs, nullptr);
    if (rc != ACAMERA_OK) { printf("setRepeatingRequest %d\n", rc); return 1; }

    for (int t = 1; t <= secs; t++) {
        sleep(1);
        printf("t=%ds results=%d failed=%d", t, g_results.load(), g_failed.load());
        for (Stream *s : streams) printf(" [%s %d]", s->name, s->frames.load());
        printf("\n");
        fflush(stdout);
    }
    printf("last exposure %.2f ms iso %d\n", g_exp.load() / 1e6, g_iso.load());
    ACameraCaptureSession_stopRepeating(sess);
    ACameraCaptureSession_close(sess);
    if (codec) {
        AMediaCodec_signalEndOfInputStream(codec);
        drain.join();
        AMediaMuxer_stop(muxer);
        AMediaMuxer_delete(muxer);
        AMediaCodec_stop(codec);
        AMediaCodec_delete(codec);
        printf("recorded\n");
    }
    ACameraDevice_close(dev);
    ACameraManager_delete(mgr);
    return 0;
}
