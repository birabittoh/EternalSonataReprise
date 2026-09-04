package com.birabittoh.eternalsonata;

import android.app.AlertDialog;
import android.content.DialogInterface;
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

    /**
     * Called from native code (host_menu.cpp) to open the back-button host
     * menu. A real AlertDialog rather than an in-engine overlay so it draws
     * as its own window above both the game surface and the on-screen touch
     * controls, which share a render target with any in-engine overlay.
     */
    public void showHostMenu(final String[] items) {
        runOnUiThread(() -> {
            new AlertDialog.Builder(EternalSonataActivity.this)
                .setItems(items, (DialogInterface dialog, int which) -> nativeHostMenuSelect(which))
                .setOnCancelListener((DialogInterface dialog) -> nativeHostMenuSelect(-1))
                .show();
        });
    }

    /**
     * Called from native code to list installed DLC. Picking an entry asks for
     * confirmation before removing it, since there is no undo.
     */
    public void showDlcManager(final String[] labels) {
        runOnUiThread(() -> {
            new AlertDialog.Builder(EternalSonataActivity.this)
                .setTitle("Installed DLC")
                .setItems(labels, (DialogInterface dialog, int which) ->
                    new AlertDialog.Builder(EternalSonataActivity.this)
                        .setTitle("Remove DLC?")
                        .setMessage(labels[which])
                        .setNegativeButton("Cancel", null)
                        .setPositiveButton("Remove",
                            (DialogInterface d, int w) -> nativeDlcRemove(which))
                        .show())
                .show();
        });
    }

    /** Called from native code to report the outcome of a menu action. */
    public void showHostToast(final String message) {
        runOnUiThread(() ->
            android.widget.Toast.makeText(this, message, android.widget.Toast.LENGTH_LONG).show());
    }

    // ---------------------------------------------------------------------
    // Storage Access Framework.
    //
    // Android 11+ hides Android/data (where user_data_root lives) from file
    // managers, so importing and exporting has to cross the sandbox. SAF hands
    // back a content:// URI, which cannot be opened with fopen; rather than
    // rerouting the native save and content code onto SAF file descriptors,
    // every transfer is staged through a plain file in the cache directory and
    // only the copy at the edge speaks content://.
    // ---------------------------------------------------------------------

    private static final int REQUEST_BASE = 0x4E4D;

    /** The op code passed back to native, and the export destination once picked. */
    private int pendingOp = -1;
    private android.net.Uri pendingExportUri;

    public void requestOpenDocument(final int op) {
        runOnUiThread(() -> {
            pendingOp = op;
            android.content.Intent intent =
                new android.content.Intent(android.content.Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(android.content.Intent.CATEGORY_OPENABLE);
            // STFS packages and our own archives have no reliable MIME type,
            // and a picker hides whatever it cannot name.
            intent.setType("*/*");
            startActivityForResult(intent, REQUEST_BASE + op);
        });
    }

    public void requestCreateDocument(final int op) {
        runOnUiThread(() -> {
            pendingOp = op;
            android.content.Intent intent =
                new android.content.Intent(android.content.Intent.ACTION_CREATE_DOCUMENT);
            intent.addCategory(android.content.Intent.CATEGORY_OPENABLE);
            intent.setType("application/zip");
            intent.putExtra(android.content.Intent.EXTRA_TITLE, "eternalsonata-saves.zip");
            startActivityForResult(intent, REQUEST_BASE + op);
        });
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, android.content.Intent data) {
        if (requestCode < REQUEST_BASE || resultCode != RESULT_OK || data == null
                || data.getData() == null) {
            super.onActivityResult(requestCode, resultCode, data);
            return;
        }
        final int op = requestCode - REQUEST_BASE;
        final android.net.Uri uri = data.getData();
        // Off the UI thread: this copies files, and nativeDocumentReady blocks
        // on the SDL thread finishing the work.
        new Thread(() -> {
            if (op == pendingOp && uri != null) {
                if (isExport(op)) {
                    exportTo(op, uri);
                } else {
                    importFrom(op, uri);
                }
            }
        }, "host-menu-transfer").start();
    }

    private boolean isExport(int op) {
        // Matches DocumentOp::kOpExportSaves in host_menu.cpp.
        return op == 2;
    }

    /** Stage the picked document as a cache file, then let native consume it. */
    private void importFrom(int op, android.net.Uri uri) {
        File staged = new File(getCacheDir(), "hostmenu_import.tmp");
        try (InputStream in = getContentResolver().openInputStream(uri);
             OutputStream out = new FileOutputStream(staged)) {
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        } catch (Exception e) {
            Log.w(TAG, "Import staging failed: " + e.getMessage());
            showHostToast("Could not read that file.");
            return;
        }
        nativeDocumentReady(op, staged.getAbsolutePath());
        staged.delete();
    }

    /** Let native write the archive, then copy it out to the picked document. */
    private void exportTo(int op, android.net.Uri uri) {
        String produced = nativeDocumentReady(op, null);
        if (produced == null) {
            // Native already reported why.
            return;
        }
        File source = new File(produced);
        try (InputStream in = new java.io.FileInputStream(source);
             OutputStream out = getContentResolver().openOutputStream(uri)) {
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        } catch (Exception e) {
            Log.w(TAG, "Export copy failed: " + e.getMessage());
            showHostToast("Could not write to that location.");
            return;
        } finally {
            source.delete();
        }
        showHostToast("Saves exported.");
    }

    private native void nativeHostMenuSelect(int index);
    private native void nativeDlcRemove(int index);
    private native String nativeDocumentReady(int op, String path);
}
