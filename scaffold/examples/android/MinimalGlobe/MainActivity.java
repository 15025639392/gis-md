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
import android.widget.CompoundButton;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.ToggleButton;

public class MainActivity extends Activity {

    private GLESView mGLView;
    private View mDebugPanel;
    private Button mDebugButton;
    private TextView mDiagnosticsText;
    private ToggleButton mToggleOverlay;
    private ToggleButton mToggleTerrain;
    private ToggleButton mToggleNormalMap;
    private Button mBtnAddVectorLayer;
    private Button mBtnResetCamera;
    private Handler mHandler;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
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
        mDebugButton.setTextSize(20);
        mDebugButton.setTextColor(Color.WHITE);
        mDebugButton.setBackgroundColor(0x88000000);
        FrameLayout.LayoutParams btnParams = new FrameLayout.LayoutParams(
                120, 120, Gravity.TOP | Gravity.END);
        btnParams.setMargins(0, 20, 20, 0);
        mDebugButton.setOnClickListener(v -> toggleDebugPanel());
        root.addView(mDebugButton, btnParams);

        // Debug panel (initially hidden)
        mDebugPanel = createDebugPanel();
        mDebugPanel.setVisibility(View.GONE);
        root.addView(mDebugPanel, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.BOTTOM));

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
        panel.setBackgroundColor(0xDD000000);
        panel.setPadding(32, 32, 32, 32);
        panel.setGravity(Gravity.CENTER_HORIZONTAL);

        TextView title = new TextView(this);
        title.setText("🔧 Debug Panel");
        title.setTextColor(Color.WHITE);
        title.setTextSize(18);
        panel.addView(title);

        // Diagnostics
        mDiagnosticsText = new TextView(this);
        mDiagnosticsText.setTextColor(0xFFAAAAAA);
        mDiagnosticsText.setTextSize(11);
        mDiagnosticsText.setPadding(0, 12, 0, 12);
        panel.addView(mDiagnosticsText);

        // Toggles row 1
        LinearLayout toggles1 = new LinearLayout(this);
        toggles1.setOrientation(LinearLayout.HORIZONTAL);

        mToggleOverlay = new ToggleButton(this);
        mToggleOverlay.setText("Overlay");
        mToggleOverlay.setTextOn("Overlay ON");
        mToggleOverlay.setTextOff("Overlay OFF");
        mToggleOverlay.setOnCheckedChangeListener((btn, on) -> setDebugOverlay(on));
        toggles1.addView(mToggleOverlay);

        mToggleTerrain = new ToggleButton(this);
        mToggleTerrain.setText("Terrain");
        mToggleTerrain.setTextOn("Terrain ON");
        mToggleTerrain.setTextOff("Terrain OFF");
        mToggleTerrain.setOnCheckedChangeListener((btn, on) -> setTerrainEnabled(on));
        toggles1.addView(mToggleTerrain);

        panel.addView(toggles1);

        // Toggles row 2
        LinearLayout toggles2 = new LinearLayout(this);
        toggles2.setOrientation(LinearLayout.HORIZONTAL);

        mToggleNormalMap = new ToggleButton(this);
        mToggleNormalMap.setText("NormalMap");
        mToggleNormalMap.setTextOn("Norm ON");
        mToggleNormalMap.setTextOff("Norm OFF");
        mToggleNormalMap.setOnCheckedChangeListener((btn, on) -> setNormalMapDebug(on));
        toggles2.addView(mToggleNormalMap);

        panel.addView(toggles2);

        // Action buttons
        LinearLayout actions = new LinearLayout(this);
        actions.setOrientation(LinearLayout.HORIZONTAL);
        actions.setGravity(Gravity.CENTER);

        mBtnAddVectorLayer = new Button(this);
        mBtnAddVectorLayer.setText("+ Vector Demo");
        mBtnAddVectorLayer.setOnClickListener(v -> addDemoVectorLayer());
        actions.addView(mBtnAddVectorLayer);

        mBtnResetCamera = new Button(this);
        mBtnResetCamera.setText("Reset Cam");
        mBtnResetCamera.setOnClickListener(v -> resetCamera());
        actions.addView(mBtnResetCamera);

        panel.addView(actions);

        // Close button
        Button closeBtn = new Button(this);
        closeBtn.setText("Close Panel");
        closeBtn.setTextColor(Color.WHITE);
        closeBtn.setBackgroundColor(0x88000000);
        closeBtn.setOnClickListener(v -> toggleDebugPanel());
        panel.addView(closeBtn);

        return new ScrollView(this) {{ addView(panel); }};
    }

    private void toggleDebugPanel() {
        if (mDebugPanel.getVisibility() == View.VISIBLE) {
            mDebugPanel.setVisibility(View.GONE);
        } else {
            mDebugPanel.setVisibility(View.VISIBLE);
            refreshDiagnostics();
            refreshToggles();
        }
    }

    private void refreshDiagnostics() {
        if (mDebugPanel.getVisibility() != View.VISIBLE) return;
        String diag = mGLView.nativeGetDiagnosticsString();
        if (diag != null && !diag.isEmpty()) {
            mDiagnosticsText.setText(diag);
        }
    }

    private void refreshToggles() {
        mToggleOverlay.setChecked(mGLView.nativeGetDebugOverlayEnabled());
        mToggleTerrain.setChecked(mGLView.nativeGetTerrainEnabled());
        mToggleNormalMap.setChecked(mGLView.nativeGetNormalMapDebugEnabled());
    }

    // --- Toggle actions (call through to GLESView native) ---
    private void setDebugOverlay(boolean on) { mGLView.nativeSetDebugOverlay(on); }
    private void setTerrainEnabled(boolean on) { mGLView.nativeSetTerrainEnabled(on); }
    private void setNormalMapDebug(boolean on) { mGLView.nativeSetNormalMapDebug(on); }
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
        mGLView.onResume();
    }
}
