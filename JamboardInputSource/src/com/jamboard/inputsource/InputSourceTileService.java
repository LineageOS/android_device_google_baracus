/*
 * Copyright (C) 2024 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 *
 * Quick Settings tile for Jamboard display input source switching.
 * Tap: shows a picker dialog with all 4 input sources.
 *
 * Writes the target to /data/misc/scalerd/input_target, which
 * scalerd polls every 500ms and sends the UART command.
 */
package com.jamboard.inputsource;

import android.app.AlertDialog;
import android.os.SystemProperties;
import android.service.quicksettings.Tile;
import android.service.quicksettings.TileService;
import android.view.WindowManager;

import java.io.FileOutputStream;

public class InputSourceTileService extends TileService {

    private static final String PROP_INPUT_SOURCE = "vendor.scaler.input_source";
    static final String TARGET_FILE = "/data/misc/scalerd/input_target";

    private static final String[] SOURCE_NAMES = {
        "Android", "DisplayPort (USB-C)", "HDMI 1 (Side)", "HDMI 2 (Back)"
    };

    @Override
    public void onStartListening() {
        updateTile();
    }

    @Override
    public void onClick() {
        int current = getInputSource();

        AlertDialog.Builder builder = new AlertDialog.Builder(
                this, android.R.style.Theme_DeviceDefault_Dialog_Alert);
        builder.setTitle("Display Input Source");
        builder.setSingleChoiceItems(SOURCE_NAMES, current, (dialog, which) -> {
            writeTarget(which);
            dialog.dismiss();
            updateTile();
        });
        builder.setNegativeButton("Cancel", null);

        AlertDialog dialog = builder.create();
        dialog.getWindow().setType(WindowManager.LayoutParams.TYPE_STATUS_BAR_SUB_PANEL);
        showDialog(dialog);
    }

    private void writeTarget(int source) {
        try {
            FileOutputStream fos = new FileOutputStream(TARGET_FILE);
            fos.write(String.valueOf(source).getBytes());
            fos.close();
        } catch (Exception e) {
            android.util.Log.e("InputSource", "Failed to write target: " + e);
        }
    }

    private int getInputSource() {
        try {
            return SystemProperties.getInt(PROP_INPUT_SOURCE, 0);
        } catch (Exception e) {
            return 0;
        }
    }

    private void updateTile() {
        Tile tile = getQsTile();
        if (tile == null) return;

        int source = getInputSource();
        String name = (source >= 0 && source < SOURCE_NAMES.length)
            ? SOURCE_NAMES[source] : "Unknown";

        tile.setSubtitle(name);
        tile.setState(Tile.STATE_ACTIVE);
        tile.updateTile();
    }
}
