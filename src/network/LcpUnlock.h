#pragma once

#include <LcpLicense.h>

#include <string>

namespace lcpunlock {

enum class Result { Ok, WrongPassphrase, NotEnrollable, Unsupported, Failed };

// Only the existing, device-enrolled /unlock endpoint may provision a book key.
// Raw publication keys never pass through Send2ereader.
Result unlock(const std::string& epubPath, const std::string& licenseJson, const std::string& userKeyHex);

// Read and validate a bounded license embedded in an encrypted EPUB.
bool embeddedLicense(const std::string& epubPath, std::string& licenseJson, freeink::content::LcpLicense& parsed);

// Read and validate an LCPL copied directly onto the SD card.
bool licenseFile(const std::string& path, std::string& licenseJson, freeink::content::LcpLicense& parsed);

// SHA-256(passphrase), as required by the LCP user-key protocol.
std::string userKey(const std::string& passphrase);

}  // namespace lcpunlock
