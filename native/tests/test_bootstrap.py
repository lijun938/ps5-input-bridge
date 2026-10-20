"""Exercise download failure/retry behavior using an actual local HTTP peer."""
import hashlib
import importlib.util
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import tempfile, threading, unittest

SPEC = importlib.util.spec_from_file_location('bridge_bootstrap', Path(__file__).resolve().parents[1]/'scripts/bootstrap.py')
bootstrap = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bootstrap)

class Downloads(unittest.TestCase):
    def test_truncated_download_never_becomes_a_cached_archive_and_retry_succeeds(self):
        class Handler(BaseHTTPRequestHandler):
            count = 0
            def do_GET(self):
                Handler.count += 1
                self.send_response(200)
                self.send_header('Content-Length', '8')
                self.end_headers()
                self.wfile.write(b'abc' if Handler.count==1 else b'abcdefgh')
                self.close_connection = True
            def log_message(self,*args): pass
        server = ThreadingHTTPServer(('127.0.0.1',0),Handler)
        thread = threading.Thread(target=server.serve_forever,daemon=True); thread.start()
        try:
            with tempfile.TemporaryDirectory() as directory:
                dest = Path(directory)/'source.tar.gz'
                url = f'http://127.0.0.1:{server.server_port}/dependency'
                with self.assertRaises((OSError,RuntimeError)):
                    bootstrap.download(url,dest)
                self.assertFalse(dest.exists(),'Interrupted bytes must not be reused as a finished archive')
                self.assertFalse(dest.with_name(dest.name+'.part').exists())
                bootstrap.download(url,dest,hashlib.sha256(b'abcdefgh').hexdigest())
                self.assertEqual(dest.read_bytes(),b'abcdefgh')
                self.assertEqual(Handler.count,2)
        finally: server.shutdown(); server.server_close(); thread.join(2)

if __name__=='__main__': unittest.main(verbosity=2)
