"""Send one EPUB to the opt-in Razor-Crest LocalSend HTTP receiver."""
import argparse
import hashlib
import http.client
import json
from pathlib import Path
from urllib.parse import urlencode

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", help="Reader IPv4 address shown in File Transfer")
    parser.add_argument("file", type=Path)
    parser.add_argument("--name", help="Destination filename; existing files are preserved")
    args = parser.parse_args()
    path = args.file
    size = path.stat().st_size
    if not path.is_file() or not 4 <= size <= 32 * 1024 * 1024:
        parser.error("Use an EPUB between 4 bytes and 32 MiB.")
    name = args.name or path.name
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(65536), b""):
            digest.update(chunk)
    info = {"alias": "Razor-Crest test sender", "version": "2.0",
            "deviceType": "desktop", "fingerprint": "test-sender", "protocol": "http", "port": 53317}
    offer = {"info": info, "files": {"epub": {"id": "epub", "fileName": name,
             "size": size, "fileType": "application/epub+zip", "sha256": digest.hexdigest()}}}
    connection = http.client.HTTPConnection(args.host, 53317, timeout=75)
    try:
        connection.request("POST", "/api/localsend/v2/prepare-upload",
                           json.dumps(offer).encode(), {"Content-Type": "application/json"})
        response = connection.getresponse()
        body = response.read()
        if response.status != 200:
            raise SystemExit(f"Preparation rejected: HTTP {response.status}. Existing filename, busy session, or invalid EPUB offer.")
        session = json.loads(body)
    finally:
        connection.close()
    query = urlencode({"sessionId": session["sessionId"], "fileId": "epub", "token": session["files"]["epub"]})
    connection = http.client.HTTPConnection(args.host, 53317, timeout=75)
    try:
        with path.open("rb") as source:
            connection.request("POST", "/api/localsend/v2/upload?" + query, source,
                               {"Content-Type": "application/octet-stream", "Content-Length": str(size)})
            response = connection.getresponse()
            response.read()
            if response.status != 200:
                raise SystemExit(f"Upload failed: HTTP {response.status}.")
    finally:
        connection.close()
    print(f"PASS: LocalSend uploaded {name} ({size} bytes); SHA-256 checked by receiver.")
    print(f"SD path: /Books/LocalSend/{name}")

if __name__ == "__main__":
    main()
