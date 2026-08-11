package com.earthengine.sdk;

import android.content.Context;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

/**
 * 渲染循环由 native 渲染线程驱动（AChoreographer 帧节拍）；
 * 本类只负责 surface 生命周期转发与触摸事件整形。
 */
public class GLESView extends SurfaceView implements SurfaceHolder.Callback {
    static {
        System.loadLibrary("earth_engine_android_jni");
    }

    private float lastX;
    private float lastY;
    private float lastPinchDistance;
    private float lastPinchAngle;
    private float lastPinchCenterX;
    private float lastPinchCenterY;
    private boolean pinching;
    private boolean suppressSingleDragUntilUp;
    private int pinchPointerId0 = -1;
    private int pinchPointerId1 = -1;

    public GLESView(Context context) {
        super(context);
        nativeInit(context.getApplicationContext());
        getHolder().addCallback(this);
        setFocusable(true);
        setFocusableInTouchMode(true);
        requestFocus();
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        Surface surface = holder.getSurface();
        nativeSurfaceCreated(surface);
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        // 输入手势阈值以 dp 定义，native 侧用 density 把物理像素换算回 dp。
        nativeSetDisplayDensity(getResources().getDisplayMetrics().density);
        nativeSurfaceChanged(width, height);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        nativeSurfaceDestroyed();
    }

    public void onPause() {
        nativePause();
    }

    public void onResume() {
        nativeResume();
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_VOLUME_UP) {
            nativeDebugZoom(1.18f, getWidth(), getHeight());
            return true;
        }
        if (keyCode == KeyEvent.KEYCODE_VOLUME_DOWN) {
            nativeDebugZoom(0.84f, getWidth(), getHeight());
            return true;
        }
        if (keyCode == KeyEvent.KEYCODE_DPAD_DOWN) {
            nativeDebugTilt(80.0f, getWidth(), getHeight());
            return true;
        }
        if (keyCode == KeyEvent.KEYCODE_DPAD_UP) {
            nativeDebugTilt(-80.0f, getWidth(), getHeight());
            return true;
        }
        if (keyCode == KeyEvent.KEYCODE_DPAD_CENTER || keyCode == KeyEvent.KEYCODE_ENTER) {
            nativeDebugPinchEnd(getWidth(), getHeight());
            return true;
        }
        // 数字键 1-4：确定性双指路径回放（pan/pitch/组合/慢拧），手势回归用。
        if (keyCode >= KeyEvent.KEYCODE_1 && keyCode <= KeyEvent.KEYCODE_4) {
            nativeDebugPinchPath(keyCode - KeyEvent.KEYCODE_1, getWidth(), getHeight());
            return true;
        }
        // 数字键 5：低 AGL 贴地掠视复现位姿（动态 near/近平面切坡验收用）。
        if (keyCode == KeyEvent.KEYCODE_5) {
            nativeTerrainGrazingView();
            return true;
        }
        // 数字键 6：6km 斜视地平线复现位姿（接缝 A/B 采集台用）。原先
        // seam_metric 靠"DPAD_UP 累积 5 次"凑掠视姿态，那是路径依赖的——
        // 相机约束收口(2026-08-03 晚)之后倾角会被钳住，同一串按键到不了
        // 同一个姿态，采集台静默失效。位姿设定是一次性的，不受钳制影响。
        if (keyCode == KeyEvent.KEYCODE_6) {
            nativeGrazingView();
            return true;
        }
        // 数字键 7/8/9：相机架构阶段 3/4/5 的真机验证钩子。这三个阶段在 demo 里
        // 没有产品入口，不接出来就只能靠 host 判据。每个都配机制信号日志
        // （StageFlight / StageTether / StageOrtho）——画面"看着像对的"分不清
        // "真跑了"和"根本没走到那条路"。
        // 数字键 0：可复现正俯视位姿。正交的真实用途是俯视——掠视下正交盒
        // 下半部整个在地下，属退化用例。顺带验阶段 2 的万向节约定（pitch
        // 恰好 −π/2 是奇点）。
        if (keyCode == KeyEvent.KEYCODE_0) {
            nativeDebugNadirView();
            return true;
        }
        if (keyCode == KeyEvent.KEYCODE_7) {
            nativeDebugFlyTo();                              // 阶段 3：飞到北京
            return true;
        }
        if (keyCode == KeyEvent.KEYCODE_8) {
            nativeDebugTether();                             // 阶段 4：系留三态循环
            return true;
        }
        if (keyCode == KeyEvent.KEYCODE_9) {
            nativeDebugToggleOrtho(getWidth(), getHeight());  // 阶段 5：正交切换
            return true;
        }
        return super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        int action = event.getActionMasked();

        if (action == MotionEvent.ACTION_POINTER_UP) {
            if (pinching) {
                nativePinchEnd(lastPinchCenterX, lastPinchCenterY);
            }
            pinching = false;
            pinchPointerId0 = -1;
            pinchPointerId1 = -1;

            int remainingIndex = firstRemainingPointerIndex(event, event.getActionIndex());
            if (remainingIndex >= 0) {
                lastX = event.getX(remainingIndex);
                lastY = event.getY(remainingIndex);
                suppressSingleDragUntilUp = false;
                nativeResumePointer();
            } else {
                suppressSingleDragUntilUp = true;
            }
            return true;
        }

        if (event.getPointerCount() >= 2) {
            if (action == MotionEvent.ACTION_POINTER_DOWN || !pinching) {
                pinchPointerId0 = event.getPointerId(0);
                pinchPointerId1 = event.getPointerId(1);
            }

            int index0 = event.findPointerIndex(pinchPointerId0);
            int index1 = event.findPointerIndex(pinchPointerId1);
            if (index0 < 0 || index1 < 0 || index0 == index1) {
                return true;
            }

            float distance = pointerDistance(event, index0, index1);
            float angle = pointerAngle(event, index0, index1);
            float centerX = pointerCenterX(event, index0, index1);
            float centerY = pointerCenterY(event, index0, index1);
            if (action == MotionEvent.ACTION_POINTER_DOWN || !pinching) {
                pinching = true;
                suppressSingleDragUntilUp = true;
                nativePinchStart(centerX, centerY);
                lastPinchDistance = distance;
                lastPinchAngle = angle;
                lastPinchCenterX = centerX;
                lastPinchCenterY = centerY;
                return true;
            }
            if (action == MotionEvent.ACTION_MOVE && lastPinchDistance > 1.0f) {
                nativePinchRotateTilt(
                        distance / lastPinchDistance,
                        normalizeAngle(angle - lastPinchAngle),
                        centerX,
                        centerY,
                        centerX - lastPinchCenterX,
                        centerY - lastPinchCenterY,
                        event.getX(index0),
                        event.getY(index0),
                        event.getX(index1),
                        event.getY(index1),
                        getWidth(),
                        getHeight());
                lastPinchAngle = angle;
                lastPinchDistance = distance;
                lastPinchCenterX = centerX;
                lastPinchCenterY = centerY;
                return true;
            }
        }

        switch (action) {
            case MotionEvent.ACTION_DOWN:
                pinching = false;
                pinchPointerId0 = -1;
                pinchPointerId1 = -1;
                suppressSingleDragUntilUp = false;
                lastX = event.getX();
                lastY = event.getY();
                nativeTouchDown(lastX, lastY);
                return true;
            case MotionEvent.ACTION_MOVE:
                if (!pinching && !suppressSingleDragUntilUp && event.getPointerCount() == 1) {
                    float x = event.getX();
                    float y = event.getY();
                    nativeDrag(lastX, lastY, x, y, getWidth(), getHeight());
                    lastX = x;
                    lastY = y;
                }
                return true;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                if (pinching) {
                    nativePinchEnd(lastPinchCenterX, lastPinchCenterY);
                }
                pinching = false;
                pinchPointerId0 = -1;
                pinchPointerId1 = -1;
                suppressSingleDragUntilUp = false;
                nativeTouchUp(event.getX(), event.getY());
                return true;
            default:
                return true;
        }
    }

    private static int firstRemainingPointerIndex(MotionEvent event, int liftedIndex) {
        for (int i = 0; i < event.getPointerCount(); ++i) {
            if (i != liftedIndex) {
                return i;
            }
        }
        return -1;
    }

    private static float pointerDistance(MotionEvent event, int index0, int index1) {
        float dx = event.getX(index0) - event.getX(index1);
        float dy = event.getY(index0) - event.getY(index1);
        return (float) Math.sqrt(dx * dx + dy * dy);
    }

    private static float pointerAngle(MotionEvent event, int index0, int index1) {
        return (float) Math.atan2(event.getY(index1) - event.getY(index0), event.getX(index1) - event.getX(index0));
    }

    private static float pointerCenterX(MotionEvent event, int index0, int index1) {
        return (event.getX(index0) + event.getX(index1)) * 0.5f;
    }

    private static float pointerCenterY(MotionEvent event, int index0, int index1) {
        return (event.getY(index0) + event.getY(index1)) * 0.5f;
    }

    private static float normalizeAngle(float angle) {
        while (angle > Math.PI) {
            angle -= (float) (Math.PI * 2.0);
        }
        while (angle < -Math.PI) {
            angle += (float) (Math.PI * 2.0);
        }
        return angle;
    }

    private static native void nativeInit(Context appContext);
    private static native void nativeSurfaceCreated(Surface surface);
    private static native void nativeSurfaceChanged(int width, int height);
    private static native void nativeSetDisplayDensity(float density);
    private static native void nativeSurfaceDestroyed();
    private static native void nativeTouchDown(float x, float y);
    // 双指抬起一指后续接单指拖拽；不产生 click/double-click。
    private static native void nativeResumePointer();
    private static native void nativeDrag(float startX, float startY, float endX, float endY, int width, int height);
    private static native void nativeTouchUp(float x, float y);
    private static native void nativePinchStart(float centerX, float centerY);
    private static native void nativePinchEnd(float centerX, float centerY);
    private static native void nativePinchRotateTilt(float scale, float rotationRadians, float centerX, float centerY, float centerDx, float centerDy, float pointer0X, float pointer0Y, float pointer1X, float pointer1Y, int width, int height);
    private static native void nativeDebugZoom(float scale, int width, int height);
    private static native void nativeDebugTilt(float centerDy, int width, int height);
    private static native void nativeDebugPinchEnd(int width, int height);
    private static native void nativeDebugPinchPath(int scenario, int width, int height);
    private static native void nativePause();
    private static native void nativeResume();

    // 指北针：相机方位角(弧度,0=正北,顺时针+) / 复位正北朝上。
    public static native float nativeGetHeadingRadians();
    public static native void nativeResetNorthUp();

    // --- Debug panel native methods ---
    // 逐帧诊断串:面板已不再显示它(30 行文本在手机上读不完,数字走 logcat),
    // 保留接口供宿主按需取用。
    public native String nativeGetDiagnosticsString();
    public native void nativeAddDemoVectorLayer();
    public native void nativeResetCamera();
    public native void nativeGrazingView();
    public native void nativeDebugNadirView();
    public native void nativeDebugFlyTo();
    public native void nativeDebugTether();
    public native void nativeDebugToggleOrtho(int width, int height);
    // 低 AGL 贴地掠视（缙云山方向，动态 near 验收位姿）。
    public native void nativeTerrainGrazingView();
    public native void nativeSetGpuTerrain(boolean enabled);
    // ⚠️ 两个开关的真值都在 native(引擎标志 / atomic),UI 不许自己存一份镜像:
    // surface 重建会把引擎档位重置,Activity 重建会把 Java 字段重置,存两份必分叉。
    public native boolean nativeGetGpuTerrain();
    public native boolean nativeGetEditMode();

    // 矢量 P2 demo 编辑流(应用层最小实现:引擎只出 pick/snap/预览接口)。
    // EDIT 开启期间触摸走顶点拖拽编辑,相机手势被抑制。
    public native void nativeSetEditMode(boolean enabled);
    public native void nativeUndoEdit();
}
