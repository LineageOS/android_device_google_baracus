/*
 * Copyright (C) 2024 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 *
 * Settings activity for Jamboard display input source selection.
 */
package com.jamboard.inputsource;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemProperties;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.TextView;

import java.io.FileOutputStream;

public class InputSourceActivity extends Activity {

    private static final String PROP_INPUT_SOURCE = "vendor.scaler.input_source";
    private static final String PROP_FIRMWARE = "vendor.scaler.firmware_version";
    private static final String PROP_BRIGHTNESS = "vendor.scaler.brightness";

    private static final int[] SOURCE_STRING_IDS = {
        R.string.source_android,
        R.string.source_displayport,
        R.string.source_hdmi1,
        R.string.source_hdmi2,
    };

    private RadioButton[] radioButtons = new RadioButton[4];
    private Handler handler = new Handler(Looper.getMainLooper());
    private boolean updatingUI = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        int pad = dpToPx(24);
        layout.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(this);
        title.setText(R.string.settings_title);
        title.setTextSize(22);
        title.setPadding(0, 0, 0, dpToPx(16));
        layout.addView(title);

        RadioGroup radioGroup = new RadioGroup(this);
        radioGroup.setOrientation(RadioGroup.VERTICAL);

        for (int i = 0; i < 4; i++) {
            radioButtons[i] = new RadioButton(this);
            radioButtons[i].setText(SOURCE_STRING_IDS[i]);
            radioButtons[i].setTextSize(18);
            radioButtons[i].setPadding(dpToPx(8), dpToPx(12), dpToPx(8), dpToPx(12));
            radioButtons[i].setId(View.generateViewId());
            final int source = i;
            radioButtons[i].setOnClickListener(v -> {
                if (!updatingUI) {
                    writeTarget(source);
                }
            });
            radioGroup.addView(radioButtons[i]);
        }
        layout.addView(radioGroup);

        String fw = SystemProperties.get(PROP_FIRMWARE, "unknown");
        String brightness = SystemProperties.get(PROP_BRIGHTNESS, "?");
        TextView info = new TextView(this);
        info.setText("Scaler firmware: " + fw + " | Brightness: " + brightness + "%");
        info.setTextSize(14);
        info.setPadding(0, dpToPx(24), 0, 0);
        info.setAlpha(0.6f);
        layout.addView(info);

        setContentView(layout);
        startPolling();
    }

    @Override
    protected void onResume() {
        super.onResume();
        updateSelection();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        handler.removeCallbacksAndMessages(null);
    }

    private void writeTarget(int source) {
        try {
            FileOutputStream fos = new FileOutputStream(InputSourceTileService.TARGET_FILE);
            fos.write(String.valueOf(source).getBytes());
            fos.close();
        } catch (Exception e) {
            android.util.Log.e("InputSource", "Failed to write target: " + e);
        }
    }

    private void updateSelection() {
        int current = SystemProperties.getInt(PROP_INPUT_SOURCE, 0);
        updatingUI = true;
        if (current >= 0 && current < 4) {
            radioButtons[current].setChecked(true);
        }
        updatingUI = false;
    }

    private void startPolling() {
        handler.postDelayed(new Runnable() {
            @Override
            public void run() {
                updateSelection();
                handler.postDelayed(this, 1000);
            }
        }, 1000);
    }

    private int dpToPx(int dp) {
        return (int) (dp * getResources().getDisplayMetrics().density);
    }
}
