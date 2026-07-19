package com.earthengine.minimalglobe;

import android.app.Activity;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import com.earthengine.sdk.GLESView;

public class MainActivity extends Activity {

    private GLESView mGLView;
    private View mDebugPanel;
    private Button mDebugButton;
    private TextView mDiagnosticsText;
    private Button mBtnAddVectorLayer;
    private Button mBtnResetCamera;
    private Handler mHandler;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        enterImmersiveMode();
        mHandler = new Handler(Looper.getMainLooper());

        FrameLayout root = new FrameLayout(this);

        // GL surface
        mGLView = new GLESView(this);
        root.addView(mGLView, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));

        // [GESTDIAG] 手势锚点可视化覆盖层（在 GL 之上、按钮之下；不拦截触摸）
        AnchorOverlayView anchorOverlay = new AnchorOverlayView(this);
        root.addView(anchorOverlay, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));

        // 指北针（左上角；点按复位正北）
        CompassView compass = new CompassView(this);
        FrameLayout.LayoutParams compassParams = new FrameLayout.LayoutParams(
                dp(56), dp(56), Gravity.TOP | Gravity.START);
        compassParams.setMargins(dp(12), dp(24), 0, 0);
        root.addView(compass, compassParams);

        // Debug button (floating)
        mDebugButton = new Button(this);
        mDebugButton.setText("⚙");
        mDebugButton.setTextSize(18);
        mDebugButton.setTextColor(Color.WHITE);
        mDebugButton.setBackgroundColor(0x88000000);
        FrameLayout.LayoutParams btnParams = new FrameLayout.LayoutParams(
                dp(48), dp(48), Gravity.TOP | Gravity.END);
        btnParams.setMargins(0, dp(24), dp(12), 0);
        mDebugButton.setOnClickListener(v -> toggleDebugPanel());
        root.addView(mDebugButton, btnParams);

        // Debug panel (initially hidden)
        mDebugPanel = createDebugPanel();
        mDebugPanel.setVisibility(View.GONE);
        FrameLayout.LayoutParams panelParams = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                dp(220),
                Gravity.BOTTOM);
        panelParams.setMargins(dp(8), 0, dp(8), dp(8));
        root.addView(mDebugPanel, panelParams);

        setContentView(root);

        // Periodic diagnostics refresh
        mHandler.postDelayed(new Runnable() {
            @Override
            public void run() {
                refreshDiagnostics();
                mHandler.postDelayed(this, 500);
            }
        }, 500);
    }

    private View createDebugPanel() {
        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setBackgroundColor(0xCC000000);
        panel.setPadding(dp(12), dp(10), dp(12), dp(10));

        TextView title = new TextView(this);
        title.setText("Debug");
        title.setTextColor(Color.WHITE);
        title.setTextSize(12);
        panel.addView(title);

        // Diagnostics
        mDiagnosticsText = new TextView(this);
        mDiagnosticsText.setTextColor(0xFFE0E0E0);
        mDiagnosticsText.setTextSize(9);
        mDiagnosticsText.setIncludeFontPadding(false);
        mDiagnosticsText.setPadding(0, dp(6), 0, dp(6));
        panel.addView(mDiagnosticsText);

        // Action buttons
        LinearLayout actions = new LinearLayout(this);
        actions.setOrientation(LinearLayout.HORIZONTAL);
        actions.setGravity(Gravity.START);

        mBtnAddVectorLayer = new Button(this);
        mBtnAddVectorLayer.setText("+ Vector");
        mBtnAddVectorLayer.setTextSize(10);
        mBtnAddVectorLayer.setOnClickListener(v -> addDemoVectorLayer());
        actions.addView(mBtnAddVectorLayer);

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

        panel.addView(actions);

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
            ring.setColor(0xAA000000);
            ring.setStyle(Paint.Style.FILL);
            north.setColor(0xFFFF3B30);   // 北=红
            south.setColor(0xFFEEEEEE);   // 南=白
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
            final float r = Math.min(cx, cy) - 3f;
            canvas.drawCircle(cx, cy, r, ring);

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

    /**
     * [GESTDIAG] 在 GL 之上绘制当前手势锚点的十字标记，vsync 连续自刷新，
     * 不拦截触摸（落到下面的 GLESView）。用于真机观察缩放/旋转时锚点是否
     * 稳定跟手——标记若在双指触摸/捏合首帧跳离手指即为"瞬间偏移"。
     */
    private static final class AnchorOverlayView extends View {
        private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint ring = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final float[] pos = new float[2];

        AnchorOverlayView(Context context) {
            super(context);
            setClickable(false);
            setFocusable(false);
            fill.setColor(0xFFFF3B30);   // 红色实心圆
            fill.setStyle(Paint.Style.FILL);
            ring.setColor(0xFFFFFFFF);   // 白色描边+十字
            ring.setStyle(Paint.Style.STROKE);
            ring.setStrokeWidth(3f);
        }

        @Override
        public boolean onTouchEvent(android.view.MotionEvent event) {
            return false;  // 不消费，触摸透传到下面的 GLESView
        }

        @Override
        protected void onDraw(Canvas canvas) {
            if (GLESView.nativeGetAnchorScreen(pos)) {
                final float x = pos[0];
                final float y = pos[1];
                final float r = 14f * getResources().getDisplayMetrics().density;
                canvas.drawCircle(x, y, 6f, fill);
                canvas.drawCircle(x, y, r, ring);
                canvas.drawLine(x - r * 1.6f, y, x + r * 1.6f, y, ring);
                canvas.drawLine(x, y - r * 1.6f, x, y + r * 1.6f, ring);
            }
            postInvalidateOnAnimation();  // vsync 连续刷新，跟随相机每帧移动
        }
    }

    private void toggleDebugPanel() {
        if (mDebugPanel.getVisibility() == View.VISIBLE) {
            mDebugPanel.setVisibility(View.GONE);
        } else {
            mDebugPanel.setVisibility(View.VISIBLE);
            refreshDiagnostics();
        }
    }

    private void refreshDiagnostics() {
        if (mDebugPanel.getVisibility() != View.VISIBLE) return;
        String diag = mGLView.nativeGetDiagnosticsString();
        if (diag != null && !diag.isEmpty()) {
            mDiagnosticsText.setText(diag);
        }
    }

    // --- Actions (call through to GLESView native) ---
    private void addDemoVectorLayer() { mGLView.nativeAddDemoVectorLayer(); }
    private void resetCamera() { mGLView.nativeResetCamera(); }

    @Override
    protected void onPause() {
        super.onPause();
        mGLView.onPause();
        mHandler.removeCallbacksAndMessages(null);
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
