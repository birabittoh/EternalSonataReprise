package com.birabittoh.eternalsonata;

import org.libsdl.app.SDLActivity;

/**
 * Thin wrapper around SDL3's SDLActivity that tells it which native libraries
 * to load.  The ReXGlue SDK statically links SDL3 into librexruntime.so, so
 * there is no separate libSDL3.so.  The load order matters: rexruntime must
 * be loaded before eternalsonata because the latter links against it.
 */
public class EternalSonataActivity extends SDLActivity {

    @Override
    protected String[] getLibraries() {
        return new String[] {
            "rexruntime",
            "eternalsonata",
        };
    }
}
