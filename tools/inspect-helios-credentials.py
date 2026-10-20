"""Print the structure of Helios Remote Play credentials without printing any secret values.

Decrypts the DPAPI-protected file for the current Windows user in memory and reports
field names, value types, lengths and character classes only.
"""
import base64, binascii, ctypes, ctypes.wintypes as w, ipaddress, json, os, re, string, sys
from pathlib import Path

DEFAULT = Path(os.environ['LOCALAPPDATA']) / 'HeliosProject/Helios/remote_play/playstation/credentials.dat'

class Blob(ctypes.Structure):
    _fields_ = [('cbData', w.DWORD), ('pbData', ctypes.POINTER(ctypes.c_char))]

def unprotect(data):
    source = Blob(len(data), ctypes.cast(ctypes.c_char_p(data), ctypes.POINTER(ctypes.c_char)))
    result = Blob()
    if not ctypes.windll.crypt32.CryptUnprotectData(ctypes.byref(source), None, None, None, None, 0, ctypes.byref(result)):
        return None, ctypes.GetLastError()
    plain = ctypes.string_at(result.pbData, result.cbData)
    ctypes.windll.kernel32.LocalFree(result.pbData)
    return plain, 0

def classify(text):
    """Describe a string's format without revealing it."""
    traits = [f'len={len(text)}']
    if text != text.strip(): traits.append('has-surrounding-whitespace')
    if re.fullmatch(r'[0-9a-fA-F]+', text): traits.append('hex')
    if re.fullmatch(r'[0-9]+', text): traits.append('digits')
    for name, decode in (('base64', base64.b64decode), ('base64url', base64.urlsafe_b64decode)):
        try:
            decoded = decode(text.strip() + '=' * (-len(text.strip()) % 4))
            if re.fullmatch(r'[A-Za-z0-9+/=_-]+', text.strip()): traits.append(f'{name}->{len(decoded)}B')
            break
        except (binascii.Error, ValueError):
            pass
    try:
        ipaddress.ip_address(text.strip()); traits.append('ip-address')
    except ValueError:
        pass
    if re.fullmatch(r'([0-9a-fA-F]{2}[:-]){5}[0-9a-fA-F]{2}', text): traits.append('mac-with-separators')
    classes = sorted({'upper' if c.isupper() else 'lower' if c.islower() else 'digit' if c.isdigit()
                      else 'space' if c.isspace() else 'non-ascii' if ord(c) > 127 else f'sym:{c}' for c in text})
    traits.append('chars=' + ','.join(classes))
    return ' '.join(traits)

def shape(value):
    if isinstance(value, dict): return {key: shape(item) for key, item in value.items()}
    if isinstance(value, list): return [shape(item) for item in value]
    if isinstance(value, str): return classify(value)
    return type(value).__name__ + ('=' + str(value) if isinstance(value, bool) else '')

path = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT
plain, error = unprotect(path.read_bytes())
if plain is None:
    print(f'DPAPI decrypt failed (error {error}); the file may use additional entropy.')
    sys.exit(1)
print('decrypted size', len(plain))
for encoding in ('utf-8-sig', 'utf-16'):
    try:
        print(f'JSON ({encoding}):', json.dumps(shape(json.loads(plain.decode(encoding))), indent=1, ensure_ascii=False))
        break
    except (UnicodeDecodeError, ValueError):
        pass
else:
    ratio = sum(byte in string.printable.encode() for byte in plain) / len(plain)
    print('not JSON; printable ratio %.2f' % ratio)
    print('identifiers', sorted({m.decode() for m in re.findall(rb'[A-Za-z_]{4,30}', plain)})[:80])
