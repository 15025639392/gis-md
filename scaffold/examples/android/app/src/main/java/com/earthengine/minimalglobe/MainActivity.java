package com.earthengine.minimalglobe;

import android.app.Activity;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
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
    private Button mBtnAddVectorLayer;
    private Button mBtnResetCamera;
    // ⚠️ 开关不在 Java 侧存状态:真值在引擎/native,这里只留按钮引用用于回读刷文案。
    // 曾经的 mGpuTerrainOn/mEditModeOn 是"同一事实两处各存一份",surface 重建或
    // Activity 旋转重建后按钮文案会与引擎实际档位静默分叉。
    private Button mBtnGpuTerrain;
    private Button mBtnEdit;
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

        // 矢量 P2 编辑流(demo 应用层):EDIT 切换 + UNDO,独立第二行
        LinearLayout editActions = new LinearLayout(this);
        editActions.setOrientation(LinearLayout.HORIZONTAL);
        editActions.setGravity(Gravity.START);

        mBtnEdit = new Button(this);
        mBtnEdit.setTextSize(10);
        mBtnEdit.setOnClickListener(v -> {
            mGLView.nativeSetEditMode(!mGLView.nativeGetEditMode());
            syncToggleLabels();
        });
        editActions.addView(mBtnEdit);

        Button btnUndo = new Button(this);
        btnUndo.setText("Undo");
        btnUndo.setTextSize(10);
        btnUndo.setOnClickListener(v -> mGLView.nativeUndoEdit());
        editActions.addView(btnUndo);

        panel.addView(editActions);

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
        mBtnEdit.setText(mGLView.nativeGetEditMode() ? "Edit: ON" : "Edit: OFF");
        final int tether = mGLView.nativeGetTetherState();
        mBtnTether.setText(tether == 2 ? "Tether: 座舱"
                         : tether == 1 ? "Tether: 跟车"
                                       : "Tether: OFF");
        mBtnOrtho.setText(mGLView.nativeGetOrtho() ? "Ortho: ON" : "Ortho: OFF");
    }

    // --- Actions (call through to GLESView native) ---
    private void addDemoVectorLayer() { mGLView.nativeAddDemoVectorLayer(); }
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
