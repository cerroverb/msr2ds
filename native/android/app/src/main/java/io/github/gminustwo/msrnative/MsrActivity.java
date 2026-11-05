package io.github.gminustwo.msrnative;

import android.content.Intent;
import android.os.Bundle;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

/**
 * msr-native's app: SDL's activity around the program (libmain.so, native/CMakeLists.txt). Its process runs the
 * launcher (no arguments) or the game (the launcher's Play passes the game's arguments), as the PC's launcher and game
 * are processes of their own; {@link #relaunch} starts the app again with other arguments (native/runtime/android.c).
 */
public class MsrActivity extends SDLActivity {
    static final String EXTRA_ARGS = "io.github.gminustwo.msrnative.ARGS";

    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL2", "main"};
    }

    @Override
    protected String[] getArguments() {
        final Intent intent = getIntent();
        final String[] args = intent != null ? intent.getStringArrayExtra(EXTRA_ARGS) : null;
        return args != null ? args : new String[0];
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    /** From the program: the app again, in a new process, with these arguments; this process ends after it. */
    public void relaunch(String[] args) {
        final Intent intent = new Intent(this, RelaunchActivity.class);
        intent.putExtra(EXTRA_ARGS, args);
        intent.putExtra(RelaunchActivity.EXTRA_PID, android.os.Process.myPid());
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(intent);
    }
}
