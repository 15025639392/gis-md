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
                nativeTouchDown();
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
                nativeTouchDown();
                lastX = event.getX();
                lastY = event.getY();
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
    private static native void nativeSurfaceDestroyed();
    private static native void nativeTouchDown();
    private static native void nativeDrag(float startX, float startY, float endX, float endY, int width, int height);
    private static native void nativeTouchUp(float x, float y);
    private static native void nativePinchStart(float centerX, float centerY);
    private static native void nativePinchEnd(float centerX, float centerY);
    private static native void nativePinchRotateTilt(float scale, float rotationRadians, float centerX, float centerY, float centerDx, float centerDy, float pointer0X, float pointer0Y, float pointer1X, float pointer1Y, int width, int height);
    private static native void nativeDebugZoom(float scale, int width, int height);
    private static native void nativeDebugTilt(float centerDy, int width, int height);
    private static native void nativeDebugPinchEnd(int width, int height);
    private static native void nativePause();
    private static native void nativeResume();

    // [GESTDIAG] 读取当前手势锚点屏幕投影(物理像素)；out[0]=x,out[1]=y，
    // 返回 true 表示有活动锚点。用于可视化缩放/旋转锚点稳定性。
    public static native boolean nativeGetAnchorScreen(float[] out);

    // 指北针：相机方位角(弧度,0=正北,顺时针+) / 复位正北朝上。
    public static native float nativeGetHeadingRadians();
    public static native void nativeResetNorthUp();

    // --- Debug panel native methods ---
    public native String nativeGetDiagnosticsString();
    public native void nativeAddDemoVectorLayer();
    public native void nativeResetCamera();
}
