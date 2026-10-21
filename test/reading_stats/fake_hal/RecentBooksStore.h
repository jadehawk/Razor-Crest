#pragma once
#include <string>
#include <vector>
struct RecentBook {
  std::string path;
  std::string title;
  std::string author;
};
struct FakeRecentBooks {
  std::vector<RecentBook> entries;
  const std::vector<RecentBook>& getBooks() const { return entries; }
};
extern FakeRecentBooks RECENT_BOOKS;
