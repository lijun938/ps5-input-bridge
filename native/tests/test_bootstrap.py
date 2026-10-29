"""Exercise download failure/retry behavior using an actual local HTTP peer."""
import hashlib
import importlib.util
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import json, tarfile, tempfile, threading, unittest

SPEC = importlib.util.spec_from_file_location('bridge_bootstrap', Path(__file__).resolve().parents[1]/'scripts/bootstrap.py')
bootstrap = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bootstrap)

class Downloads(unittest.TestCase):
    def test_pinned_sources_accept_all_patches_twice(self):
        archive = bootstrap.DEPS/f'chiaki-{bootstrap.hashlib.sha256(bootstrap.SOURCES[0][1].encode()).hexdigest()[:12]}.tar.gz'
        if not archive.is_file():
            self.skipTest('Run the native bootstrap to populate pinned source archives first')
        edits = []
        for name in ('chiaki-edits.json', 'chiaki-low-latency.json'):
            edits.extend(json.loads((bootstrap.ROOT/'native/patches'/name).read_text(encoding='utf-8')))
        targets = {edit['file'] for edit in edits}
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory)
            with tarfile.open(archive) as bundle:
                members = {member.name.split('/', 1)[1]: member for member in bundle
                           if member.isfile() and '/' in member.name}
                for relative in targets:
                    target = source/relative
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(bundle.extractfile(members[relative]).read())
            original = bootstrap.CHIAKI
            try:
                bootstrap.CHIAKI = source
                bootstrap.apply_patches()
                first = {relative: (source/relative).read_bytes() for relative in targets}
                bootstrap.apply_patches()
                self.assertEqual(first, {relative: (source/relative).read_bytes() for relative in targets})
                for edit in edits:
                    self.assertIn(edit['after'], (source/edit['file']).read_text(encoding='utf-8'))
            finally:
                bootstrap.CHIAKI = original

    def test_all_patch_sets_apply_repeatably(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root/'upstream'
            source.mkdir()
            patches = root/'native/patches'
            (patches/'chiaki').mkdir(parents=True)
            target = source/'source.c'
            target.write_text('error_old\nlatency_old\n', encoding='utf-8')
            for name, before, after in [('chiaki-edits.json', 'error_old', 'error_new'),
                                        ('chiaki-low-latency.json', 'latency_old', 'latency_new')]:
                (patches/name).write_text(json.dumps([{'file':'source.c', 'before':before, 'after':after}]))
            original = bootstrap.ROOT, bootstrap.CHIAKI
            try:
                bootstrap.ROOT, bootstrap.CHIAKI = root, source
                bootstrap.apply_patches()
                bootstrap.apply_patches()
                self.assertEqual(target.read_text(), 'error_new\nlatency_new\n')
            finally:
                bootstrap.ROOT, bootstrap.CHIAKI = original

    def test_checked_patch_is_repeatable_and_rejects_unexpected_source(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root/'source.c'
            target.write_text('before\n', encoding='utf-8')
            edits = [{'file':'source.c', 'before':'before\n', 'after':'after\n'}]
            bootstrap.apply_checked_edits(root, edits)
            bootstrap.apply_checked_edits(root, edits)
            self.assertEqual(target.read_text(), 'after\n')
            target.write_text('changed baseline\n', encoding='utf-8')
            with self.assertRaises(RuntimeError):
                bootstrap.apply_checked_edits(root, edits)
            self.assertEqual(target.read_text(), 'changed baseline\n')

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

    def test_changed_pin_replaces_extracted_source_and_never_reuses_old_archive(self):
        import io, tarfile, time
        def archive(text):
            data = io.BytesIO()
            with tarfile.open(fileobj=data, mode='w:gz') as tar:
                payload = text.encode()
                info = tarfile.TarInfo('top/file.txt'); info.size = len(payload)
                tar.addfile(info, io.BytesIO(payload))
            return data.getvalue()
        bodies = {'/old': archive('old pin'), '/new': archive('new pin')}
        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                body = bodies[self.path]
                self.send_response(200)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            def log_message(self,*args): pass
        server = ThreadingHTTPServer(('127.0.0.1',0),Handler)
        thread = threading.Thread(target=server.serve_forever,daemon=True); thread.start()
        saved_deps = bootstrap.DEPS
        try:
            with tempfile.TemporaryDirectory() as directory:
                bootstrap.DEPS = Path(directory)/'deps'; bootstrap.DEPS.mkdir()
                dest = Path(directory)/'source'
                base = f'http://127.0.0.1:{server.server_port}'
                bootstrap.source(('dep', base+'/old', dest))
                (dest/'stale-from-old-pin.txt').write_text('x')
                extracted_after = time.time() - 1
                bootstrap.source(('dep', base+'/new', dest))
                self.assertGreater((dest/'file.txt').stat().st_mtime, extracted_after)
                self.assertEqual((dest/'file.txt').read_text(), 'new pin')
                self.assertFalse((dest/'stale-from-old-pin.txt').exists())
                bootstrap.source(('dep', base+'/new', dest))  # same pin: kept as is
                self.assertEqual((dest/'file.txt').read_text(), 'new pin')
        finally:
            bootstrap.DEPS = saved_deps
            server.shutdown(); server.server_close(); thread.join(2)

if __name__=='__main__': unittest.main(verbosity=2)
