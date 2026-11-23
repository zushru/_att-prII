# Hash-pinned, temporary patch for the attendance target's bundled WebServer.
Import("env")

import atexit
import errno
import hashlib
import json
import os
import socket
import stat
import tempfile
from pathlib import Path

EXPECTED_ENV = "att"
PACKAGE_NAME = "framework-arduinoespressif32"
REVIEWED_PACKAGE_VERSION = "4.30312.261116"
WEBSERVER_VERSION = "3.3.12"
PARSING_BEFORE_SHA256 = "c24f1e65b4ce3fb7ab868f6bca7aab7af5dcdfd1ca81ce9716752a922e52ce78"


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def fail(message):
    raise RuntimeError("ATT WebServer patch refused: " + message)


def atomic_write(path, data, mode):
    fd, temporary = tempfile.mkstemp(prefix="." + path.name + ".att-", dir=str(path.parent))
    try:
        with os.fdopen(fd, "wb") as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        os.chmod(temporary, mode)
        os.replace(temporary, str(path))
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def property_version(path):
    values = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if "=" in line and not line.lstrip().startswith("#"):
            key, value = line.split("=", 1)
            values[key.strip()] = value.strip()
    return values.get("version")


def _windows_process_is_alive(pid):
    """Check a process without using os.kill(), which is not a safe probe on Windows."""
    if pid > 0xFFFFFFFF:
        # Windows process IDs are DWORD values; do not let ctypes wrap a
        # malformed lock-file PID onto an unrelated live process.
        return True

    import ctypes

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    open_process = kernel32.OpenProcess
    open_process.argtypes = (ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32)
    open_process.restype = ctypes.c_void_p
    wait_for_single_object = kernel32.WaitForSingleObject
    wait_for_single_object.argtypes = (ctypes.c_void_p, ctypes.c_uint32)
    wait_for_single_object.restype = ctypes.c_uint32
    close_handle = kernel32.CloseHandle
    close_handle.argtypes = (ctypes.c_void_p,)
    close_handle.restype = ctypes.c_int

    synchronize = 0x00100000
    wait_object_0 = 0x00000000
    wait_timeout = 0x00000102
    error_invalid_parameter = 87

    handle = open_process(synchronize, 0, pid)
    if not handle:
        # OpenProcess reports ERROR_INVALID_PARAMETER when the PID no longer
        # identifies a process. Access-denied and unknown errors are treated
        # as alive so lock recovery always fails closed.
        return ctypes.get_last_error() != error_invalid_parameter

    try:
        result = wait_for_single_object(handle, 0)
        if result == wait_object_0:
            return False
        if result == wait_timeout:
            return True
        # WAIT_FAILED and any unexpected result must not permit stale-lock
        # removal while process state is uncertain.
        return True
    finally:
        close_handle(handle)


def process_is_alive(pid):
    if os.name == "nt":
        return _windows_process_is_alive(pid)

    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    except OSError as error:
        if error.errno == errno.ESRCH:
            return False
        # Unknown process-query errors are treated as alive. A false negative
        # here could let a second build rewrite the shared framework mid-compile.
        return True


def acquire_build_lock(path):
    """Serialize ATT builds that temporarily patch the shared framework file."""
    owner = {
        "pid": os.getpid(),
        "host": socket.gethostname(),
    }
    owner_bytes = json.dumps(owner, sort_keys=True).encode("ascii")
    flags = os.O_CREAT | os.O_EXCL | os.O_WRONLY
    for attempt in range(2):
        try:
            descriptor = os.open(str(path), flags, 0o600)
        except FileExistsError:
            try:
                existing_bytes = path.read_bytes()
                existing = json.loads(existing_bytes.decode("ascii"))
                if not isinstance(existing, dict):
                    raise ValueError("lock payload is not an object")
                existing_pid = existing.get("pid")
                existing_host = existing.get("host")
                if (
                    isinstance(existing_pid, bool)
                    or not isinstance(existing_pid, int)
                    or existing_pid <= 0
                    or not isinstance(existing_host, str)
                    or not existing_host
                ):
                    raise ValueError("lock owner fields are invalid")
            except (OSError, ValueError, KeyError, TypeError, UnicodeDecodeError):
                fail(
                    "an unreadable WebServer patch lock exists; verify no ATT "
                    "build is running, then remove " + str(path)
                )
            if existing_host != socket.gethostname():
                fail(
                    "the WebServer patch lock belongs to another host; refusing "
                    "to modify the shared framework"
                )
            if existing_pid <= 0 or process_is_alive(existing_pid):
                fail(
                    "another ATT build is using the shared WebServer framework "
                    "(PID " + str(existing_pid) + ")"
                )
            # A dead same-host owner is a forced-termination remnant. Compare
            # before unlinking so a replaced lock is never removed accidentally.
            try:
                if path.read_bytes() != existing_bytes:
                    fail("the WebServer patch lock changed during stale-lock recovery")
                path.unlink()
            except OSError as error:
                fail("cannot remove stale WebServer patch lock: " + str(error))
            continue
        try:
            with os.fdopen(descriptor, "wb") as lock_file:
                lock_file.write(owner_bytes)
                lock_file.flush()
                os.fsync(lock_file.fileno())
        except OSError as error:
            try:
                path.unlink()
            except OSError:
                pass
            fail("cannot persist WebServer patch lock: " + str(error))
        return owner_bytes
    fail("could not acquire the exclusive WebServer patch lock")


def release_build_lock(path, owner_bytes):
    try:
        if path.exists() and path.read_bytes() == owner_bytes:
            path.unlink()
    except OSError as error:
        print("ATT WebServer patch warning: could not release build lock: " + str(error))


if env.subst("$PIOENV") != EXPECTED_ENV:
    fail("script is permitted only for the att firmware environment")

package_dir_value = env.PioPlatform().get_package_dir(PACKAGE_NAME)
if not package_dir_value:
    fail("pinned Arduino framework package is not installed")
package_dir = Path(package_dir_value)

try:
    package_metadata = json.loads((package_dir / "package.json").read_text(encoding="utf-8"))
except (OSError, ValueError) as error:
    fail("cannot read framework package metadata: " + str(error))

if package_metadata.get("name") != PACKAGE_NAME:
    fail("framework package name does not match the expected package")

actual_package_version = package_metadata.get("version")
actual_webserver_version = property_version(
    package_dir / "libraries" / "WebServer" / "library.properties"
)
if actual_webserver_version != WEBSERVER_VERSION:
    fail(
        "bundled WebServer library version mismatch: expected "
        + WEBSERVER_VERSION + ", found " + str(actual_webserver_version)
    )

parsing_path = package_dir / "libraries" / "WebServer" / "src" / "Parsing.cpp"
backup_path = parsing_path.with_name("Parsing.cpp.att-original")
lock_path = parsing_path.with_name("Parsing.cpp.att-lock")
source_mode = stat.S_IMODE(parsing_path.stat().st_mode)
lock_owner = acquire_build_lock(lock_path)
atexit.register(release_build_lock, lock_path, lock_owner)

replacements = (
    (
        b'static const char filename[] PROGMEM = "filename";\n',
        b'static const char filename[] PROGMEM = "filename";\n\n'
        b'// Raw callbacks must bound request bodies before any form parser can allocate.\n'
        b'static const size_t kWebServerMaxRawBodyLen = 768;\n',
    ),
    (
        b'    bool isForm = false;\n    bool isEncoded = false;\n',
        b'    bool isForm = false;\n    bool isEncoded = false;\n'
        b'    bool contentLengthValid = true;\n'
        b'    size_t contentLengthHeaderCount = 0;\n'
        b'    size_t contentTypeHeaderCount = 0;\n'
        b'    size_t transferEncodingHeaderCount = 0;\n',
    ),
    (
        b'      if (headerName.equalsIgnoreCase(FPSTR(Content_Type))) {',
        b'      if (headerName.equalsIgnoreCase(FPSTR(Content_Type))) {\n        ++contentTypeHeaderCount;',
    ),
    (
        b'''      } else if (headerName.equalsIgnoreCase(F("Content-Length"))) {
        _clientContentLength = headerValue.toInt();
      } else if (headerName.equalsIgnoreCase(F("Host"))) {''',
        b'''      } else if (headerName.equalsIgnoreCase(F("Content-Length"))) {
        ++contentLengthHeaderCount;
        if (headerValue.length() == 0) {
          contentLengthValid = false;
        } else {
          unsigned long parsedLength = 0;
          for (size_t i = 0; i < headerValue.length(); ++i) {
            const char digitChar = headerValue[i];
            if (digitChar < '0' || digitChar > '9') {
              contentLengthValid = false;
              break;
            }
            const unsigned long digit =
                static_cast<unsigned long>(digitChar - '0');
            if (parsedLength > (2147483647UL - digit) / 10UL) {
              contentLengthValid = false;
              break;
            }
            parsedLength = parsedLength * 10UL + digit;
          }
          if (contentLengthValid)
            _clientContentLength = static_cast<int>(parsedLength);
        }
      } else if (headerName.equalsIgnoreCase(F("Transfer-Encoding"))) {
        ++transferEncodingHeaderCount;
      } else if (headerName.equalsIgnoreCase(F("Host"))) {''',
    ),
    (
        b'    if (!isForm && _currentHandler && _currentHandler->canRaw(*this, _currentUri)) {',
        b'''    if (_currentHandler == nullptr) {
      sendErrorResponse(client, "405 Method Not Allowed");
      return false;
    }
    if (!contentLengthValid || contentLengthHeaderCount > 1 ||
        contentTypeHeaderCount > 1 || transferEncodingHeaderCount > 0) {
      sendErrorResponse(client, "400 Bad Request");
      return false;
    }

    // Reject multipart before the stock parser can allocate form-field Strings.
    if (isForm) {
      sendErrorResponse(client, "415 Unsupported Media Type");
      return false;
    }

    // Bodyless function routes (for example, authenticated enrollment controls)
    // do not implement canRaw(). Preserve them, but never let them accept a body.
    if (!_currentHandler->canRaw(*this, _currentUri) &&
        _clientContentLength > 0) {
      sendErrorResponse(client, "405 Method Not Allowed");
      return false;
    }

    if (_currentHandler->canRaw(*this, _currentUri)) {''',
    ),
    (
        b'''      while (_currentRaw->totalSize < (size_t)_clientContentLength) {
        size_t read_len = std::min((size_t)_clientContentLength - _currentRaw->totalSize, (size_t)HTTP_RAW_BUFLEN);
        _currentRaw->currentSize = client.readBytes(_currentRaw->buf, read_len);
        _currentRaw->totalSize += _currentRaw->currentSize;
        if (_currentRaw->currentSize == 0) {
          _currentRaw->status = RAW_ABORTED;
          _currentHandler->raw(*this, _currentUri, *_currentRaw);
          return false;
        }
        _currentHandler->raw(*this, _currentUri, *_currentRaw);
      }''',
        b'''      const unsigned long rawBodyStart = millis();
      if (_clientContentLength < 0) {
        _currentRaw->status = RAW_ABORTED;
        _currentHandler->raw(*this, _currentUri, *_currentRaw);
        sendErrorResponse(client, "400 Bad Request");
        return false;
      }
      if (static_cast<size_t>(_clientContentLength) >
          kWebServerMaxRawBodyLen) {
        _currentRaw->status = RAW_ABORTED;
        _currentHandler->raw(*this, _currentUri, *_currentRaw);
        sendErrorResponse(client, "413 Content Too Large");
        return false;
      }
      const size_t requestBodyLength =
          static_cast<size_t>(_clientContentLength);
      while (_currentRaw->totalSize < requestBodyLength) {
        if (static_cast<unsigned long>(millis() - rawBodyStart) >=
            HTTP_MAX_POST_WAIT) {
          _currentRaw->status = RAW_ABORTED;
          _currentHandler->raw(*this, _currentUri, *_currentRaw);
          if (client.connected())
            sendErrorResponse(client, "408 Request Timeout");
          return false;
        }
        const int available = client.available();
        if (available <= 0) {
          if (!client.connected()) {
            _currentRaw->status = RAW_ABORTED;
            _currentHandler->raw(*this, _currentUri, *_currentRaw);
            return false;
          }
          delay(1);
          continue;
        }
        const size_t remaining =
            requestBodyLength - _currentRaw->totalSize;
        const size_t read_len = std::min(
            std::min(static_cast<size_t>(available), remaining),
            static_cast<size_t>(HTTP_RAW_BUFLEN));
        const int bytesRead = client.read(_currentRaw->buf, read_len);
        _currentRaw->currentSize = bytesRead > 0
            ? static_cast<size_t>(bytesRead)
            : 0;
        _currentRaw->totalSize += _currentRaw->currentSize;
        if (_currentRaw->currentSize == 0) {
          _currentRaw->status = RAW_ABORTED;
          _currentHandler->raw(*this, _currentUri, *_currentRaw);
          if (client.connected())
            sendErrorResponse(client, "408 Request Timeout");
          return false;
        }
        _currentHandler->raw(*this, _currentUri, *_currentRaw);
      }''',
    ),
)

def apply_patch(original):
    patched_source = original
    for old, new in replacements:
        if patched_source.count(old) != 1:
            fail("a pinned parser patch anchor is missing or duplicated")
        patched_source = patched_source.replace(old, new)
    return patched_source


# Recover a source left patched by any earlier revision of this build script.
# The backup is accepted only when its bytes match the exact pinned pristine
# parser. Do not require the current file to match today's transformed output:
# an older patch recipe may have produced a different after-image, while this
# verified backup still provides an unambiguous and safe recovery source.
if backup_path.exists():
    backup_bytes = backup_path.read_bytes()
    backup_hash = sha256(backup_bytes)
    if backup_hash != PARSING_BEFORE_SHA256:
        fail(
            "retained framework backup does not match the pinned original: "
            + backup_hash
        )
    current_bytes = parsing_path.read_bytes()
    current_hash = sha256(current_bytes)
    if current_hash != PARSING_BEFORE_SHA256:
        print(
            "ATT WebServer patch warning: restoring Parsing.cpp from its "
            "verified pinned backup (current SHA-256 " + current_hash + ")"
        )
        atomic_write(parsing_path, backup_bytes, source_mode)
        if sha256(parsing_path.read_bytes()) != PARSING_BEFORE_SHA256:
            fail("recovered framework source failed its SHA-256 read-back")
    backup_path.unlink()

source = parsing_path.read_bytes()
source_hash = sha256(source)
if source_hash != PARSING_BEFORE_SHA256:
    fail(
        "Parsing.cpp source fingerprint mismatch: expected "
        + PARSING_BEFORE_SHA256 + ", found " + source_hash
        + " (framework package version " + str(actual_package_version)
        + "). Refusing to patch an unreviewed parser."
    )

# PlatformIO 8.0.0 declares a compatible framework package range rather than
# one exact package-build stamp. Different registry rebuild stamps are safe
# only when the WebServer library version AND the pristine parser bytes are
# exactly the reviewed versions above.
if actual_package_version != REVIEWED_PACKAGE_VERSION:
    print(
        "ATT WebServer patch: framework package stamp differs from reviewed "
        + REVIEWED_PACKAGE_VERSION + " (found " + str(actual_package_version)
        + "); exact WebServer 3.3.12 and Parsing.cpp SHA-256 fingerprints "
        "match, so continuing."
    )

patched = apply_patch(source)
patched_hash = sha256(patched)
atomic_write(backup_path, source, 0o600)


def restore_framework_source():
    if not backup_path.exists():
        return
    try:
        current_hash = sha256(parsing_path.read_bytes())
        if current_hash == patched_hash:
            atomic_write(parsing_path, source, source_mode)
        elif current_hash != PARSING_BEFORE_SHA256:
            print("ATT WebServer patch warning: source changed during build; retaining the original backup")
            return
        backup_path.unlink()
        print("ATT WebServer patch: original framework source restored")
    except OSError as error:
        print("ATT WebServer patch warning: restore failed: " + str(error))


atexit.register(restore_framework_source)
atomic_write(parsing_path, patched, source_mode)
if sha256(parsing_path.read_bytes()) != patched_hash:
    fail("patched framework source failed its SHA-256 read-back")

print("ATT WebServer patch: bounded raw-body parser enabled (768 bytes / 5000 ms); bodyless function routes preserved")
