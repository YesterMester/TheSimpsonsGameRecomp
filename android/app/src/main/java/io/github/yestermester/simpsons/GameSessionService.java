package io.github.yestermester.simpsons;

import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.Handler;
import android.os.IBinder;
import android.os.Parcel;
import android.os.RemoteException;

/** Keeps the launcher's session guard alive until the native process exits. */
public final class GameSessionService extends Service {
    static final String DESCRIPTOR = "io.github.yestermester.simpsons.GameSession";
    static final int STATUS = IBinder.FIRST_CALL_TRANSACTION;
    static final int CLAIM = STATUS + 1;
    static final int CANCEL = STATUS + 2;
    private boolean claimed;
    private final Binder session = new Binder() {
        @Override protected synchronized boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                throws RemoteException {
            if (code == IBinder.INTERFACE_TRANSACTION) { reply.writeString(DESCRIPTOR); return true; }
            if (code < STATUS || code > CANCEL) return super.onTransact(code, data, reply, flags);
            data.enforceInterface(DESCRIPTOR);
            boolean value = claimed || GameActivity.active;
            if (code == CLAIM) { value = !value; claimed = true; }
            if (code == CANCEL && !GameActivity.active) {
                new Handler(getMainLooper()).post(() -> android.os.Process.killProcess(android.os.Process.myPid()));
            }
            reply.writeNoException();
            reply.writeInt(value ? 1 : 0);
            return true;
        }
    };
    @Override public IBinder onBind(Intent intent) { return session; }
}
