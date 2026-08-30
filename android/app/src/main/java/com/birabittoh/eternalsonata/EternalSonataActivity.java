package com.birabittoh.eternalsonata;

import android.os.Bundle;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

import org.libsdl.app.SDLActivity;

/**
 * Thin wrapper around SDL3's SDLActivity that tells it which native libraries
 * to load and copies bundled assets (guest_shaders.bin) to internal storage
 * where the native code can open them with std::ifstream.
 */
public class EternalSonataActivity extends SDLActivity {

    private static final String TAG = "EternalSonata";

    @Override
    protected String[] getLibraries() {
        return new String[] {
            "rexruntime",
            "eternalsonata",
        };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        copyAssetIfMissing("guest_shaders.bin");
        super.onCreate(savedInstanceState);
    }

    /**
     * Copy an APK asset to the app's internal files directory so the native
     * code can open it with a plain filesystem path.  Skips the copy if the
     * destination already exists (assumes the asset doesn't change between
     * launches of the same APK — a version bump rebuilds it).
     */
    private void copyAssetIfMissing(String name) {
        File dest = new File(getFilesDir(), name);
        if (dest.exists()) return;
        try (InputStream in = getAssets().open(name);
             OutputStream out = new FileOutputStream(dest)) {
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            Log.i(TAG, "Copied " + name + " to " + dest.getAbsolutePath());
        } catch (Exception e) {
            Log.w(TAG, "Failed to copy asset " + name + ": " + e.getMessage());
        }
    }
}
