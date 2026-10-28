package com.birabittoh.eternalsonata;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Process;

/**
 * Starts the game again after EternalSonataActivity.restartApp() has killed
 * its process. It lives in its own process (see the manifest) so it survives
 * that kill, and as a foreground activity it may launch the game from the
 * background, which an alarm or a service no longer may.
 */
public class RestartTrampolineActivity extends Activity {

    static final String EXTRA_NEXT = "next";
    static final String EXTRA_PID = "pid";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Intent next = getIntent().getParcelableExtra(EXTRA_NEXT);
        // Normally gone already; makes sure of it before the new instance maps guest memory.
        int pid = getIntent().getIntExtra(EXTRA_PID, -1);
        if (pid > 0 && pid != Process.myPid()) {
            Process.killProcess(pid);
        }
        if (next != null) {
            startActivity(next);
        }
        finish();
        Runtime.getRuntime().exit(0);
    }
}
