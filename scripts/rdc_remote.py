#!/usr/bin/env python3
"""Replay a RenderDoc capture on the Android device it was taken on.

Android captures can only be replayed by a remote server running on the
phone, so this talks to the RenderDoc APK over adb using the python module
built in ../renderdoc (which must match the capture's RenderDoc version).

Usage:
  rdc_remote.py serve <capture.rdc>    keep a replay session open; it runs
                                       snippets dropped into <state>/cmd.py
                                       and writes their stdout to <state>/out.txt
  rdc_remote.py q [file.py]            send a snippet (file or stdin) to the
                                       session and print its output
  rdc_remote.py stop                   shut the session down

Snippets run with `rd` (the renderdoc module), `ctrl` (ReplayController) and
`remote` (RemoteServer) in scope. Set QUIT=1 to end the session.
"""
import contextlib, io, os, sys, time, traceback

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RD_BUILD = os.environ.get('RENDERDOC_BUILD', os.path.join(ROOT, '..', 'renderdoc', 'build'))
STATE = os.environ.get('RDC_STATE', os.path.join(ROOT, 'out', 'rdc_remote'))
CMD, OUT, READY = (os.path.join(STATE, n) for n in ('cmd.py', 'out.txt', 'ready'))


def serve(capture):
    sys.path.insert(0, os.path.join(RD_BUILD, 'lib'))
    import renderdoc as rd
    os.makedirs(STATE, exist_ok=True)
    for f in (CMD, OUT, READY):
        if os.path.exists(f): os.remove(f)
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    proto = rd.GetDeviceProtocolController('adb')
    devs = proto.GetDevices()
    if not devs: sys.exit('no adb devices')
    url = 'adb://' + devs[0]
    print('device:', url, flush=True)
    res, remote = rd.CreateRemoteServerConnection(url)
    if res != rd.ResultCode.Succeeded:
        print('starting remote server:', proto.StartRemoteServer(url), flush=True)
        for _ in range(60):
            res, remote = rd.CreateRemoteServerConnection(url)
            if res == rd.ResultCode.Succeeded: break
            time.sleep(1)
    if res != rd.ResultCode.Succeeded: sys.exit(f'connect failed: {res}')
    path = remote.CopyCaptureToRemote(os.path.abspath(capture), None)
    print('remote path:', path, flush=True)
    res, ctrl = remote.OpenCapture(rd.RemoteServer.NoPreference, path, rd.ReplayOptions(), None)
    if res != rd.ResultCode.Succeeded: sys.exit(f'open failed: {res}')
    print('ready', flush=True)
    g = {'rd': rd, 'ctrl': ctrl, 'remote': remote}
    open(READY, 'w').write('1')
    try:
        while 'QUIT' not in g:
            if not os.path.exists(CMD):
                # The server drops idle connections; qrenderdoc pings on a timer too.
                remote.Ping()
                time.sleep(0.2); continue
            src = open(CMD).read(); os.remove(CMD)
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                try: exec(src, g)
                except Exception: traceback.print_exc(file=buf)
            open(OUT + '.tmp', 'w').write(buf.getvalue())
            os.rename(OUT + '.tmp', OUT)
    finally:
        if os.path.exists(READY): os.remove(READY)
        ctrl.Shutdown()
        remote.ShutdownServerAndConnection()


def query(src, timeout=600):
    if not os.path.exists(READY): sys.exit('no session (run `serve` first)')
    if os.path.exists(OUT): os.remove(OUT)
    open(CMD + '.tmp', 'w').write(src); os.rename(CMD + '.tmp', CMD)
    for _ in range(timeout * 4):
        if os.path.exists(OUT):
            sys.stdout.write(open(OUT).read()); return
        time.sleep(0.25)
    sys.exit('timeout')


if __name__ == '__main__':
    a = sys.argv[1:]
    if a[:1] == ['serve']: serve(a[1])
    elif a[:1] == ['q']: query(open(a[1]).read() if len(a) > 1 else sys.stdin.read())
    elif a[:1] == ['stop']: query('QUIT=1')
    else: sys.exit(__doc__)
