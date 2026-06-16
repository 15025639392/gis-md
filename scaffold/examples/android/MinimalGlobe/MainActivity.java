package com.earthengine.minimalglobe;

import android.app.Activity;
import android.graphics.Color;
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
