package com.earthengine.minimalglobe;

import android.app.Activity;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;

import com.earthengine.sdk.GLESView;

public class MainActivity extends Activity {

    private GLESView mGLView;
    private View mDebugPanel;
    private Button mDebugButton;
    private Button mBtnResetCamera;
    // ⚠️ 开关不在 Java 侧存状态:真值在引擎/native,这里只留按钮引用用于回读刷文案。
    // 曾经的 mGpuTerrainOn/mEditModeOn 是"同一事实两处各存一份",surface 重建或
    // Activity 旋转重建后按钮文案会与引擎实际档位静默分叉。
    private Button mBtnGpuTerrain;
    private Button mBtnTether;
    private Button mBtnOrtho;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        enterImmersiveMode();

        FrameLayout root = new FrameLayout(this);


        // GL surface
        mGLView = new GLESView(this);
        root.addView(mGLView, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));

        // 指北针（左上角；点按复位正北）
        CompassView compass = new CompassView(this);
        FrameLayout.LayoutParams compassParams = new FrameLayout.LayoutParams(
                dp(38), dp(38), Gravity.BOTTOM | Gravity.END);
        // 高德网页布局：罗盘位于右下方缩放控件上方，设置按钮仍在右上角。
        compassParams.setMargins(0, 0, dp(14), dp(112));
        root.addView(compass, compassParams);

        // 比例尺（左下角）：数值来自相机拾取射线，随缩放低频更新。
        ScaleBarView scaleBar = new ScaleBarView(this);
        FrameLayout.LayoutParams scaleParams = new FrameLayout.LayoutParams(
                dp(152), dp(52), Gravity.BOTTOM | Gravity.START);
        scaleParams.setMargins(dp(14), 0, 0, dp(18));
        root.addView(scaleBar, scaleParams);

        // 高德式右下角缩放控件；按钮直接复用 demo 的相机缩放入口。
        LinearLayout zoomControls = new LinearLayout(this);
        zoomControls.setOrientation(LinearLayout.VERTICAL);
        zoomControls.setBackground(makeZoomControlBackground());
        zoomControls.setElevation(dp(3));
        Button zoomIn = makeZoomButton("+");
        Button zoomOut = makeZoomButton("−");
        zoomIn.setOnClickListener(v ->
                GLESView.requestZoom(1.38f, mGLView.getWidth(), mGLView.getHeight()));
        zoomOut.setOnClickListener(v ->
                GLESView.requestZoom(0.72f, mGLView.getWidth(), mGLView.getHeight()));
        zoomControls.addView(zoomIn);
        zoomControls.addView(zoomOut);
        FrameLayout.LayoutParams zoomParams = new FrameLayout.LayoutParams(
                dp(38), dp(76), Gravity.BOTTOM | Gravity.END);
        zoomParams.setMargins(0, 0, dp(14), dp(24));
        root.addView(zoomControls, zoomParams);

        // Debug button (floating)
        mDebugButton = new Button(this);
        mDebugButton.setText("⚙");
        mDebugButton.setTextSize(17);
        mDebugButton.setTextColor(0xFF555555);
        mDebugButton.setGravity(Gravity.CENTER);
        mDebugButton.setPadding(0, 0, 0, 0);
        mDebugButton.setMinWidth(0);
        mDebugButton.setMinHeight(0);
        mDebugButton.setBackground(makeFloatingControlBackground());
        mDebugButton.setElevation(dp(3));
        FrameLayout.LayoutParams btnParams = new FrameLayout.LayoutParams(
                dp(36), dp(36), Gravity.TOP | Gravity.END);
        btnParams.setMargins(0, dp(18), dp(12), 0);
        mDebugButton.setOnClickListener(v -> toggleDebugPanel());
        root.addView(mDebugButton, btnParams);

        // Debug panel (initially hidden)
        mDebugPanel = createDebugPanel();
        mDebugPanel.setVisibility(View.GONE);
        FrameLayout.LayoutParams panelParams = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                // 诊断文本没了 ⇒ 高度跟随内容,别再留一块空的黑框挡地球。
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.BOTTOM);
        panelParams.setMargins(dp(8), 0, dp(8), dp(8));
        root.addView(mDebugPanel, panelParams);

        setContentView(root);
    }

    private View createDebugPanel() {
        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setBackgroundColor(0xCC000000);
        panel.setPadding(dp(12), dp(10), dp(12), dp(10));

        // 逐帧诊断文本已移除:同样的数字每帧都进 logcat,而 30 行 9sp 塞进 220dp
        // 高的框在手机上没人读得完,还挡着被诊断的画面本身。要看数字读 logcat。

        // Action buttons
        LinearLayout actions = new LinearLayout(this);
        actions.setOrientation(LinearLayout.HORIZONTAL);
        actions.setGravity(Gravity.START);

        mBtnResetCamera = new Button(this);
        mBtnResetCamera.setText("Reset");
        mBtnResetCamera.setTextSize(10);
        mBtnResetCamera.setOnClickListener(v -> resetCamera());
        actions.addView(mBtnResetCamera);

        // 斜视地平线预设（性能测量用，可复现固定位姿）
        Button btnHorizon = new Button(this);
        btnHorizon.setText("Horizon");
        btnHorizon.setTextSize(10);
        btnHorizon.setOnClickListener(v -> mGLView.nativeGrazingView());
        actions.addView(btnHorizon);

        // 北极星 Phase 2c 地形 GPU 位移 A/B 开关(设备侧前后对比)。
        mBtnGpuTerrain = new Button(this);
        mBtnGpuTerrain.setTextSize(10);
        mBtnGpuTerrain.setOnClickListener(v -> {
            // 先回读再取反:以引擎当前档位为准,而不是以上一次点击为准。
            mGLView.nativeSetGpuTerrain(!mGLView.nativeGetGpuTerrain());
            syncToggleLabels();
        });
        actions.addView(mBtnGpuTerrain);

        panel.addView(actions);

        // 相机阶段 3/4/5 的验收钩子(此前只在数字键 0/7/8/9 上,要 adb 才按得到)。
        LinearLayout cameraActions = new LinearLayout(this);
        cameraActions.setOrientation(LinearLayout.HORIZONTAL);
        cameraActions.setGravity(Gravity.START);

        // 阶段 2:可复现正俯视位姿(pitch 恰好 −π/2 = 万向节奇点),正交的用武之地。
        Button btnNadir = new Button(this);
        btnNadir.setText("Nadir");
        btnNadir.setTextSize(10);
        btnNadir.setOnClickListener(v -> mGLView.nativeDebugNadirView());
        cameraActions.addView(btnNadir);

        // 阶段 3:飞到北京(拱高规划 + cameraFlightActive 契约)。
        Button btnFly = new Button(this);
        btnFly.setText("Fly");
        btnFly.setTextSize(10);
        btnFly.setOnClickListener(v -> mGLView.nativeDebugFlyTo());
        cameraActions.addView(btnFly);

        // 阶段 4:系留三态循环 Free → 跟车 → 座舱 → Free。
        mBtnTether = new Button(this);
        mBtnTether.setTextSize(10);
        mBtnTether.setOnClickListener(v -> {
            mGLView.nativeDebugTether();
            syncToggleLabels();
        });
        cameraActions.addView(mBtnTether);

        // 阶段 5:正交/透视切换。宽高传给 native 换算正交足迹,和按键路径同参。
        mBtnOrtho = new Button(this);
        mBtnOrtho.setTextSize(10);
        mBtnOrtho.setOnClickListener(v -> {
            mGLView.nativeDebugToggleOrtho(mGLView.getWidth(), mGLView.getHeight());
            syncToggleLabels();
        });
        cameraActions.addView(mBtnOrtho);

        panel.addView(cameraActions);

        // Close button
        Button closeBtn = new Button(this);
        closeBtn.setText("Close");
        closeBtn.setTextSize(10);
        closeBtn.setTextColor(Color.WHITE);
        closeBtn.setBackgroundColor(0x88000000);
        closeBtn.setOnClickListener(v -> toggleDebugPanel());
        panel.addView(closeBtn);

        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(false);
        scroll.addView(panel);
        return scroll;
    }

    private void enterImmersiveMode() {
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                        | View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    /** 高德式轻量浮层：白底、浅边框、阴影交给系统 elevation，避免额外绘制开销。 */
    private GradientDrawable makeFloatingControlBackground() {
        GradientDrawable background = new GradientDrawable();
        background.setShape(GradientDrawable.OVAL);
        background.setColor(0xF7FFFFFF);
        background.setStroke(dp(1), 0x1F000000);
        return background;
    }

    private GradientDrawable makeZoomControlBackground() {
        GradientDrawable background = new GradientDrawable();
        background.setShape(GradientDrawable.RECTANGLE);
        background.setColor(0xF7FFFFFF);
        background.setCornerRadius(dp(4));
        background.setStroke(dp(1), 0x1F000000);
        return background;
    }

    private Button makeZoomButton(String caption) {
        Button button = new Button(this);
        button.setText(caption);
        button.setTextSize(20);
        button.setTextColor(0xFF4C4C4C);
        button.setGravity(Gravity.CENTER);
        button.setPadding(0, 0, 0, 0);
        button.setMinWidth(0);
        button.setMinHeight(0);
        button.setBackgroundColor(Color.TRANSPARENT);
        return button;
    }

    /**
     * 指北针：红针指向正北。相机 heading=0（朝正北）时红针朝上。点按复位正北朝上。
     * 每帧读一次 heading（极廉价），仅当角度变化时才重绘（省电）。
     */
    private static final class CompassView extends View {
        private final Paint ring = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint north = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint south = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint label = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Path needle = new Path();
        private float headingRad = 0f;

        CompassView(Context context) {
            super(context);
            setClickable(true);
            setOnClickListener(v -> GLESView.nativeResetNorthUp());
            setElevation(getResources().getDisplayMetrics().density * 3f);
            ring.setColor(0xF7FFFFFF);
            ring.setStyle(Paint.Style.FILL);
            north.setColor(0xFFE84A3C);   // 北=红
            south.setColor(0xFF717171);   // 南=深灰，在白色浮层上保持辨识度
            label.setColor(0xFFFFFFFF);
            label.setTextAlign(Paint.Align.CENTER);

            post(new Runnable() {
                @Override public void run() {
                    setHeading(GLESView.nativeGetHeadingRadians());
                    postOnAnimation(this);   // 每帧廉价读取，变化才 invalidate
                }
            });
        }

        private void setHeading(float h) {
            if (Math.abs(h - headingRad) > 0.005f) {   // ~0.3°
                headingRad = h;
                invalidate();
            }
        }

        @Override
        protected void onDraw(Canvas canvas) {
            final float cx = getWidth() * 0.5f;
            final float cy = getHeight() * 0.5f;
            final float r = Math.min(cx, cy) - 2f;
            canvas.drawCircle(cx, cy, r, ring);

            ring.setColor(0x1F000000);
            ring.setStyle(Paint.Style.STROKE);
            ring.setStrokeWidth(getResources().getDisplayMetrics().density);
            canvas.drawCircle(cx, cy, r - ring.getStrokeWidth() * 0.5f, ring);
            ring.setColor(0xF7FFFFFF);
            ring.setStyle(Paint.Style.FILL);

            canvas.save();
            // heading=0 → 红针朝上；heading 增大(向东)→ 北在屏幕上向左偏，故反向旋转。
            canvas.rotate((float) Math.toDegrees(-headingRad), cx, cy);
            needle.reset();
            needle.moveTo(cx, cy - r * 0.72f);
            needle.lineTo(cx - r * 0.22f, cy);
            needle.lineTo(cx + r * 0.22f, cy);
            needle.close();
            canvas.drawPath(needle, north);
            needle.reset();
            needle.moveTo(cx, cy + r * 0.72f);
            needle.lineTo(cx - r * 0.22f, cy);
            needle.lineTo(cx + r * 0.22f, cy);
            needle.close();
            canvas.drawPath(needle, south);
            label.setTextSize(r * 0.42f);
            canvas.drawText("N", cx, cy - r * 0.34f, label);
            canvas.restore();
        }
    }

    /** 无卡片、细线条的地图比例尺；400ms 查询一次，避免逐帧跨线程同步。 */
    private static final class ScaleBarView extends View {
        private static final long UPDATE_INTERVAL_MS = 400L;
        private final Paint line = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final float density;
        private double displayedMeters;
        private float displayedWidthPx;
        private String displayedLabel = "";

        private final Runnable updater = new Runnable() {
            @Override public void run() {
                if (!isAttachedToWindow()) return;
                updateScale(GLESView.nativeGetMetersPerPixel());
                postDelayed(this, UPDATE_INTERVAL_MS);
            }
        };

        ScaleBarView(Context context) {
            super(context);
            density = getResources().getDisplayMetrics().density;
            line.setColor(0xCC3E4146);
            line.setStyle(Paint.Style.STROKE);
            line.setStrokeWidth(1f);
            line.setStrokeCap(Paint.Cap.SQUARE);
            text.setColor(0xE63E4146);
            text.setTextSize(10f * density);
            text.setTextAlign(Paint.Align.CENTER);
            text.setShadowLayer(1.5f * density, 0f, 0.5f * density, 0xCCFFFFFF);
        }

        @Override protected void onAttachedToWindow() {
            super.onAttachedToWindow();
            removeCallbacks(updater);
            post(updater);
        }

        @Override protected void onDetachedFromWindow() {
            removeCallbacks(updater);
            super.onDetachedFromWindow();
        }

        private void updateScale(double metersPerPixel) {
            if (!(metersPerPixel > 0.0) || !Double.isFinite(metersPerPixel)) {
                if (displayedWidthPx != 0f) {
                    displayedWidthPx = 0f;
                    invalidate();
                }
                return;
            }

            final double targetPx = 88.0 * density;
            final double rawMeters = metersPerPixel * targetPx;
            final double power = Math.pow(10.0, Math.floor(Math.log10(rawMeters)));
            final double normalized = rawMeters / power;
            final double nice = normalized >= 5.0 ? 5.0
                    : normalized >= 2.0 ? 2.0 : 1.0;
            final double meters = nice * power;
            final float widthPx = (float) (meters / metersPerPixel);
            final String label = formatDistance(meters);
            if (Math.abs(meters - displayedMeters) > 0.01 ||
                    Math.abs(widthPx - displayedWidthPx) > 0.5f ||
                    !label.equals(displayedLabel)) {
                displayedMeters = meters;
                displayedWidthPx = widthPx;
                displayedLabel = label;
                invalidate();
            }
        }

        private static String formatDistance(double meters) {
            if (meters >= 1000.0) {
                final double km = meters / 1000.0;
                return (km >= 10.0 || Math.rint(km) == km
                        ? String.format(java.util.Locale.ROOT, "%.0f", km)
                        : String.format(java.util.Locale.ROOT, "%.1f", km)) + " 公里";
            }
            return String.format(java.util.Locale.ROOT, "%.0f 米", meters);
        }

        @Override protected void onDraw(Canvas canvas) {
            if (displayedWidthPx <= 0f || displayedLabel.isEmpty()) return;
            final float left = 6f * density;
            final float maxWidth = getWidth() - left - 6f * density;
            final float width = Math.min(displayedWidthPx, maxWidth);
            final float y = 32f * density;
            final float tick = 5f * density;
            canvas.drawLine(left, y, left + width, y, line);
            canvas.drawLine(left, y - tick, left, y + 1f, line);
            canvas.drawLine(left + width, y - tick, left + width, y + 1f, line);
            canvas.drawText(displayedLabel, left + width * 0.5f,
                    y - 7f * density, text);
        }
    }

    private void toggleDebugPanel() {
        if (mDebugPanel.getVisibility() == View.VISIBLE) {
            mDebugPanel.setVisibility(View.GONE);
        } else {
            mDebugPanel.setVisibility(View.VISIBLE);
            // 每次打开都回读:面板关着的这段时间里引擎可能已被重建(surface 重建)。
            syncToggleLabels();
        }
    }

    /** 把 toggle 按钮文案对齐 native 真值。唯一的写文案入口。 */
    private void syncToggleLabels() {
        mBtnGpuTerrain.setText(
                mGLView.nativeGetGpuTerrain() ? "GPU Terr: ON" : "GPU Terr: OFF");
        final int tether = mGLView.nativeGetTetherState();
        mBtnTether.setText(tether == 2 ? "Tether: 座舱"
                         : tether == 1 ? "Tether: 跟车"
                                       : "Tether: OFF");
        mBtnOrtho.setText(mGLView.nativeGetOrtho() ? "Ortho: ON" : "Ortho: OFF");
    }

    // --- Actions (call through to GLESView native) ---
    private void resetCamera() { mGLView.nativeResetCamera(); }

    @Override
    protected void onPause() {
        super.onPause();
        mGLView.onPause();
        // 面板收起:回读只发生在"打开的那一刻",而离开前台期间 surface 可能被销毁
        // 重建(引擎全重建,开关档位跟着回默认)。强制重新打开 ⇒ 必然重新回读,
        // 不留"面板开着时状态在背后变了"的窗口。
        mDebugPanel.setVisibility(View.GONE);
    }

    @Override
    protected void onResume() {
        super.onResume();
        enterImmersiveMode();
        mGLView.onResume();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            enterImmersiveMode();
        }
    }
}
