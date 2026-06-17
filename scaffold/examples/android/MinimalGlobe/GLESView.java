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
    private int pinchPointerId0 = -1;
    private int pinchPointerId1 = -1;

    // DIAGNOSTIC ONLY: Android input/JNI timing for MinimalGlobe stutter diagnosis.
    private static final String DIAG_TAG = "EarthPerfInput";
    private long diagWindowStartNs = System.nanoTime();
    private int diagMoveEvents;
    private int diagSingleMoveEvents;
    private int diagPinchMoveEvents;
    private int diagIgnoredMoveEvents;
    private int diagHistoricalSamples;
    private int diagNativeDragCalls;
    private int diagNativePinchCalls;
    private long diagNativeDragNs;
    private long diagNativePinchNs;
    private long diagMaxNativeDragNs;
    private long diagMaxNativePinchNs;
    private int diagLastPointerCount;

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
        if (action == MotionEvent.ACTION_MOVE) {
            diagRecordMoveEvent(event);
        }

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
                long diagNativeStartNs = System.nanoTime();
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
                diagRecordNativePinch(System.nanoTime() - diagNativeStartNs);
                lastPinchAngle = angle;
                lastPinchDistance = distance;
                lastPinchCenterX = centerX;
                lastPinchCenterY = centerY;
                diagMaybeLog("pinchMove");
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
                    long diagNativeStartNs = System.nanoTime();
                    nativeDrag(lastX, lastY, x, y, getWidth(), getHeight());
                    diagRecordNativeDrag(System.nanoTime() - diagNativeStartNs);
                    lastX = x;
                    lastY = y;
                } else {
                    diagIgnoredMoveEvents++;
                }
                diagMaybeLog("move");
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

    private void diagRecordMoveEvent(MotionEvent event) {
        diagMoveEvents++;
        diagHistoricalSamples += event.getHistorySize();
        diagLastPointerCount = event.getPointerCount();
        if (event.getPointerCount() >= 2) {
            diagPinchMoveEvents++;
        } else if (event.getPointerCount() == 1) {
            diagSingleMoveEvents++;
        }
    }

    private void diagRecordNativeDrag(long elapsedNs) {
        diagNativeDragCalls++;
        diagNativeDragNs += elapsedNs;
        if (elapsedNs > diagMaxNativeDragNs) {
            diagMaxNativeDragNs = elapsedNs;
        }
    }

    private void diagRecordNativePinch(long elapsedNs) {
        diagNativePinchCalls++;
        diagNativePinchNs += elapsedNs;
        if (elapsedNs > diagMaxNativePinchNs) {
            diagMaxNativePinchNs = elapsedNs;
        }
    }

    private void diagMaybeLog(String phase) {
        long nowNs = System.nanoTime();
        long windowNs = nowNs - diagWindowStartNs;
        if (windowNs < 1_000_000_000L) {
            return;
        }

        double windowMs = windowNs / 1_000_000.0;
        double dragAvgMs = diagNativeDragCalls > 0
                ? (diagNativeDragNs / 1_000_000.0) / diagNativeDragCalls
                : 0.0;
        double pinchAvgMs = diagNativePinchCalls > 0
                ? (diagNativePinchNs / 1_000_000.0) / diagNativePinchCalls
                : 0.0;
        android.util.Log.i(DIAG_TAG,
                "DIAG javaInput phase=" + phase
                        + " windowMs=" + String.format("%.0f", windowMs)
                        + " move=" + diagMoveEvents
                        + " singleMove=" + diagSingleMoveEvents
                        + " pinchMove=" + diagPinchMoveEvents
                        + " ignoredMove=" + diagIgnoredMoveEvents
                        + " historySamples=" + diagHistoricalSamples
                        + " nativeDragCalls=" + diagNativeDragCalls
                        + " nativeDragAvgMs=" + String.format("%.3f", dragAvgMs)
                        + " nativeDragMaxMs=" + String.format("%.3f", diagMaxNativeDragNs / 1_000_000.0)
                        + " nativePinchCalls=" + diagNativePinchCalls
                        + " nativePinchAvgMs=" + String.format("%.3f", pinchAvgMs)
                        + " nativePinchMaxMs=" + String.format("%.3f", diagMaxNativePinchNs / 1_000_000.0)
                        + " lastPointers=" + diagLastPointerCount);

        diagWindowStartNs = nowNs;
        diagMoveEvents = 0;
        diagSingleMoveEvents = 0;
        diagPinchMoveEvents = 0;
        diagIgnoredMoveEvents = 0;
        diagHistoricalSamples = 0;
        diagNativeDragCalls = 0;
        diagNativePinchCalls = 0;
        diagNativeDragNs = 0;
        diagNativePinchNs = 0;
        diagMaxNativeDragNs = 0;
        diagMaxNativePinchNs = 0;
        diagLastPointerCount = 0;
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
    private static native void nativePinchRotateTilt(float scale, float rotationRadians, float centerX, float centerY, float centerDx, float centerDy, float pointer0X, float pointer0Y, float pointer1X, float pointer1Y, int width, int height);
    private static native void nativeDebugZoom(float scale, int width, int height);
    private static native void nativePause();
    private static native void nativeResume();

    // --- Debug panel native methods ---
    public native String nativeGetDiagnosticsString();
    public native void nativeAddDemoVectorLayer();
    public native void nativeResetCamera();
}
