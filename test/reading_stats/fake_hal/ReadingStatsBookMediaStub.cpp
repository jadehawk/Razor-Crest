#include <cstdint>

// The SD statistics store only asks the media cache to remove book previews.
namespace readingstats::media {
void remove(uint64_t) {}
}  // namespace readingstats::media
