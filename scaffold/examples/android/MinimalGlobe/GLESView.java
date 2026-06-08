package com.earthengine.minimalglobe;

import android.content.Context;
import android.view.Choreographer;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

public class GLESView extends SurfaceView implements SurfaceHolder.Callback, Choreographer.FrameCallback {
    static {
        System.loadLibrary("minimal_globe_jni");
    }

    private boolean rendering;
    private float lastX;
    private float lastY;
    private float lastPinchDistance;
    private float lastPinchAngle;
    private float lastPinchCenterX;
    private float lastPinchCenterY;
    private boolean pinching;
    private boolean suppressSingleDragUntilUp;

    public GLESView(Context context) {
        super(context);
        getHolder().addCallback(this);
        setFocusable(true);
        setFocusableInTouchMode(true);
        requestFocus();
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        Surface surface = holder.getSurface();
        nativeSurfaceCreated(surface);
        rendering = true;
        Choreographer.getInstance().postFrameCallback(this);
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        nativeSurfaceChanged(width, height);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        rendering = false;
        Choreographer.getInstance().removeFrameCallback(this);
        nativeSurfaceDestroyed();
    }

    @Override
    public void doFrame(long frameTimeNanos) {
        if (!rendering) {
            return;
        }
        nativeRenderFrame();
        Choreographer.getInstance().postFrameCallback(this);
    }

    public void onPause() {
        rendering = false;
        Choreographer.getInstance().removeFrameCallback(this);
        nativePause();
    }

    public void onResume() {
        nativeResume();
        if (getHolder().getSurface().isValid()) {
            rendering = true;
            Choreographer.getInstance().postFrameCallback(this);
        }
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
        return super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        int action = event.getActionMasked();

        if (event.getPointerCount() >= 2) {
            float distance = pointerDistance(event);
            float angle = pointerAngle(event);
            float centerX = pointerCenterX(event);
            float centerY = pointerCenterY(event);
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
                        centerY - lastPinchCenterY,
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
            case MotionEvent.ACTION_POINTER_UP:
                pinching = false;
                suppressSingleDragUntilUp = true;
                nativePinchEnd(lastPinchCenterX, lastPinchCenterY);
                if (event.getPointerCount() > 1) {
                    int remainingIndex = event.getActionIndex() == 0 ? 1 : 0;
                    lastX = event.getX(remainingIndex);
                    lastY = event.getY(remainingIndex);
                }
                return true;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                if (pinching) {
                    nativePinchEnd(lastPinchCenterX, lastPinchCenterY);
                }
                pinching = false;
                suppressSingleDragUntilUp = false;
                nativeTouchUp(event.getX(), event.getY());
                return true;
            default:
                return true;
        }
    }

    private static float pointerDistance(MotionEvent event) {
        float dx = event.getX(0) - event.getX(1);
        float dy = event.getY(0) - event.getY(1);
        return (float) Math.sqrt(dx * dx + dy * dy);
    }

    private static float pointerAngle(MotionEvent event) {
        return (float) Math.atan2(event.getY(1) - event.getY(0), event.getX(1) - event.getX(0));
    }

    private static float pointerCenterX(MotionEvent event) {
        return (event.getX(0) + event.getX(1)) * 0.5f;
    }

    private static float pointerCenterY(MotionEvent event) {
        return (event.getY(0) + event.getY(1)) * 0.5f;
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

    private static native void nativeSurfaceCreated(Surface surface);
    private static native void nativeSurfaceChanged(int width, int height);
    private static native void nativeRenderFrame();
    private static native void nativeSurfaceDestroyed();
    private static native void nativeTouchDown();
    private static native void nativeDrag(float startX, float startY, float endX, float endY, int width, int height);
    private static native void nativeTouchUp(float x, float y);
    private static native void nativePinchStart(float centerX, float centerY);
    private static native void nativePinchEnd(float centerX, float centerY);
    private static native void nativePinchRotateTilt(float scale, float rotationRadians, float centerX, float centerY, float centerDy, int width, int height);
    private static native void nativeDebugZoom(float scale, int width, int height);
    private static native void nativePause();
    private static native void nativeResume();
}
