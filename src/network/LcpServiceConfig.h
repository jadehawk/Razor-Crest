#pragma once

#ifndef RAZOR_CREST_LCP_BASE_URL
#define RAZOR_CREST_LCP_BASE_URL "https://lcp.techy-notes.com"
#endif

#ifndef RAZOR_CREST_LCP_RELEASE_KEY
#define RAZOR_CREST_LCP_RELEASE_KEY ""
#endif

namespace lcpservice {

inline constexpr const char* ENROLL_URL = RAZOR_CREST_LCP_BASE_URL "/enroll";
inline constexpr const char* FULFILL_URL = RAZOR_CREST_LCP_BASE_URL "/fulfill";
inline constexpr const char* UNLOCK_URL = RAZOR_CREST_LCP_BASE_URL "/unlock";
inline constexpr char RELEASE_KEY[] = RAZOR_CREST_LCP_RELEASE_KEY;

}  // namespace lcpservice
