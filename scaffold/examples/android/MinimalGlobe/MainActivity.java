package com.earthengine.minimalglobe;

import android.app.Activity;
import android.os.Bundle;

/**
 * 最小地球引擎 Android 示例。
 * 创建 GLSurfaceView + Choreographer 驱动渲染循环。
 */
public class MainActivity extends Activity {

    private GLESView mGLView;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        mGLView = new GLESView(this);
        setContentView(mGLView);
    }

    @Override
    protected void onPause() {
        super.onPause();
        mGLView.onPause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        mGLView.onResume();
    }
}
