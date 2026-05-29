#pragma once

// Font Awesome 5/6 Free Icons
// These are UTF-8 encoded Unicode code points for FontAwesome icons
// Note: Requires FontAwesome font to be loaded in ImGui for icons to render
// If FontAwesome is not loaded, these will appear as text fallbacks

// Icon definitions - using UTF-8 encoded codepoints
// If FontAwesome font is not available, define as text fallbacks
#ifndef ICON_FA_LAYER_GROUP
#define ICON_FA_LAYER_GROUP     "\xef\x97\xbd"   // U+F5FD
#endif

#ifndef ICON_FA_SYNC
#define ICON_FA_SYNC            "\xef\x80\xa1"   // U+F021
#endif

#ifndef ICON_FA_SYNC_ALT
#define ICON_FA_SYNC_ALT        "\xef\x8b\xb1"   // U+F2F1
#endif

#ifndef ICON_FA_THERMOMETER_HALF
#define ICON_FA_THERMOMETER_HALF "\xef\x8a\x89"  // U+F2C9
#endif

#ifndef ICON_FA_TIMES
#define ICON_FA_TIMES           "\xef\x80\x8d"   // U+F00D
#endif

#ifndef ICON_FA_PLUS
#define ICON_FA_PLUS            "\xef\x81\xa7"   // U+F067
#endif

#ifndef ICON_FA_LIST
#define ICON_FA_LIST            "\xef\x80\xba"   // U+F03A
#endif

#ifndef ICON_FA_ARROW_UP
#define ICON_FA_ARROW_UP        "\xef\x81\xa2"   // U+F062
#endif

#ifndef ICON_FA_ARROW_DOWN
#define ICON_FA_ARROW_DOWN      "\xef\x81\xa3"   // U+F063
#endif

#ifndef ICON_FA_COG
#define ICON_FA_COG             "\xef\x80\x93"   // U+F013
#endif

#ifndef ICON_FA_CUBE
#define ICON_FA_CUBE            "\xef\x86\xb2"   // U+F1B2
#endif

#ifndef ICON_FA_FOLDER_OPEN
#define ICON_FA_FOLDER_OPEN     "\xef\x81\xbc"   // U+F07C
#endif

#ifndef ICON_FA_COPY
#define ICON_FA_COPY            "\xef\x83\x85"   // U+F0C5
#endif

#ifndef ICON_FA_TRASH
#define ICON_FA_TRASH           "\xef\x87\xb8"   // U+F1F8
#endif

#ifndef ICON_FA_CHECK
#define ICON_FA_CHECK           "\xef\x80\x8c"   // U+F00C
#endif

#ifndef ICON_FA_EXCLAMATION_TRIANGLE
#define ICON_FA_EXCLAMATION_TRIANGLE "\xef\x81\xb1" // U+F071
#endif

#ifndef ICON_FA_CHART_BAR
#define ICON_FA_CHART_BAR       "\xef\x82\x80"   // U+F080
#endif

#ifndef ICON_FA_EYE
#define ICON_FA_EYE             "\xef\x81\xae"   // U+F06E
#endif

#ifndef ICON_FA_EYE_SLASH
#define ICON_FA_EYE_SLASH       "\xef\x81\xb0"   // U+F070
#endif

#ifndef ICON_FA_FILTER
#define ICON_FA_FILTER          "\xef\x82\xb0"   // U+F0B0
#endif

#ifndef ICON_FA_MOUNTAIN
#define ICON_FA_MOUNTAIN        "\xef\x9b\xbc"   // U+F6FC
#endif

#ifndef ICON_FA_SUN
#define ICON_FA_SUN             "\xef\x86\x85"   // U+F185
#endif

#ifndef ICON_FA_ROAD
#define ICON_FA_ROAD            "\xef\x80\x98"   // U+F018
#endif

#ifndef ICON_FA_MAP_MARKER
#define ICON_FA_MAP_MARKER      "\xef\x82\x81"   // U+F041
#endif

#ifndef ICON_FA_CLOUD
#define ICON_FA_CLOUD           "\xef\x83\x82"   // U+F0C2
#endif

#ifndef ICON_FA_RANDOM
#define ICON_FA_RANDOM          "\xef\x81\xb4"   // U+F074
#endif

#ifndef ICON_FA_PAINT_BRUSH
#define ICON_FA_PAINT_BRUSH     "\xef\x87\xbc"   // U+F1FC
#endif

#ifndef ICON_FA_HAND_POINTER
#define ICON_FA_HAND_POINTER    "\xef\x89\x9a"   // U+F25A
#endif

// Note: If FontAwesome font is not loaded, you may want to use text fallbacks instead:
// Uncomment these lines to use text fallbacks instead of Unicode icons
/*
#undef ICON_FA_LAYER_GROUP
#undef ICON_FA_SYNC
#undef ICON_FA_SYNC_ALT
#undef ICON_FA_THERMOMETER_HALF
#undef ICON_FA_TIMES
#undef ICON_FA_PLUS
#undef ICON_FA_LIST
#undef ICON_FA_ARROW_UP
#undef ICON_FA_ARROW_DOWN
#undef ICON_FA_COG
#undef ICON_FA_CUBE
#undef ICON_FA_FOLDER_OPEN
#undef ICON_FA_COPY
#undef ICON_FA_TRASH
#undef ICON_FA_CHECK
#undef ICON_FA_EXCLAMATION_TRIANGLE
#undef ICON_FA_CHART_BAR
#undef ICON_FA_EYE
#undef ICON_FA_EYE_SLASH
#undef ICON_FA_FILTER
#undef ICON_FA_MOUNTAIN
#undef ICON_FA_SUN
#undef ICON_FA_ROAD
#undef ICON_FA_MAP_MARKER
#undef ICON_FA_CLOUD
#undef ICON_FA_RANDOM
#undef ICON_FA_PAINT_BRUSH
#undef ICON_FA_HAND_POINTER

#define ICON_FA_LAYER_GROUP     "[L]"
#define ICON_FA_SYNC            "[S]"
#define ICON_FA_SYNC_ALT        "[S]"
#define ICON_FA_THERMOMETER_HALF "[T]"
#define ICON_FA_TIMES           "[x]"
#define ICON_FA_PLUS            "[+]"
#define ICON_FA_LIST            "[=]"
#define ICON_FA_ARROW_UP        "[^]"
#define ICON_FA_ARROW_DOWN      "[v]"
#define ICON_FA_COG             "[*]"
#define ICON_FA_CUBE            "[C]"
#define ICON_FA_FOLDER_OPEN     "[F]"
#define ICON_FA_COPY            "[c]"
#define ICON_FA_TRASH           "[D]"
#define ICON_FA_CHECK           "[OK]"
#define ICON_FA_EXCLAMATION_TRIANGLE "[!]"
#define ICON_FA_CHART_BAR       "[#]"
#define ICON_FA_EYE             "[O]"
#define ICON_FA_EYE_SLASH       "[-]"
#define ICON_FA_FILTER          "[f]"
#define ICON_FA_MOUNTAIN        "[M]"
#define ICON_FA_SUN             "[S]"
#define ICON_FA_ROAD            "[R]"
#define ICON_FA_MAP_MARKER      "[P]"
#define ICON_FA_CLOUD           "[C]"
#define ICON_FA_RANDOM          "[?]"
#define ICON_FA_PAINT_BRUSH     "[B]"
#define ICON_FA_HAND_POINTER    "[H]"
*/

