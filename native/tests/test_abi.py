"""Offline tests exercise the actual DLL; no PS5/account secrets needed."""
import ctypes as c
from pathlib import Path
import unittest
import socket, threading, time
from concurrent.futures import ThreadPoolExecutor

DLL = Path(__file__).resolve().parents[1] / 'build' / 'ps5bridge.dll'
class Keys(c.Structure):
    _fields_ = [('account', c.c_ubyte*8), ('regist', c.c_ubyte*16), ('morning', c.c_ubyte*16),
                ('mac', c.c_ubyte*6), ('reserved', c.c_ubyte*2), ('target', c.c_int32), ('nickname', c.c_char*32)]
class Status(c.Structure):
    _fields_ = [('state',c.c_uint32),('index',c.c_uint32),('present',c.c_uint32),('error',c.c_int32),
                ('reports',c.c_uint64),('samples',c.c_uint64),('message',c.c_char*256)]
class Timing(c.Structure):
    _fields_ = [('count',c.c_uint64),('mean',c.c_double),('max',c.c_double)]
class InputDebug(c.Structure):
    _fields_ = [('buttons',c.c_uint32),('l2',c.c_uint32),('r2',c.c_uint32),('present',c.c_uint32),
                ('observed',c.c_uint64),('failures',c.c_uint64),('l1',c.c_uint64),('r1',c.c_uint64),('chords',c.c_uint64)]

class NativeABI(unittest.TestCase):
    def test_input_health_snapshot_before_first_read(self):
        lib = self.open_library()
        class InputHealth(c.Structure):
            _fields_ = [('reads', c.c_uint64), ('changes', c.c_uint64), ('failures', c.c_uint64),
                ('read_age', c.c_double), ('change_age', c.c_double), ('read_duration', c.c_double),
                ('packet', c.c_uint32), ('error', c.c_uint32),
                ('lx', c.c_int16), ('ly', c.c_int16), ('rx', c.c_int16), ('ry', c.c_int16),
                ('buttons', c.c_uint32), ('l2', c.c_uint32), ('r2', c.c_uint32), ('stalls', c.c_uint32)]
        self.assertEqual(c.sizeof(InputHealth), 80)
        self.assertTrue(hasattr(lib, 'pb_get_input_health'), 'native DLL must expose input-read health')
        lib.pb_get_input_health.argtypes = [c.c_void_p, c.POINTER(InputHealth)]
        ctx = lib.pb_create()
        try:
            health = InputHealth()
            self.assertEqual(lib.pb_get_input_health(ctx, c.byref(health)), 0)
            self.assertEqual((health.reads, health.changes, health.failures), (0, 0, 0))
            self.assertEqual(health.stalls, 0)
            self.assertEqual((health.read_age, health.change_age), (-1, -1))
            self.assertEqual(lib.pb_get_input_health(None, c.byref(health)), -1)
            self.assertEqual(lib.pb_get_input_health(ctx, None), -1)
        finally:
            lib.pb_destroy(ctx)

    def test_stable_controller_identity_and_independent_status_contract(self):
        lib = self.open_library()
        class ControllerStatus(c.Structure):
            _fields_ = [('state', c.c_uint32), ('error', c.c_uint32)]
        self.assertEqual(c.sizeof(ControllerStatus), 8)
        lib.pb_set_controller_device.argtypes = [c.c_void_p, c.c_uint32, c.c_char_p]
        lib.pb_get_controller_status.argtypes = [c.c_void_p, c.POINTER(ControllerStatus)]
        lib.pb_start_device.argtypes = [c.c_void_p, c.c_char_p, c.POINTER(Keys), c.c_uint32, c.c_char_p]
        ctx = lib.pb_create()
        try:
            self.assertEqual(lib.pb_set_controller_device(ctx, 4, None), -1)
            self.assertEqual(lib.pb_set_controller_device(ctx, 4, b''), -1)
            self.assertEqual(lib.pb_set_controller_device(ctx, 4, b'a'*1536), -1)
            self.assertEqual(lib.pb_set_controller_device(ctx, 4, b'hid:selected'), 0)
            self.assertEqual(lib.pb_set_controller_device(ctx, 0, b'hid:selected'), -1)
            self.assertEqual(lib.pb_set_controller_device(ctx, 0, None), 0)
            status, controller = Status(), ControllerStatus()
            self.assertEqual(lib.pb_get_status(ctx, c.byref(status)), 0)
            self.assertEqual(lib.pb_get_controller_status(ctx, c.byref(controller)), 0)
            self.assertEqual((status.state, controller.state, controller.error), (0, 0, 0))
            self.assertEqual(lib.pb_start_device(ctx, b'127.0.0.1', c.byref(Keys()), 4, None), -1)
        finally:
            lib.pb_destroy(ctx)

    def test_latency_statistics_contract_and_empty_window(self):
        lib = self.open_library()
        class Latency(c.Structure):
            _fields_ = [('submissions',c.c_uint64),('intervals',c.c_uint64),
                ('submit_p95',c.c_double),('submit_p99',c.c_double),
                ('interval_mean',c.c_double),('interval_p95',c.c_double),
                ('interval_p99',c.c_double),('interval_max',c.c_double),('failures',c.c_uint64)]
        self.assertEqual(c.sizeof(Latency),72)
        self.assertTrue(hasattr(lib,'pb_get_latency_stats'))
        lib.pb_get_latency_stats.argtypes=[c.c_void_p,c.POINTER(Latency)]
        ctx=lib.pb_create()
        try:
            result=Latency()
            self.assertEqual(lib.pb_get_latency_stats(ctx,c.byref(result)),0)
            self.assertEqual((result.submissions,result.intervals,result.failures),(0,0,0))
            self.assertEqual((result.submit_p95,result.submit_p99,result.interval_p95,result.interval_p99),(0,0,0,0))
        finally:lib.pb_destroy(ctx)

    def test_new_diagnostics_and_login_generation_contract(self):
        lib = self.open_library()
        self.assertEqual(lib.pb_abi_version(), 6)
        self.assertEqual(c.sizeof(InputDebug),56)
        self.assertTrue(hasattr(lib,'pb_get_input_debug'),'New input diagnostics are missing')
        self.assertTrue(hasattr(lib,'pb_login_request_serial'),'Login request generations are missing')
        class HidInfo(c.Structure):
            _fields_=[('vid',c.c_uint16),('pid',c.c_uint16),('layout',c.c_uint32),('name',c.c_char*64),('device_id',c.c_char*1536)]
        self.assertEqual(c.sizeof(HidInfo),1608)
        lib.pb_hid_devices.argtypes=[c.POINTER(HidInfo),c.c_uint32]
        lib.pb_hid_devices.restype=c.c_uint32
        devices=(HidInfo*16)()
        count=lib.pb_hid_devices(devices,16)
        lib.pb_controller_mask.restype=c.c_uint32
        mask=lib.pb_controller_mask()
        for i in range(min(count,16)):
            self.assertIn(devices[i].layout,(1,2))
            self.assertTrue(devices[i].device_id)
            self.assertTrue(mask & (1<<(4+i)),'Every listed HID pad must appear in the controller mask')
        self.assertEqual(lib.pb_hid_devices(None,0),count,'Capacity 0 must still report the count')
        lib.pb_get_input_debug.argtypes=[c.c_void_p,c.POINTER(InputDebug)]
        lib.pb_login_request_serial.argtypes=[c.c_void_p]
        lib.pb_login_request_serial.restype=c.c_uint64
        ctx=lib.pb_create()
        try:
            debug=InputDebug()
            self.assertEqual(lib.pb_get_input_debug(ctx,c.byref(debug)),0)
            self.assertEqual((debug.buttons,debug.l2,debug.r2,debug.observed,debug.failures),(0,0,0,0,0))
            self.assertEqual(lib.pb_login_request_serial(ctx),0)
            self.assertEqual(lib.pb_stop(ctx),0)
        finally:lib.pb_destroy(ctx)

    def open_library(self):
        self.assertTrue(DLL.exists(), 'Native bridge implementation has not produced its DLL yet')
        lib = c.CDLL(str(DLL))
        lib.pb_create.restype = c.c_void_p
        for name in ('pb_destroy','pb_stop','pb_set_controller','pb_set_poll_interval','pb_button'):
            getattr(lib,name).argtypes = [c.c_void_p] + ([c.c_uint32,c.c_uint32] if name=='pb_button' else
                [c.c_uint32] if name in ('pb_set_controller','pb_set_poll_interval') else [])
        lib.pb_start.argtypes = [c.c_void_p,c.c_char_p,c.POINTER(Keys),c.c_uint32]
        lib.pb_register.argtypes = [c.c_void_p,c.c_char_p,c.POINTER(c.c_ubyte),c.c_uint32,c.c_uint32,c.POINTER(Keys)]
        lib.pb_get_status.argtypes = [c.c_void_p,c.POINTER(Status)]
        lib.pb_get_timing_stats.argtypes = [c.c_void_p,c.POINTER(Timing)]
        lib.pb_set_login_pin.argtypes = [c.c_void_p,c.c_char_p]
        return lib

    def test_bridge_contract_and_lifetime(self):
        lib = self.open_library()
        self.assertEqual((c.sizeof(Keys), c.sizeof(Status), c.sizeof(Timing)), (84,288,24))
        self.assertEqual(lib.pb_abi_version(),6)
        self.assertEqual(lib.pb_self_test(),0, 'Native XInput conversion and neutral-release self-test failed')
        for _ in range(20):
            ctx = lib.pb_create()
            self.assertTrue(ctx)
            try:
                state = Status(); timing = Timing(); keys = Keys(); account = (c.c_ubyte*8)()
                self.assertEqual(lib.pb_get_status(ctx,c.byref(state)),0)
                self.assertEqual(state.state,0)
                self.assertEqual(lib.pb_start(ctx,b'127.0.0.1',c.byref(keys),0),-1)
                self.assertEqual(lib.pb_start(ctx,b'',c.byref(keys),0),-1)
                self.assertEqual(lib.pb_register(ctx,b'127.0.0.1',account,12345678,1000,c.byref(keys)),-1)
                self.assertEqual(lib.pb_set_controller(ctx,20),-1)
                self.assertEqual(lib.pb_set_controller(ctx,4),0)
                self.assertEqual(lib.pb_set_controller(ctx,0),0)
                self.assertEqual(lib.pb_set_poll_interval(ctx,0),-1)
                self.assertEqual(lib.pb_set_poll_interval(ctx,500),0)
                self.assertEqual(lib.pb_set_poll_interval(ctx,1000),0)
                self.assertEqual(lib.pb_set_poll_interval(ctx,100),0)
                self.assertEqual(lib.pb_set_poll_interval(ctx,250),-1)
                self.assertEqual(lib.pb_button(ctx,4,1),-1)
                lib.pb_set_back_mapping.argtypes=[c.c_void_p,c.c_uint32]
                self.assertEqual(lib.pb_set_back_mapping(ctx,2),-1)
                self.assertEqual(lib.pb_set_back_mapping(ctx,1),0)
                self.assertEqual(lib.pb_set_back_mapping(ctx,0),0)
                self.assertEqual(lib.pb_set_login_pin(ctx,b'bad'),-1)
                self.assertEqual(lib.pb_stop(ctx),0)
                self.assertEqual(lib.pb_stop(ctx),0)
                self.assertEqual(lib.pb_get_status(ctx,c.byref(state)),0)
                self.assertEqual(state.state,5)
                self.assertEqual(lib.pb_get_timing_stats(ctx,c.byref(timing)),0)
                self.assertEqual(timing.count,0)
            finally: lib.pb_destroy(ctx)

    def test_registration_can_be_cancelled_without_waiting_for_timeout(self):
        lib = self.open_library()
        ctx = lib.pb_create(); keys = Keys(); account = (c.c_ubyte*8)(1,0,0,0,0,0,0,0)
        peer=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); peer.bind(('127.0.0.1',9295))
        try:
            with ThreadPoolExecutor(max_workers=1) as pool:
                pending = pool.submit(lib.pb_register,ctx,b'127.0.0.1',account,12345678,30000,c.byref(keys))
                state = Status(); deadline = time.monotonic()+2
                while time.monotonic()<deadline:
                    lib.pb_get_status(ctx,c.byref(state))
                    if state.state==1: break
                    time.sleep(.001)
                self.assertEqual(state.state,1)
                started = time.monotonic()
                self.assertEqual(lib.pb_stop(ctx),0)
                self.assertEqual(pending.result(timeout=2),-4)
                self.assertLess(time.monotonic()-started,2)
        finally: peer.close(); lib.pb_destroy(ctx)

    def test_registration_context_can_be_reused_while_status_is_read_concurrently(self):
        lib = self.open_library(); ctx = lib.pb_create(); finished = threading.Event()
        peer=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); peer.bind(('127.0.0.1',9295))
        errors = []
        def reader():
            status=Status(); timing=Timing()
            while not finished.is_set():
                if lib.pb_get_status(ctx,c.byref(status))!=0 or lib.pb_get_timing_stats(ctx,c.byref(timing))!=0:
                    errors.append('Read failed')
                    break
                time.sleep(.0005)
        reader_thread=threading.Thread(target=reader); reader_thread.start()
        try:
            with ThreadPoolExecutor(max_workers=1) as pool:
                for cycle in range(8):
                    keys=Keys(); account=(c.c_ubyte*8)(cycle+1,0,0,0,0,0,0,0)
                    pending=pool.submit(lib.pb_register,ctx,b'127.0.0.1',account,12345678,30000,c.byref(keys))
                    status=Status(); deadline=time.monotonic()+2
                    while time.monotonic()<deadline:
                        lib.pb_get_status(ctx,c.byref(status))
                        if status.state==1: break
                        time.sleep(.0005)
                    self.assertEqual(status.state,1)
                    self.assertEqual(lib.pb_stop(ctx),0)
                    self.assertEqual(pending.result(timeout=2),-4)
                    self.assertEqual(bytes(keys.regist),bytes(16),'Failed registration must not expose stale keys')
            self.assertEqual(errors,[])
        finally:
            finished.set(); reader_thread.join(2); peer.close(); lib.pb_destroy(ctx)

    def test_stop_releases_threads_while_console_response_is_pending(self):
        lib = self.open_library()
        ctx = lib.pb_create(); keys = Keys(); keys.target=1000100
        keys.account[0]=keys.regist[0]=keys.morning[0]=1
        listener=socket.socket(); listener.bind(('127.0.0.1',9295)); listener.listen(1); listener.settimeout(2)
        release=threading.Event(); accepted=threading.Event()
        def server():
            try:
                conn,_=listener.accept()
                with conn:
                    accepted.set()
                    release.wait(2)
            except OSError: pass
        worker=threading.Thread(target=server); worker.start()
        try:
            self.assertEqual(lib.pb_start(ctx,b'127.0.0.1',c.byref(keys),0),0)
            self.assertTrue(accepted.wait(2))
            time.sleep(.025)
            state=Status(); timing=Timing()
            lib.pb_get_status(ctx,c.byref(state)); lib.pb_get_timing_stats(ctx,c.byref(timing))
            self.assertEqual(state.state,2, 'The fake console never authenticates; connected must not be reported')
            self.assertGreater(state.samples,1)
            self.assertEqual(state.reports,0, 'Handshake caching is not a connected submission')
            self.assertEqual((timing.count,timing.mean,timing.max),(0,0,0))
            started=time.monotonic()
            self.assertEqual(lib.pb_stop(ctx),0)
            self.assertLess(time.monotonic()-started,2)
            lib.pb_get_status(ctx,c.byref(state))
            self.assertEqual(state.state,5)
            final_samples=state.samples
            time.sleep(.01)
            lib.pb_get_status(ctx,c.byref(state))
            self.assertEqual(state.samples,final_samples,'Input thread must be joined after stop')
        finally:
            release.set(); listener.close(); worker.join(2); lib.pb_destroy(ctx)

if __name__ == '__main__': unittest.main(verbosity=2)
