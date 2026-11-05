package io.github.gminustwo.msrnative;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;

/**
 * In a process of its own (":relaunch"): ends the app's process and starts {@link MsrActivity} again with the
 * arguments it was given, then ends its own. The launcher and the game each get a fresh process this way.
 */
public class RelaunchActivity extends Activity {
    static final String EXTRA_PID = "io.github.gminustwo.msrnative.PID";

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        final int pid = getIntent().getIntExtra(EXTRA_PID, -1);
        if (pid > 0) {
            android.os.Process.killProcess(pid);   // the program ends it too (_exit), whichever comes first
        }
        final Intent next = new Intent(this, MsrActivity.class);
        next.putExtra(MsrActivity.EXTRA_ARGS, getIntent().getStringArrayExtra(MsrActivity.EXTRA_ARGS));
        next.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        startActivity(next);
        finish();
        Runtime.getRuntime().exit(0);
    }
}
