#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#define LOG_TAG "MinimalGlobe"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ============================================================
// EGL / GL ES 3.0 上下文初始化
// ============================================================

static EGLDisplay gDisplay = EGL_NO_DISPLAY;
static EGLSurface gSurface = EGL_NO_SURFACE;
static EGLContext gContext = EGL_NO_CONTEXT;
static ANativeWindow* gWindow = nullptr;
static int gWidth = 0, gHeight = 0;
static GLuint gProgram = 0;
static GLuint gVao = 0;
static GLuint gVertexBuffer = 0;
static GLuint gIndexBuffer = 0;
static GLsizei gIndexCount = 0;
static GLint gMvpLocation = -1;
static GLint gModelLocation = -1;
static GLint gLightDirLocation = -1;
static std::chrono::steady_clock::time_point gStartTime;
static std::chrono::steady_clock::time_point gLastFrameTime;
static std::chrono::steady_clock::time_point gLastDragTime;
static glm::quat gGlobeRotation(1.0f, 0.0f, 0.0f, 0.0f);
static float gCameraDistance = 7.0f;
static float gCameraTilt = 0.0f;
static glm::vec3 gInertiaAxis(0.0f, 1.0f, 0.0f);
static float gInertiaAngularVelocity = 0.0f;
static bool gTouching = false;

struct GlobeVertex {
    float position[3];
    float normal[3];
    float texcoord[2];
};

static GLuint compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLint infoLen = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLen);
        std::vector<char> log(static_cast<size_t>(infoLen) + 1);
        glGetShaderInfoLog(shader, infoLen, nullptr, log.data());
        LOGE("Shader compile failed: %s", log.data());
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static bool createGlobeProgram() {
    static const char* kVertexShader = R"glsl(
        #version 300 es
        layout(location = 0) in vec3 a_position;
        layout(location = 1) in vec3 a_normal;
        layout(location = 2) in vec2 a_texcoord;

        uniform mat4 u_modelViewProjection;
        uniform mat4 u_model;

        out vec3 v_normal;
        out vec2 v_texcoord;

        void main() {
            v_normal = normalize(mat3(u_model) * a_normal);
            v_texcoord = a_texcoord;
            gl_Position = u_modelViewProjection * vec4(a_position, 1.0);
        }
    )glsl";

    static const char* kFragmentShader = R"glsl(
        #version 300 es
        precision mediump float;

        in vec3 v_normal;
        in vec2 v_texcoord;

        uniform vec3 u_lightDir;

        out vec4 fragColor;

        void main() {
            vec3 n = normalize(v_normal);
            float diffuse = max(dot(n, normalize(u_lightDir)), 0.0);
            vec3 ocean = vec3(0.05, 0.26, 0.58);
            vec3 land = vec3(0.18, 0.48, 0.24);
            float band = smoothstep(0.42, 0.58, sin(v_texcoord.x * 37.0) * 0.5 + sin(v_texcoord.y * 23.0) * 0.5 + 0.5);
            vec3 base = mix(ocean, land, band * 0.45);
            vec3 color = base * (0.22 + diffuse * 0.88);
            fragColor = vec4(color, 1.0);
        }
    )glsl";

    GLuint vertexShader = compileShader(GL_VERTEX_SHADER, kVertexShader);
    GLuint fragmentShader = compileShader(GL_FRAGMENT_SHADER, kFragmentShader);
    if (!vertexShader || !fragmentShader) {
        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);
        return false;
    }

    gProgram = glCreateProgram();
    glAttachShader(gProgram, vertexShader);
    glAttachShader(gProgram, fragmentShader);
    glLinkProgram(gProgram);
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    GLint linked = GL_FALSE;
    glGetProgramiv(gProgram, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint infoLen = 0;
        glGetProgramiv(gProgram, GL_INFO_LOG_LENGTH, &infoLen);
        std::vector<char> log(static_cast<size_t>(infoLen) + 1);
        glGetProgramInfoLog(gProgram, infoLen, nullptr, log.data());
        LOGE("Program link failed: %s", log.data());
        glDeleteProgram(gProgram);
        gProgram = 0;
        return false;
    }

    gMvpLocation = glGetUniformLocation(gProgram, "u_modelViewProjection");
    gModelLocation = glGetUniformLocation(gProgram, "u_model");
    gLightDirLocation = glGetUniformLocation(gProgram, "u_lightDir");
    return true;
}

static bool createGlobeMesh() {
    constexpr int kLongitudeSegments = 96;
    constexpr int kLatitudeSegments = 48;
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kPolarRadiusRatio = 6356752.314245f / 6378137.0f;

    std::vector<GlobeVertex> vertices;
    std::vector<uint32_t> indices;
    vertices.reserve(static_cast<size_t>((kLongitudeSegments + 1) * (kLatitudeSegments + 1)));
    indices.reserve(static_cast<size_t>(kLongitudeSegments * kLatitudeSegments * 6));

    for (int lat = 0; lat <= kLatitudeSegments; ++lat) {
        float v = static_cast<float>(lat) / static_cast<float>(kLatitudeSegments);
        float phi = -0.5f * kPi + v * kPi;
        float cosPhi = std::cos(phi);
        float sinPhi = std::sin(phi);

        for (int lon = 0; lon <= kLongitudeSegments; ++lon) {
            float u = static_cast<float>(lon) / static_cast<float>(kLongitudeSegments);
            float theta = u * 2.0f * kPi;
            float cosTheta = std::cos(theta);
            float sinTheta = std::sin(theta);

            GlobeVertex vertex{};
            vertex.position[0] = cosPhi * cosTheta;
            vertex.position[1] = kPolarRadiusRatio * sinPhi;
            vertex.position[2] = cosPhi * sinTheta;

            glm::vec3 normal(vertex.position[0], vertex.position[1] / (kPolarRadiusRatio * kPolarRadiusRatio), vertex.position[2]);
            normal = glm::normalize(normal);
            vertex.normal[0] = normal.x;
            vertex.normal[1] = normal.y;
            vertex.normal[2] = normal.z;
            vertex.texcoord[0] = u;
            vertex.texcoord[1] = v;
            vertices.push_back(vertex);
        }
    }

    for (int lat = 0; lat < kLatitudeSegments; ++lat) {
        for (int lon = 0; lon < kLongitudeSegments; ++lon) {
            uint32_t row0 = static_cast<uint32_t>(lat * (kLongitudeSegments + 1));
            uint32_t row1 = static_cast<uint32_t>((lat + 1) * (kLongitudeSegments + 1));
            uint32_t a = row0 + static_cast<uint32_t>(lon);
            uint32_t b = row0 + static_cast<uint32_t>(lon + 1);
            uint32_t c = row1 + static_cast<uint32_t>(lon);
            uint32_t d = row1 + static_cast<uint32_t>(lon + 1);
            indices.push_back(a);
            indices.push_back(c);
            indices.push_back(b);
            indices.push_back(b);
            indices.push_back(c);
            indices.push_back(d);
        }
    }

    glGenVertexArrays(1, &gVao);
    glBindVertexArray(gVao);

    glGenBuffers(1, &gVertexBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, gVertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(GlobeVertex)), vertices.data(), GL_STATIC_DRAW);

    glGenBuffers(1, &gIndexBuffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gIndexBuffer);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indices.size() * sizeof(uint32_t)), indices.data(), GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GlobeVertex), reinterpret_cast<void*>(offsetof(GlobeVertex, position)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(GlobeVertex), reinterpret_cast<void*>(offsetof(GlobeVertex, normal)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(GlobeVertex), reinterpret_cast<void*>(offsetof(GlobeVertex, texcoord)));

    glBindVertexArray(0);
    gIndexCount = static_cast<GLsizei>(indices.size());
    LOGI("Globe mesh created: %zu vertices, %zu indices", vertices.size(), indices.size());
    return true;
}

static bool createGlobeResources() {
    gStartTime = std::chrono::steady_clock::now();
    gLastFrameTime = gStartTime;
    gLastDragTime = gStartTime;
    if (!createGlobeProgram()) {
        return false;
    }
    if (!createGlobeMesh()) {
        return false;
    }
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    return true;
}

static void destroyGlobeResources() {
    if (gIndexBuffer) glDeleteBuffers(1, &gIndexBuffer);
    if (gVertexBuffer) glDeleteBuffers(1, &gVertexBuffer);
    if (gVao) glDeleteVertexArrays(1, &gVao);
    if (gProgram) glDeleteProgram(gProgram);
    gIndexBuffer = 0;
    gVertexBuffer = 0;
    gVao = 0;
    gProgram = 0;
    gIndexCount = 0;
}

static bool initEGL(ANativeWindow* window) {
    gDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (gDisplay == EGL_NO_DISPLAY) return false;

    EGLint major, minor;
    if (!eglInitialize(gDisplay, &major, &minor)) return false;

    const EGLint attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_NONE
    };

    EGLConfig config;
    EGLint numConfigs;
    if (!eglChooseConfig(gDisplay, attribs, &config, 1, &numConfigs)) return false;
    if (numConfigs < 1) return false;

    gSurface = eglCreateWindowSurface(gDisplay, config, window, nullptr);
    if (gSurface == EGL_NO_SURFACE) return false;

    const EGLint ctxAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    gContext = eglCreateContext(gDisplay, config, EGL_NO_CONTEXT, ctxAttribs);
    if (gContext == EGL_NO_CONTEXT) return false;

    if (!eglMakeCurrent(gDisplay, gSurface, gSurface, gContext)) return false;

    eglQuerySurface(gDisplay, gSurface, EGL_WIDTH, &gWidth);
    eglQuerySurface(gDisplay, gSurface, EGL_HEIGHT, &gHeight);

    LOGI("EGL initialized: %dx%d, GL: %s, GLSL: %s",
         gWidth, gHeight,
         glGetString(GL_VERSION),
         glGetString(GL_SHADING_LANGUAGE_VERSION));

    return createGlobeResources();
}

static void destroyEGL() {
    destroyGlobeResources();
    eglMakeCurrent(gDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (gContext != EGL_NO_CONTEXT) eglDestroyContext(gDisplay, gContext);
    if (gSurface != EGL_NO_SURFACE) eglDestroySurface(gDisplay, gSurface);
    if (gDisplay != EGL_NO_DISPLAY) eglTerminate(gDisplay);
    gContext = EGL_NO_CONTEXT;
    gSurface = EGL_NO_SURFACE;
    gDisplay = EGL_NO_DISPLAY;
}

static void renderFrame() {
    glClearColor(0.0f, 0.0f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (gProgram && gVao && gWidth > 0 && gHeight > 0) {
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - gLastFrameTime).count();
        gLastFrameTime = now;
        if (!gTouching && gInertiaAngularVelocity > 0.0001f && dt > 0.0f) {
            constexpr float kInertiaDampingPerSecond = 3.0f;
            float angle = gInertiaAngularVelocity * dt;
            glm::quat delta = glm::angleAxis(angle, gInertiaAxis);
            gGlobeRotation = glm::normalize(delta * gGlobeRotation);
            gInertiaAngularVelocity *= std::exp(-kInertiaDampingPerSecond * dt);
        }

        float aspect = static_cast<float>(gWidth) / static_cast<float>(gHeight);
        float cameraDistance = gCameraDistance;

        glm::mat4 projection = glm::perspective(glm::radians(42.0f), aspect, 0.1f, 20.0f);
        glm::vec3 eye(0.0f,
                      std::sin(gCameraTilt) * cameraDistance,
                      std::cos(gCameraTilt) * cameraDistance);
        glm::vec3 up(0.0f, std::cos(gCameraTilt), -std::sin(gCameraTilt));
        glm::mat4 view = glm::lookAt(eye,
                                     glm::vec3(0.0f, 0.0f, 0.0f),
                                     up);
        glm::mat4 model = glm::mat4_cast(gGlobeRotation);
        glm::mat4 mvp = projection * view * model;

        glUseProgram(gProgram);
        glUniformMatrix4fv(gMvpLocation, 1, GL_FALSE, glm::value_ptr(mvp));
        glUniformMatrix4fv(gModelLocation, 1, GL_FALSE, glm::value_ptr(model));
        glUniform3f(gLightDirLocation, 0.35f, 0.45f, 0.82f);
        glBindVertexArray(gVao);
        glDrawElements(GL_TRIANGLES, gIndexCount, GL_UNSIGNED_INT, nullptr);
        glBindVertexArray(0);
    }

    eglSwapBuffers(gDisplay, gSurface);
}

static float projectedGlobeRadiusPixels(float height) {
    constexpr float kVerticalFovRadians = 42.0f * 3.14159265358979323846f / 180.0f;
    float focalLengthPixels = (height * 0.5f) / std::tan(kVerticalFovRadians * 0.5f);
    return std::max(1.0f, focalLengthPixels / gCameraDistance);
}

static glm::vec3 mapToArcball(float x, float y, float width, float height) {
    float radius = projectedGlobeRadiusPixels(height);
    float nx = (x - width * 0.5f) / radius;
    float ny = (height * 0.5f - y) / radius;
    float lengthSquared = nx * nx + ny * ny;
    if (lengthSquared <= 1.0f) {
        return glm::normalize(glm::vec3(nx, ny, std::sqrt(1.0f - lengthSquared)));
    }
    return glm::normalize(glm::vec3(nx, ny, 0.0f));
}

static void orbitCamera(float startX, float startY, float endX, float endY, float width, float height) {
    glm::vec3 from = mapToArcball(startX, startY, width, height);
    glm::vec3 to = mapToArcball(endX, endY, width, height);
    glm::vec3 axis = glm::cross(from, to);
    float axisLength = glm::length(axis);
    if (axisLength < 1e-5f) {
        return;
    }

    float dot = std::clamp(glm::dot(from, to), -1.0f, 1.0f);
    float angle = std::atan2(axisLength, dot);
    glm::vec3 normalizedAxis = axis / axisLength;
    glm::quat delta = glm::angleAxis(angle, normalizedAxis);
    gGlobeRotation = glm::normalize(delta * gGlobeRotation);

    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - gLastDragTime).count();
    gLastDragTime = now;
    if (dt > 0.0f && dt < 0.25f) {
        constexpr float kMaxInertiaAngularVelocity = 5.0f;
        constexpr float kVelocitySmoothing = 0.35f;
        float instantaneousVelocity = std::min(angle / dt, kMaxInertiaAngularVelocity);
        gInertiaAxis = normalizedAxis;
        gInertiaAngularVelocity =
            gInertiaAngularVelocity * (1.0f - kVelocitySmoothing) +
            instantaneousVelocity * kVelocitySmoothing;
    }
}

static void zoomCamera(float scale) {
    if (scale <= 0.0f) {
        return;
    }
    if (std::abs(scale - 1.0f) < 0.004f) {
        return;
    }
    constexpr float kMinDistance = 2.4f;
    constexpr float kMaxDistance = 12.0f;
    gInertiaAngularVelocity = 0.0f;
    gCameraDistance = std::clamp(gCameraDistance / scale, kMinDistance, kMaxDistance);
}

static glm::vec3 anchorAxisOnGlobe(float centerX, float centerY, float width, float height) {
    glm::vec3 viewSpaceAnchor = mapToArcball(centerX, centerY, width, height);
    glm::vec3 localAnchor = glm::normalize(glm::inverse(gGlobeRotation) * viewSpaceAnchor);
    return localAnchor;
}

static void rotateAroundAnchor(float rotationRadians, float centerX, float centerY, float width, float height) {
    constexpr float kRotateDeadZoneRadians = 0.006f;
    if (std::abs(rotationRadians) < kRotateDeadZoneRadians) {
        return;
    }
    gInertiaAngularVelocity = 0.0f;
    glm::vec3 localAnchor = anchorAxisOnGlobe(centerX, centerY, width, height);
    glm::quat delta = glm::angleAxis(-rotationRadians, localAnchor);
    gGlobeRotation = glm::normalize(gGlobeRotation * delta);
}

static void tiltAroundAnchor(float centerX, float centerY, float centerDy, float width, float height) {
    constexpr float kTiltDeadZonePixels = 3.0f;
    if (height <= 1.0f || std::abs(centerDy) < kTiltDeadZonePixels) {
        return;
    }
    constexpr float kTiltRadiansPerScreen = 1.8f;
    gInertiaAngularVelocity = 0.0f;
    float tiltRadians = -(centerDy / height) * kTiltRadiansPerScreen;
    glm::vec3 viewSpaceAnchor = mapToArcball(centerX, centerY, width, height);
    glm::vec3 viewRight = glm::normalize(glm::cross(viewSpaceAnchor, glm::vec3(0.0f, 0.0f, 1.0f)));
    if (glm::length(viewRight) < 1e-5f) {
        viewRight = glm::vec3(1.0f, 0.0f, 0.0f);
    }
    glm::quat delta = glm::angleAxis(tiltRadians, viewRight);
    gGlobeRotation = glm::normalize(delta * gGlobeRotation);
}

static void applyPinchRotateTilt(float scale,
                                 float rotationRadians,
                                 float centerX,
                                 float centerY,
                                 float centerDy,
                                 float width,
                                 float height) {
    zoomCamera(scale);
    rotateAroundAnchor(rotationRadians, centerX, centerY, width, height);
    tiltAroundAnchor(centerX, centerY, centerDy, width, height);
}

// ============================================================
// JNI 桥接
// ============================================================

extern "C" {

JNIEXPORT void JNICALL
Java_com_earthengine_minimalglobe_GLESView_nativeSurfaceCreated(
    JNIEnv* env, jobject /* this */, jobject surface) {

    gWindow = ANativeWindow_fromSurface(env, surface);
    if (!initEGL(gWindow)) {
        LOGE("Failed to initialize EGL");
    }
}

JNIEXPORT void JNICALL
Java_com_earthengine_minimalglobe_GLESView_nativeSurfaceChanged(
    JNIEnv* /* env */, jobject /* this */, jint width, jint height) {
    gWidth = width;
    gHeight = height;
    glViewport(0, 0, width, height);
}

JNIEXPORT void JNICALL
Java_com_earthengine_minimalglobe_GLESView_nativeRenderFrame(
    JNIEnv* /* env */, jobject /* this */) {
    renderFrame();
}

JNIEXPORT void JNICALL
Java_com_earthengine_minimalglobe_GLESView_nativeSurfaceDestroyed(
    JNIEnv* /* env */, jobject /* this */) {
    destroyEGL();
    if (gWindow) {
        ANativeWindow_release(gWindow);
        gWindow = nullptr;
    }
}

JNIEXPORT void JNICALL
Java_com_earthengine_minimalglobe_GLESView_nativeTouchDown(
    JNIEnv* /* env */, jobject /* this */) {
    gTouching = true;
    gInertiaAngularVelocity = 0.0f;
    gLastDragTime = std::chrono::steady_clock::now();
}

JNIEXPORT void JNICALL
Java_com_earthengine_minimalglobe_GLESView_nativeDrag(
    JNIEnv* /* env */, jobject /* this */,
    jfloat startX, jfloat startY, jfloat endX, jfloat endY,
    jint width, jint height) {
    orbitCamera(startX, startY, endX, endY, static_cast<float>(width), static_cast<float>(height));
}

JNIEXPORT void JNICALL
Java_com_earthengine_minimalglobe_GLESView_nativeTouchUp(
    JNIEnv* /* env */, jobject /* this */) {
    gTouching = false;
}

JNIEXPORT void JNICALL
Java_com_earthengine_minimalglobe_GLESView_nativePinch(
    JNIEnv* /* env */, jobject /* this */, jfloat scale) {
    zoomCamera(scale);
}

JNIEXPORT void JNICALL
Java_com_earthengine_minimalglobe_GLESView_nativePinchRotateTilt(
    JNIEnv* /* env */, jobject /* this */,
    jfloat scale, jfloat rotationRadians,
    jfloat centerX, jfloat centerY, jfloat centerDy,
    jint width, jint height) {
    applyPinchRotateTilt(scale,
                         rotationRadians,
                         centerX,
                         centerY,
                         centerDy,
                         static_cast<float>(width),
                         static_cast<float>(height));
}

JNIEXPORT void JNICALL
Java_com_earthengine_minimalglobe_GLESView_nativePause(
    JNIEnv* /* env */, jobject /* this */) {
    // TODO: 通知 PlatformBridge::onEnterBackground()
}

JNIEXPORT void JNICALL
Java_com_earthengine_minimalglobe_GLESView_nativeResume(
    JNIEnv* /* env */, jobject /* this */) {
    // TODO: 通知 PlatformBridge::onEnterForeground()
}

} // extern "C"
