#include <cassert>
#include <cstdint>
#include <cstring>

#include "../../src/util/ReadingStatsBookIndex.h"

int main() {
  readingstats::BookEntry book{};
  book.setHash(0x1020304050607080ULL);
  strcpy(book.path, "/Books/Test.epub");
  strcpy(book.title, "Test Book");
  strcpy(book.author, "Test Author");
  book.seal();
  assert(book.valid());
  assert(book.hash() == 0x1020304050607080ULL);
  book.title[2] ^= 1;
  assert(!book.valid());
  book.title[2] ^= 1;
  book.seal();
  assert(book.valid());
  book.path[sizeof(book.path) - 1] = 'x';
  assert(!book.valid());
}
