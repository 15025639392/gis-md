package com.earthengine.injector;

import android.app.UiAutomation;
import android.os.Bundle;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.MotionEvent.PointerCoords;
import android.view.MotionEvent.PointerProperties;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;

import org.junit.Test;
import org.junit.runner.RunWith;

/**
 * 免 root 多点触控注入台。通过 UiAutomation.injectInputEvent 把 2 指 MotionEvent
 * 打给当前焦点窗口(= 前台的 MinimalGlobe)。坐标为屏幕像素。
 *
 * 本模块是独立空壳 app,targetPackage 自指,故跑完不会 force-stop MinimalGlobe。
 *
 * 通用入口 twoFinger:两指各自从 (ax0,ay0)/(bx0,by0) 线性插值到
 * (ax1,ay1)/(bx1,by1),steps 步、总时长 dur ms。pinch/rotate/tilt 都由主机侧
 * 算好这 8 个坐标喂进来,台本身不区分手势语义。
 *
 * 用法:
 *   adb shell am instrument -w \
 *     -e class com.earthengine.injector.GestureInjector#twoFinger \
 *     -e ax0 620 -e ay0 1150 -e bx0 620 -e by0 1650 \
 *     -e ax1 620 -e ay1 900  -e bx1 620 -e by1 1900 \
 *     -e steps 24 -e dur 1000 \
 *     com.earthengine.injector.test/androidx.test.runner.AndroidJUnitRunner
 */
@RunWith(AndroidJUnit4.class)
public class GestureInjector {

    private static float argF(Bundle b, String k, float def) {
        String v = b.getString(k);
        return v == null ? def : Float.parseFloat(v);
    }

    private static int argI(Bundle b, String k, int def) {
        String v = b.getString(k);
        return v == null ? def : Integer.parseInt(v);
    }

    @Test
    public void twoFinger() throws InterruptedException {
        Bundle a = InstrumentationRegistry.getArguments();
        float ax0 = argF(a, "ax0", 0), ay0 = argF(a, "ay0", 0);
        float bx0 = argF(a, "bx0", 0), by0 = argF(a, "by0", 0);
        float ax1 = argF(a, "ax1", 0), ay1 = argF(a, "ay1", 0);
        float bx1 = argF(a, "bx1", 0), by1 = argF(a, "by1", 0);
        int steps = Math.max(1, argI(a, "steps", 20));
        int dur = Math.max(steps, argI(a, "dur", 400));

        UiAutomation ua = InstrumentationRegistry.getInstrumentation().getUiAutomation();

        PointerProperties[] props = new PointerProperties[2];
        PointerCoords[] coords = new PointerCoords[2];
        for (int i = 0; i < 2; i++) {
            props[i] = new PointerProperties();
            props[i].id = i;
            props[i].toolType = MotionEvent.TOOL_TYPE_FINGER;
            coords[i] = new PointerCoords();
            coords[i].pressure = 1f;
            coords[i].size = 1f;
        }

        final long down = SystemClock.uptimeMillis();

        // 指0 DOWN
        coords[0].x = ax0; coords[0].y = ay0;
        inject(ua, down, down, MotionEvent.ACTION_DOWN, 1, props, coords);

        // 指1 POINTER_DOWN(actionIndex=1)
        coords[0].x = ax0; coords[0].y = ay0;
        coords[1].x = bx0; coords[1].y = by0;
        int pDown = MotionEvent.ACTION_POINTER_DOWN
                | (1 << MotionEvent.ACTION_POINTER_INDEX_SHIFT);
        inject(ua, down, SystemClock.uptimeMillis(), pDown, 2, props, coords);

        // 插值 MOVE
        int perStep = dur / steps;
        for (int s = 1; s <= steps; s++) {
            float t = (float) s / steps;
            coords[0].x = ax0 + (ax1 - ax0) * t;
            coords[0].y = ay0 + (ay1 - ay0) * t;
            coords[1].x = bx0 + (bx1 - bx0) * t;
            coords[1].y = by0 + (by1 - by0) * t;
            inject(ua, down, SystemClock.uptimeMillis(),
                    MotionEvent.ACTION_MOVE, 2, props, coords);
            if (perStep > 0) Thread.sleep(perStep);
        }

        // 指1 POINTER_UP
        int pUp = MotionEvent.ACTION_POINTER_UP
                | (1 << MotionEvent.ACTION_POINTER_INDEX_SHIFT);
        inject(ua, down, SystemClock.uptimeMillis(), pUp, 2, props, coords);

        // 指0 UP
        inject(ua, down, SystemClock.uptimeMillis(),
                MotionEvent.ACTION_UP, 1, props, coords);
    }

    /**
     * 单指线性拖拽/fling。恒速(无 input swipe 的末端 ease-out),用合成恒 dt
     * 时间戳,故引擎侧算得的**释放速度 = 总距/dur**,稳过近地惯性 100px/s 阈值
     * (契约 1.4)。C-V4 近地惯性专用。
     *   -e x0 .. -e y0 .. -e x1 .. -e y1 .. -e steps .. -e dur ..
     */
    @Test
    public void oneFinger() throws InterruptedException {
        Bundle a = InstrumentationRegistry.getArguments();
        float x0 = argF(a, "x0", 0), y0 = argF(a, "y0", 0);
        float x1 = argF(a, "x1", 0), y1 = argF(a, "y1", 0);
        int steps = Math.max(1, argI(a, "steps", 20));
        int dur = Math.max(steps, argI(a, "dur", 300));

        UiAutomation ua = InstrumentationRegistry.getInstrumentation().getUiAutomation();
        PointerProperties[] props = new PointerProperties[]{new PointerProperties()};
        props[0].id = 0;
        props[0].toolType = MotionEvent.TOOL_TYPE_FINGER;
        PointerCoords[] coords = new PointerCoords[]{new PointerCoords()};
        coords[0].pressure = 1f;
        coords[0].size = 1f;

        final long down = SystemClock.uptimeMillis();
        final int perStep = dur / steps;

        coords[0].x = x0; coords[0].y = y0;
        inject(ua, down, down, MotionEvent.ACTION_DOWN, 1, props, coords);

        // 线性 MOVE + 合成恒 dt 时间戳(down + s*perStep)⇒ 恒速,末段不减速
        for (int s = 1; s <= steps; s++) {
            float t = (float) s / steps;
            coords[0].x = x0 + (x1 - x0) * t;
            coords[0].y = y0 + (y1 - y0) * t;
            inject(ua, down, down + (long) s * perStep,
                    MotionEvent.ACTION_MOVE, 1, props, coords);
            if (perStep > 0) Thread.sleep(perStep);
        }

        // UP 停在终点(释放速度由末几帧恒速样本决定)
        coords[0].x = x1; coords[0].y = y1;
        inject(ua, down, down + (long) (steps + 1) * perStep,
                MotionEvent.ACTION_UP, 1, props, coords);
    }

    private static void inject(UiAutomation ua, long downTime, long eventTime,
                               int action, int count,
                               PointerProperties[] props, PointerCoords[] coords) {
        MotionEvent e = MotionEvent.obtain(
                downTime, eventTime, action, count, props, coords,
                0, 0, 1f, 1f, 0, 0,
                InputDevice.SOURCE_TOUCHSCREEN, 0);
        ua.injectInputEvent(e, true);
        e.recycle();
    }
}
