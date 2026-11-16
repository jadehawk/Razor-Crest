"""Compile the actual LCP path helpers against a host SD-card stub.

No firmware toolchain or device is required. Keeping the production helper
bodies in this harness catches C++ syntax and collision/archive regressions.
"""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


def function_body(path: str, signature: str) -> str:
    source = (ROOT / path).read_text(encoding="utf-8")
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for pos in range(opening, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[start : pos + 1]
    raise AssertionError(f"Could not extract {signature}")


class LcpSdAcquisitionTests(unittest.TestCase):
    def test_name_and_archive_policy_using_real_cpp_functions(self):
        clang = shutil.which("clang++") or r"C:\Program Files\LLVM\bin\clang++.exe"
        if not Path(clang).is_file() and not shutil.which(clang):
            self.skipTest("Host clang++ is not installed")

        title_fn = function_body(
            "src/activities/home/FileBrowserActivity.cpp",
            "std::string lcpTitledDestination(",
        )
        archive_fn = function_body(
            "src/activities/reader/EpubReaderActivity.cpp",
            "bool archiveOpenedLcpLicense(",
        )
        source = r"""
#include <cassert>
#include <cctype>
#include <cstring>
#include <string>
#include <unordered_set>

namespace FsHelpers {
void sanitizePathComponentForFat32(const char* input, char* output, size_t maxLen) {
  if (!maxLen) return;
  size_t i = 0;
  for (; input[i] && i < maxLen - 1; ++i) {
    char c = input[i];
    output[i] = (c == '\\' || c == '/' || c == ':' || c == '*' ||
      c == '?' || c == '"' || c == '<' || c == '>' || c == '|' ||
      c == ' ' || (c > 0 && c <= 31)) ? '-' : c;
  }
  output[i] = '\0';
}
bool checkFileExtension(const std::string& path, const char* ext) {
  const std::string suffix(ext);
  if (path.size() < suffix.size()) return false;
  for (size_t i = 0; i < suffix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(path[path.size() - suffix.size() + i]))
        != suffix[i]) return false;
  }
  return true;
}
}

struct FakeStorage {
  std::unordered_set<std::string> files;
  std::unordered_set<std::string> dirs;
  bool allowMkdir = true;
  bool allowRename = true;
  bool exists(const char* path) {
    return files.count(path) || dirs.count(path);
  }
  bool mkdir(const char* path, bool) {
    if (!allowMkdir) return false;
    dirs.insert(path);
    return true;
  }
  bool rename(const char* src, const char* dst) {
    if (!allowRename || !files.count(src) || exists(dst)) return false;
    files.erase(src);
    files.insert(dst);
    return true;
  }
} Storage;

#define LOG_INF(...) ((void)0)
#define LOG_ERR(...) ((void)0)
constexpr char LCP_PROCESSED_DIR[] = "/.crosspoint/lcp/processed";
""" + title_fn + "\n" + archive_fn + r"""

int main() {
  const std::string originalEpub = "/books/9781250424853.epub";
  assert(lcpTitledDestination(originalEpub, "") == "");
  assert(lcpTitledDestination(originalEpub, "...") == "");
  assert(lcpTitledDestination(originalEpub, "The Real Book")
      == "/books/The-Real-Book.epub");
  assert(lcpTitledDestination(originalEpub, "A/B:C")
      == "/books/A-B-C.epub");

  Storage.files.insert("/books/The-Real-Book.epub");
  assert(lcpTitledDestination(originalEpub, "The Real Book")
      == "/books/The-Real-Book_1.epub");
  Storage.files.insert("/books/The-Real-Book_1.epub.key");
  assert(lcpTitledDestination(originalEpub, "The Real Book")
      == "/books/The-Real-Book_2.epub");

  const std::string originalLcpl = "/books/9781250424853.lcpl";
  const std::string archive = "/.crosspoint/lcp/processed/";
  assert(!archiveOpenedLcpLicense(originalLcpl));  // Missing source is never invented.
  Storage.files.insert(originalLcpl);
  Storage.allowMkdir = false;
  assert(!archiveOpenedLcpLicense(originalLcpl));
  assert(Storage.files.count(originalLcpl));

  Storage.allowMkdir = true;
  Storage.allowRename = false;
  assert(!archiveOpenedLcpLicense(originalLcpl));
  assert(Storage.files.count(originalLcpl));

  Storage.allowRename = true;
  assert(archiveOpenedLcpLicense(originalLcpl));
  assert(!Storage.files.count(originalLcpl));
  assert(Storage.files.count(archive + "9781250424853.lcpl"));

  Storage.files.insert(originalLcpl);
  assert(archiveOpenedLcpLicense(originalLcpl));
  assert(Storage.files.count(archive + "9781250424853_1.lcpl"));
  assert(!Storage.files.count(originalLcpl));

  assert(archiveOpenedLcpLicense(archive + "9781250424853.lcpl"));
  assert(!archiveOpenedLcpLicense("/books/wrong.epub"));
  return 0;
}
"""

        # The archive must be queued by the reader, after a page is rendered.
        reader = (ROOT / "src/activities/reader/EpubReaderActivity.cpp").read_text(encoding="utf-8")
        self.assertIn("rememberBookOnceRendered();", reader)
        self.assertIn("if (bookRemembered && !acquiredLcplPath.empty())", reader)
        self.assertIn("archiveOpenedLcpLicense(acquiredLcplPath)", reader)

        with tempfile.TemporaryDirectory(prefix="lcp-sd-host-") as tmp:
            program = Path(tmp) / "lcp_paths.cpp"
            executable = Path(tmp) / ("lcp_paths.exe" if __import__("os").name == "nt" else "lcp_paths")
            program.write_text(source, encoding="utf-8")
            build = subprocess.run(
                [clang, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 str(program), "-o", str(executable)],
                capture_output=True, text=True,
            )
            self.assertEqual(build.returncode, 0, build.stdout + "\n" + build.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + "\n" + run.stderr)


if __name__ == "__main__":
    unittest.main()
